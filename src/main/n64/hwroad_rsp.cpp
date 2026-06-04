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
bool     enabled = true;   // TEMP: default to RSP path for first-light test

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
    void*      desc_cached = nullptr;   // cached alias (for memalign / free)
    Descriptor* desc_uc    = nullptr;   // KSEG1 alias used for writes
    bool       initialised = false;
    bool       roads_flushed = false;

    constexpr uint8_t CTRL_SKIP = 0x80;
}

void init()
{
    if (initialised)
        return;

    overlay_id = rspq_overlay_register(&rsp_hwroad);

    desc_cached = memalign(16, DESC_BUF_SIZE);
    assertf(desc_cached != nullptr, "hwroad_rsp: failed to alloc descriptor buffer");
    desc_uc = (Descriptor*)UncachedAddr(desc_cached);
    std::memset(desc_uc, 0, DESC_BUF_SIZE);

    initialised = true;
}

void shutdown()
{
    if (!initialised)
        return;
    rspq_overlay_unregister(overlay_id);
    overlay_id = 0;
    free(desc_cached);
    desc_cached = nullptr;
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

    // The roads[] texture data is essentially read-only after decode_road().
    // The RSP DMAs from RDRAM, so we need to make sure no dirty CPU cache
    // lines are hiding a fresh value. Do it once.
    if (!roads_flushed)
    {
        data_cache_hit_writeback_invalidate(roads, sizeof(roads));
        roads_flushed = true;
    }

    const uint16_t* roadram = ramBuff;
    const int32_t   control = road_control & 3;
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

    int active_lines = 0;
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
            continue;
        }

        // Case-specific early skip: case 0 only draws road 0, case 3 only
        // draws road 1.
        if (control == 0 && (data0 & 0x800))
        {
            d->ctrl |= CTRL_SKIP;
            continue;
        }
        if (control == 3 && (data1 & 0x800))
        {
            d->ctrl |= CTRL_SKIP;
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

        active_lines++;
    }

    (void)active_lines;

    // dst_rgba lives in KSEG1 (scratch_uc_ptr). PhysicalAddr handles that.
    uint32_t desc_phys    = PhysicalAddr(desc_cached);
    uint32_t scratch_phys = PhysicalAddr(dst_rgba);

    static int debug_once = 0;
    if (debug_once++ < 3)
    {
        debugf("hwroad_rsp: ovl_id=0x%lx desc_phys=0x%lx scratch_phys=0x%lx N=%d\n",
               (unsigned long)overlay_id,
               (unsigned long)desc_phys,
               (unsigned long)scratch_phys,
               (int)S16_HEIGHT);
        debugf("hwroad_rsp: desc_cached=%p desc_uc=%p dst_rgba=%p\n",
               desc_cached, desc_uc, dst_rgba);
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
}
