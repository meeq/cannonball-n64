/***************************************************************************
    CPU-side dispatch for the hwroad_rsp overlay.

    Each frame, the CPU walks the 224 scanlines of road RAM, builds a packed
    descriptor (64 bytes per line — see hwroad_rsp.S for the on-wire format),
    and kicks the overlay with a single rspq_write. The RSP then DMAs each
    descriptor + source rows in, rasterises into a 640-byte scratch row, and
    DMAs the result out to the engine scratch surface.

    Symmetric with the CPU path in HWRoad::render_foreground_lores; refer to
    that function for the per-scanline logic this descriptor encodes.
***************************************************************************/

#include "n64/hwroad_rsp.hpp"
#include "n64/platform.hpp"
#include "hwvideo/hwroad.hpp"
#include "frontend/config.hpp"

#include <libdragon.h>
#include <malloc.h>
#include <cstddef>
#include <cstring>

// The RSP overlay's ucode symbol lives in the global namespace. We declare
// it under a different identifier so it doesn't clash with the
// `n64::hwroad_rsp` namespace below — same trick libdragon uses with
// `rsp_mixer` etc.
DEFINE_RSP_UCODE(rsp_hwroad);

namespace n64
{
namespace hwroad_rsp
{

uint32_t last_us = 0;
bool     enabled = false;  // CPU rasteriser wins by ~45% — see PR notes
uint8_t  rsp_case_mask = 0x0F;  // all four cases on RSP by default
bool     validate = false; // diff against CPU reference each frame (dev aid)
uint32_t v_mismatches = 0;
int      v_first_y    = -1;
int      v_first_x    = -1;

uint32_t case_scanlines[4] = { 0, 0, 0, 0 };
uint32_t case_skipped  [4] = { 0, 0, 0, 0 };
uint32_t case_total    [4] = { 0, 0, 0, 0 };

namespace
{
    constexpr int   MAX_SCANLINES = S16_HEIGHT;     // 224
    constexpr int   DESC_BYTES    = 64;
    constexpr int   DESC_BUF_SIZE = MAX_SCANLINES * DESC_BYTES;

    // Descriptor layout — must match rsp_hwroad.S. All fields land at their
    // natural alignment so no `packed` attr is needed (and using `packed`
    // would force aligned(1) on the u16/u32 fields and trip the toolchain's
    // -Waddress-of-packed-member when we hand color_table to inner loops).
    struct alignas(16) Descriptor
    {
        uint8_t  y;
        uint8_t  ctrl;          // bits[0:1] = case, bit7 = skip
        uint16_t hpos0;
        uint16_t hpos1;
        uint16_t _pad0;
        uint32_t src0_phys;
        uint32_t src1_phys;
        uint16_t color_table[16]; // [0..7] road 0, [8..15] road 1
        uint16_t _pad1[8];        // pad to 64 bytes
    };
    static_assert(sizeof(Descriptor) == 64, "Descriptor must be 64 bytes");
    static_assert(offsetof(Descriptor, hpos0)       == 0x02, "");
    static_assert(offsetof(Descriptor, hpos1)       == 0x04, "");
    static_assert(offsetof(Descriptor, src0_phys)   == 0x08, "");
    static_assert(offsetof(Descriptor, src1_phys)   == 0x0c, "");
    static_assert(offsetof(Descriptor, color_table) == 0x10, "");

    uint32_t   overlay_id = 0;
    Descriptor* desc_uc   = nullptr;    // KSEG1 alias; allocated uncached so
                                        // no cache-line aliasing with the RSP
                                        // DMA reader. Free via free_uncached.
    bool       initialised = false;
    bool       roads_flushed = false;

    // Validation shadow buffer — same dimensions as the engine scratch
    // surface (320×224 RGBA5551 = 143360 bytes). Allocated lazily the first
    // time validate is true.
    constexpr int SHADOW_BYTES = S16_WIDTH * S16_HEIGHT * 2;
    uint16_t* shadow_uc     = nullptr;

    constexpr uint8_t CTRL_SKIP = 0x80;
}

void init()
{
    if (initialised)
        return;

    overlay_id = rspq_overlay_register(&rsp_hwroad);

    desc_uc = (Descriptor*)malloc_uncached_aligned(16, DESC_BUF_SIZE);
    assertf(desc_uc != nullptr, "hwroad_rsp: failed to alloc descriptor buffer");
    std::memset(desc_uc, 0, DESC_BUF_SIZE);

    initialised = true;
}

void shutdown()
{
    if (!initialised)
        return;
    rspq_overlay_unregister(overlay_id);
    overlay_id = 0;
    free_uncached(desc_uc);
    desc_uc = nullptr;
    initialised = false;
}

} // namespace hwroad_rsp
} // namespace n64

// ---------------------------------------------------------------------------
// HWRoad::render_foreground_lores_rsp
//
// Mirrors the CPU render_foreground_lores in hwroad.cpp — see that function
// for the source-of-truth logic. The only differences:
//   * Per-scanline state (hpos0/1, color_table[0..7]+[10..17], src ptrs) is
//     packed into a 64-byte descriptor and consumed by the RSP, not run
//     inline on the R4300.
//   * Whole-scanline "both roads low priority" and case-0/3 "this road low
//     priority" skips set CTRL_SKIP — the RSP fast-path skips those lines
//     entirely (no DMA, no fill). Pixels remain whatever start_frame zeroed
//     them to, matching the CPU contract.
// ---------------------------------------------------------------------------
void HWRoad::render_foreground_lores_rsp(uint16_t* dst_rgba, const uint16_t* rgb_lut)
{
    using namespace n64::hwroad_rsp;
    assertf(initialised, "hwroad_rsp::init() not called");

    const int32_t control = road_control & 3;

    // Per-case selector: fall back to the CPU rasteriser when this case's
    // bit is clear in rsp_case_mask. Lets the user A/B individual cases at
    // runtime without rebuilding. Counts below stay zero for fall-through
    // frames so RSP-side stats reflect actual RSP usage.
    if (!((rsp_case_mask >> control) & 1))
    {
        for (int i = 0; i < 4; i++)
        {
            n64::hwroad_rsp::case_scanlines[i] = 0;
            n64::hwroad_rsp::case_skipped  [i] = 0;
            n64::hwroad_rsp::case_total    [i] = 0;
        }
        render_foreground_lores(dst_rgba, rgb_lut);
        return;
    }

    // The roads[] texture data is essentially read-only after decode_road().
    // The RSP DMAs from RDRAM, so we need to make sure no dirty CPU cache
    // lines are hiding a fresh value. Do it once.
    if (!roads_flushed)
    {
        data_cache_hit_writeback_invalidate(roads, sizeof(roads));
        roads_flushed = true;
    }

    const uint16_t* roadram = ramBuff;
    const uint16_t  s16_x   = 0x5f8 + config.s16_x_off;
    const int32_t   xoff_total = s16_x + x_offset;

    const uint8_t* dummy_row = roads + 256 * 2 * 512;
    uint32_t dummy_phys = PhysicalAddr((void*)dummy_row);

    static const uint8_t priority_map[2][8] =
    {
        { 0x80,0x81,0x81,0x87,0,0,0,0x00 },
        { 0x81,0x81,0x81,0x8f,0,0,0,0x80 }
    };
    (void)priority_map; // RSP applies the priority maps internally

    uint32_t local_active  = 0;
    uint32_t local_skipped = 0;
    for (int y = 0; y < S16_HEIGHT; y++)
    {
        Descriptor* d = &desc_uc[y];
        d->y    = (uint8_t)y;
        d->ctrl = (uint8_t)control;

        const uint32_t data0 = roadram[0x000 + y];
        const uint32_t data1 = roadram[0x100 + y];

        // Both roads low priority → skip
        if (((data0 & 0x800) != 0) && ((data1 & 0x800) != 0))
        {
            d->ctrl |= CTRL_SKIP;
            local_skipped++;
            continue;
        }

        // Case-specific early skip: case 0 only draws road 0, case 3 only
        // draws road 1.
        if (control == 0 && (data0 & 0x800))
        {
            d->ctrl |= CTRL_SKIP;
            local_skipped++;
            continue;
        }
        if (control == 3 && (data1 & 0x800))
        {
            d->ctrl |= CTRL_SKIP;
            local_skipped++;
            continue;
        }

        const uint8_t* src0 = ((data0 & 0x800) != 0)
            ? dummy_row
            : (roads + (0x000 + ((data0 >> 1) & 0xff)) * 512);
        const uint8_t* src1 = ((data1 & 0x800) != 0)
            ? dummy_row
            : (roads + (0x100 + ((data1 >> 1) & 0xff)) * 512);

        int32_t hpos0 = roadram[0x200 + (((road_control & 4) != 0) ? y : (data0 & 0x1ff))] & 0xfff;
        int32_t hpos1 = roadram[0x400 + (((road_control & 4) != 0) ? (0x100 + y) : (data1 & 0x1ff))] & 0xfff;
        int32_t color0 = roadram[0x600 + (((road_control & 4) != 0) ? y : (data0 & 0x1ff))];
        int32_t color1 = roadram[0x600 + (((road_control & 4) != 0) ? (0x100 + y) : (data1 & 0x1ff))];

        // Apply the s16_x + x_offset adjustment up-front (RSP just increments
        // and wraps at 0xfff).
        hpos0 = (hpos0 - xoff_total) & 0xfff;
        hpos1 = (hpos1 - xoff_total) & 0xfff;

        d->hpos0 = (uint16_t)hpos0;
        d->hpos1 = (uint16_t)hpos1;

        d->src0_phys = (src0 == dummy_row) ? dummy_phys
                                           : PhysicalAddr((void*)src0);
        d->src1_phys = (src1 == dummy_row) ? dummy_phys
                                           : PhysicalAddr((void*)src1);

        // Resolve color_table — same expressions as render_foreground_lores.
        int32_t bgcolor;
        uint16_t* ct = d->color_table;

        ct[0x00] = rgb_lut[color_offset1 ^ 0x00 ^ ((color0 >> 0) & 1)];
        ct[0x01] = rgb_lut[color_offset1 ^ 0x02 ^ ((color0 >> 1) & 1)];
        ct[0x02] = rgb_lut[color_offset1 ^ 0x04 ^ ((color0 >> 2) & 1)];
        bgcolor  = (color0 >> 8) & 0xf;
        ct[0x03] = ((data0 & 0x200) != 0) ? ct[0x00]
                                          : rgb_lut[color_offset2 ^ 0x00 ^ bgcolor];
        ct[0x07] = rgb_lut[color_offset1 ^ 0x06 ^ ((color0 >> 3) & 1)];

        ct[0x08] = rgb_lut[color_offset1 ^ 0x08 ^ ((color1 >> 4) & 1)];
        ct[0x09] = rgb_lut[color_offset1 ^ 0x0a ^ ((color1 >> 5) & 1)];
        ct[0x0a] = rgb_lut[color_offset1 ^ 0x0c ^ ((color1 >> 6) & 1)];
        bgcolor  = (color1 >> 8) & 0xf;
        ct[0x0b] = ((data1 & 0x200) != 0) ? ct[0x08]
                                          : rgb_lut[color_offset2 ^ 0x10 ^ bgcolor];
        ct[0x0f] = rgb_lut[color_offset1 ^ 0x0e ^ ((color1 >> 7) & 1)];

        local_active++;
    }

    // Publish per-case stats. Only one of the four slots is non-zero per
    // frame (control is a per-frame value), but the array shape makes it
    // easy to accumulate over sessions outside this function.
    for (int i = 0; i < 4; i++)
    {
        n64::hwroad_rsp::case_scanlines[i] = 0;
        n64::hwroad_rsp::case_skipped  [i] = 0;
        n64::hwroad_rsp::case_total    [i] = 0;
    }
    n64::hwroad_rsp::case_scanlines[control] = local_active;
    n64::hwroad_rsp::case_skipped  [control] = local_skipped;
    n64::hwroad_rsp::case_total    [control] = local_active + local_skipped;

    // dst_rgba lives in KSEG1 (scratch_uc_ptr). PhysicalAddr handles that.
    uint32_t desc_phys    = PhysicalAddr(desc_uc);
    uint32_t scratch_phys = PhysicalAddr(dst_rgba);

    static int debug_once = 0;
    if (debug_once++ < 3)
    {
        debugf("hwroad_rsp: ovl_id=0x%lx desc_phys=0x%lx scratch_phys=0x%lx N=%d\n",
               (unsigned long)overlay_id,
               (unsigned long)desc_phys,
               (unsigned long)scratch_phys,
               (int)S16_HEIGHT);
        debugf("hwroad_rsp: desc_uc=%p dst_rgba=%p\n",
               desc_uc, dst_rgba);
    }

    uint32_t t0 = TICKS_READ();
    // arg0 is OR'd with the cmd word's top byte (ovl_id | cmd_id), so it must
    // not use bits 24..31. Pass num_scanlines there (≤ 224 fits in 16 bits)
    // and put the RDRAM pointers in arg1/arg2 — mirrors the rsp_mixer pattern.
    rspq_write(overlay_id, 0,
        (uint32_t)S16_HEIGHT,
        desc_phys,
        scratch_phys);
    rspq_wait();  // block so the µs measurement reflects RSP cost
    uint32_t t1 = TICKS_READ();
    uint32_t dt_us = TIMER_MICROS(t1 - t0);

    // EMA over 8 frames so the on-screen counter is readable.
    n64::hwroad_rsp::last_us = (n64::hwroad_rsp::last_us * 7 + dt_us) >> 3;

    // ---- Validation: re-run the CPU reference into a shadow buffer and diff
    // both surfaces. Stride is S16_WIDTH; both paths assume 320 here (we
    // assert that elsewhere via the non-widescreen default).
    if (n64::hwroad_rsp::validate)
    {
        if (!n64::hwroad_rsp::shadow_uc)
        {
            n64::hwroad_rsp::shadow_uc = (uint16_t*)malloc_uncached_aligned(
                16, n64::hwroad_rsp::SHADOW_BYTES);
            assertf(n64::hwroad_rsp::shadow_uc != nullptr,
                    "hwroad_rsp: shadow alloc failed");
        }

        // Zero the shadow surface — render_foreground_lores only writes
        // pixels for non-skipped scanlines, and the engine's start_frame()
        // contract clears the real scratch each frame. Match that here.
        std::memset(n64::hwroad_rsp::shadow_uc, 0, n64::hwroad_rsp::SHADOW_BYTES);
        render_foreground_lores(n64::hwroad_rsp::shadow_uc, rgb_lut);

        uint32_t mismatches = 0;
        uint32_t per_case[4] = { 0, 0, 0, 0 };
        int first_y = -1, first_x = -1;
        int first_case = -1;
        for (int y = 0; y < S16_HEIGHT; y++)
        {
            const uint16_t* cpu = n64::hwroad_rsp::shadow_uc + y * S16_WIDTH;
            const uint16_t* rsp = dst_rgba + y * S16_WIDTH;
            const uint8_t   ctrl_byte = n64::hwroad_rsp::desc_uc[y].ctrl;
            const int       cs        = ctrl_byte & 0x03;
            for (int x = 0; x < S16_WIDTH; x++)
            {
                if (cpu[x] != rsp[x])
                {
                    if (first_y < 0) { first_y = y; first_x = x; first_case = cs; }
                    mismatches++;
                    per_case[cs]++;
                }
            }
        }

        n64::hwroad_rsp::v_mismatches = mismatches;
        n64::hwroad_rsp::v_first_y    = first_y;
        n64::hwroad_rsp::v_first_x    = first_x;

        static int v_log_throttle = 0;
        if (mismatches > 0)
        {
            if ((v_log_throttle++ % 60) == 0)
            {
                const uint16_t* cpu = n64::hwroad_rsp::shadow_uc + first_y * S16_WIDTH;
                const uint16_t* rsp = dst_rgba + first_y * S16_WIDTH;
                debugf("hwroad_rsp: VALIDATE FAIL %lu mismatches | case0=%lu case1=%lu case2=%lu case3=%lu | first(%d,%d) case=%d cpu=0x%04x rsp=0x%04x\n",
                       (unsigned long)mismatches,
                       (unsigned long)per_case[0], (unsigned long)per_case[1],
                       (unsigned long)per_case[2], (unsigned long)per_case[3],
                       first_x, first_y, first_case,
                       (unsigned)cpu[first_x], (unsigned)rsp[first_x]);

                // Dump descriptor + a window of the first mismatching scanline.
                const Descriptor& d = n64::hwroad_rsp::desc_uc[first_y];
                debugf("  desc y=%u ctrl=0x%02x hpos0=0x%04x hpos1=0x%04x src0=0x%08lx src1=0x%08lx\n",
                       d.y, d.ctrl, d.hpos0, d.hpos1,
                       (unsigned long)d.src0_phys, (unsigned long)d.src1_phys);
                debugf("  ct r0: %04x %04x %04x %04x %04x %04x %04x %04x\n",
                       d.color_table[0x0], d.color_table[0x1], d.color_table[0x2], d.color_table[0x3],
                       d.color_table[0x4], d.color_table[0x5], d.color_table[0x6], d.color_table[0x7]);
                debugf("  ct r1: %04x %04x %04x %04x %04x %04x %04x %04x\n",
                       d.color_table[0x8], d.color_table[0x9], d.color_table[0xa], d.color_table[0xb],
                       d.color_table[0xc], d.color_table[0xd], d.color_table[0xe], d.color_table[0xf]);
                int xs = first_x > 4 ? first_x - 4 : 0;
                int xe = first_x + 8;
                if (xe > S16_WIDTH) xe = S16_WIDTH;
                for (int x = xs; x < xe; x++)
                    debugf("    x=%-3d cpu=0x%04x rsp=0x%04x %s\n",
                           x, (unsigned)cpu[x], (unsigned)rsp[x],
                           cpu[x] == rsp[x] ? "" : "  <--");
            }
        }
        else if ((v_log_throttle++ % 120) == 0)
        {
            debugf("hwroad_rsp: VALIDATE OK\n");
        }
    }
}
