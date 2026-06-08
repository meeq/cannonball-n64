/***************************************************************************
    CPU-side dispatch for the hwroad_rdp_rsp overlay.

    Step 1 — overlay scaffold + CPU descriptor build + pre-fill. The RSP
    overlay is a stub: the mask region is left in the all-OOB state the
    pre-fill writes, so the visual contract when enabled==true is "road
    region paints a uniform OOB colour through the RDP". That validates
    the build/emit wiring before step 2 adds the actual scalar pack loop
    in rsp_hwroad_rdp.S.

    Per-frame protocol:

      1. CPU: same per-row math the CPU-only build does (data0/data1
         skip flags, color_idx, hpos -> span, src pointers, 16-entry
         TLUT). Writes are: hwroad_rdp::detail::tlut_buf (per-row),
         hwroad_rdp::detail::line (LineState), and a per-row entry in
         the descriptor table here.
      2. CPU: pre-fill mask row with oob_pair via 4-byte uncached stores.
         RSP only needs to overwrite the in-span nibble pairs; OOB
         segments are already correct.
      3. CPU: rspq_write(HWRoadRDP_BuildMasks, desc_base, state_base).
      4. RSP: per row, async DMA src0+src1 in, scalar pack, DMA mask
         out. Caller (emit_foreground_lores_rdp) issues rspq_wait
         before reading mask_buf via RDP DMA.
***************************************************************************/

#include "n64/hwroad_rdp.hpp"
#include "n64/hwroad_rdp_internal.hpp"
#include "n64/hwroad_rdp_rsp.hpp"
#include "n64/platform.hpp"
#include "hwvideo/hwroad.hpp"
#include "frontend/config.hpp"

#include <libdragon.h>
#include <malloc.h>
#include <cstddef>
#include <cstring>

DEFINE_RSP_UCODE(rsp_hwroad_rdp);

namespace n64
{
namespace hwroad_rdp_rsp
{

// Shipping default: RSP path on. Handles all ctrl values (0/1/2/3); ctrl=1/2
// dual-road frames are scanned piece-wise inside do_case12.
bool     enabled    = true;
uint32_t last_us    = 0;
uint32_t cpu_us     = 0;
uint32_t rsp_us     = 0;

// Toggle on to re-run the CPU build into runs_buf and diff against the
// RSP-produced runs[] / n_runs. Doubles prepare cost; off for perf.
bool     validate   = false;
uint32_t v_mismatches = 0;
int      v_first_row  = -1;
int      v_first_byte = -1;

namespace
{
    using namespace n64::hwroad_rdp::detail;

    // Per-row descriptor handed to the RSP overlay. Layout mirrors
    // rsp_hwroad_rdp.S DESC_* offsets — keep in sync. 32 bytes exact.
    //
    // The RSP overlay no longer packs a CI4 mask; it emits a Run list
    // ({x_end, color5551}). Per row it needs:
    //   * the 16-entry RGBA5551 TLUT (tlut_phys, 32B) so it can resolve
    //     slot -> colour without a frame-wide TLUT cache;
    //   * the src bytes (src0_phys / src1_phys, single-road may zero
    //     the unused one);
    //   * the runs_phys destination (writes up to 64 Run records = 256B);
    //   * c_oob_color so RSP can short-circuit oob-only segments in
    //     ctrl=1/2 without going through the scan loop.
    struct alignas(8) DescriptorRDP
    {
        uint8_t  kind;          // 0=SKIP, 1=OOB_ONLY, 2=DRAW
        uint8_t  ctrl_case;     // 0..3
        uint16_t span_start;
        uint16_t span_end;
        uint16_t s0s;
        uint16_t s0e;
        uint16_t s1s;
        uint16_t s1e;
        uint16_t c_oob_color;   // RGBA5551 = tlut[oob_slot]; for gap fill
        uint32_t tlut_phys;     // RDRAM phys of 16-entry RGBA5551 TLUT
        uint32_t src0_phys;     // RDRAM phys of src0 + t0_offset
        uint32_t src1_phys;     // RDRAM phys of src1 + t1_offset
        uint32_t runs_phys;     // RDRAM phys of runs[y][0]
    };
    static_assert(sizeof(DescriptorRDP) == 32, "DescriptorRDP must be 32 bytes");

    // Frame-level state DMA'd into DMEM once per frame. Holds slot LUTs,
    // the 64-entry merged_idx table, and the writeback addr for n_runs
    // (parallel u16[MAX_LINES] that the dispatcher copies into
    // line[y].n_runs after rspq_wait).
    struct alignas(16) FrameStateRDP
    {
        uint8_t  ctrl;
        uint8_t  oob_pair;      // (oob_slot << 4) | oob_slot — legacy, unused by runs path
        uint16_t _pad0;
        uint32_t n_runs_phys;   // RDRAM phys of u16[MAX_LINES] write-back
        uint8_t  slot8r0[8];    // merged_idx[(p<<3) | 3] for p in 0..7
        uint8_t  slot8r1[8];    // merged_idx[(3<<3) | p] for p in 0..7
        uint8_t  merged_idx[64];
        uint8_t  _pad1[40];
    };
    static_assert(sizeof(FrameStateRDP) == 128, "FrameStateRDP must be 128 bytes");

    constexpr int DESC_BUF_BYTES = MAX_LINES * (int)sizeof(DescriptorRDP);

    uint32_t       overlay_id   = 0;
    void*          desc_cached  = nullptr;
    DescriptorRDP* desc_uc      = nullptr;
    void*          state_cached = nullptr;
    FrameStateRDP* state_uc     = nullptr;

    bool initialised   = false;
    bool roads_flushed = false;

    // Shadow buf for runs validation (compare RSP-produced runs[] vs CPU).
    // sizeof(Run) == 4; MAX_RUNS_PER_ROW == 64. Cached.
    constexpr size_t SHADOW_BYTES =
        (size_t)MAX_LINES * MAX_RUNS_PER_ROW * sizeof(Run);
    void*    shadow_cached = nullptr;
    uint8_t* shadow_buf    = nullptr;

    // n_runs write-back array. RSP writes one u16 per DRAW row;
    // dispatcher copies into line[y].n_runs after rspq_wait. Sized to
    // 8-byte multiple so SP DMA's length encoding is clean.
    constexpr size_t N_RUNS_BYTES = ((size_t)MAX_LINES * 2 + 7) & ~7;
    void*     n_runs_cached = nullptr;
    uint16_t* n_runs_uc     = nullptr;

    uint32_t s_frame = 0;
}

void init()
{
    if (initialised) return;

    overlay_id = rspq_overlay_register(&rsp_hwroad_rdp);

    desc_cached  = memalign(16, DESC_BUF_BYTES);
    assertf(desc_cached, "hwroad_rdp_rsp: descriptor alloc failed");
    desc_uc      = (DescriptorRDP*)UncachedAddr(desc_cached);
    std::memset(desc_uc, 0, DESC_BUF_BYTES);

    state_cached = memalign(16, sizeof(FrameStateRDP));
    assertf(state_cached, "hwroad_rdp_rsp: state alloc failed");
    state_uc     = (FrameStateRDP*)UncachedAddr(state_cached);
    std::memset(state_uc, 0, sizeof(FrameStateRDP));

    // Shadow buffer for validate path — holds the CPU baseline runs[] while
    // we diff against the RSP output. Cached so the diff loop reads cache.
    shadow_cached = memalign(16, SHADOW_BYTES);
    assertf(shadow_cached, "hwroad_rdp_rsp: shadow alloc failed");
    shadow_buf    = (uint8_t*)shadow_cached;
    std::memset(shadow_buf, 0, SHADOW_BYTES);

    n_runs_cached = memalign(16, N_RUNS_BYTES);
    assertf(n_runs_cached, "hwroad_rdp_rsp: n_runs alloc failed");
    n_runs_uc     = (uint16_t*)UncachedAddr(n_runs_cached);
    std::memset(n_runs_uc, 0, N_RUNS_BYTES);

    state_uc->n_runs_phys = PhysicalAddr(n_runs_cached);

    initialised = true;
}

void sync_runs()
{
    using namespace n64::hwroad_rdp;
    using namespace n64::hwroad_rdp::detail;

    if (!initialised) return;
    rspq_wait();
    for (int y = 0; y < MAX_LINES; y++) {
        if (line[y].kind == DRAW)
            line[y].n_runs = n_runs_uc[y];
    }
}

void shutdown()
{
    if (!initialised) return;
    rspq_overlay_unregister(overlay_id);
    overlay_id = 0;
    free(desc_cached);   desc_cached  = nullptr; desc_uc  = nullptr;
    free(state_cached);  state_cached = nullptr; state_uc = nullptr;
    if (shadow_cached) { free(shadow_cached); shadow_cached = nullptr; shadow_buf = nullptr; }
    if (n_runs_cached) { free(n_runs_cached); n_runs_cached = nullptr; n_runs_uc = nullptr; }
    initialised = false;
}

} // namespace hwroad_rdp_rsp
} // namespace n64

// ---------------------------------------------------------------------------
// HWRoad::build_foreground_lores_rdp_rsp
//
// Same on-frame contract as build_foreground_lores_rdp: populates the
// shared mask + TLUT + LineState arrays in n64::hwroad_rdp::detail.
// Replaces the CPU mask-pack inner loop with a descriptor table consumed
// by the RSP overlay.
//
// STEP 1: the RSP overlay is a no-op. The mask is left in the all-OOB
// pre-fill state, so the road region paints a uniform OOB colour. That
// validates the dispatch wiring. The CPU TLUT + state + descriptor +
// pre-fill cost should be ~1.5-2 ms — i.e. the floor for what this path
// can ever beat (the existing CPU-only build at 13 ms minus the missing
// per-pixel pack work).
// ---------------------------------------------------------------------------
void HWRoad::build_foreground_lores_rdp_rsp(const uint16_t* rgb_lut)
{
    using namespace n64::hwroad_rdp;
    using namespace n64::hwroad_rdp::detail;
    namespace rsp = n64::hwroad_rdp_rsp;

    if (!mask_buf || !tlut_buf || !rsp::desc_uc || !rsp::state_uc) return;

    const uint8_t ctrl = road_control & 3;
    rsp::s_frame++;

    // roads[] is read-only after decode_road. Flush once so the RSP DMA
    // never races a stale cache line. The CPU path doesn't need this
    // (it reads via cached loads), so we keep it gated on first-use.
    if (!rsp::roads_flushed) {
        data_cache_hit_writeback_invalidate(roads, sizeof(roads));
        rsp::roads_flushed = true;
    }

    uint64_t t_wait0 = get_ticks_us();
    // Block on the prior frame's RSP work + RDP DMAs reading last frame's
    // mask. Same rationale as the CPU build_foreground_lores_rdp.
    rspq_wait();
    uint64_t t0 = get_ticks_us();
    const uint32_t sub_wait = (uint32_t)(t0 - t_wait0);

    uint16_t* roadram = ramBuff;
    const int W      = config.s16_width;
    const uint16_t s16_x = 0x5f8 + config.s16_x_off;

    static const uint8_t priority_map[2][8] =
    {
        { 0x80, 0x81, 0x81, 0x87, 0, 0, 0, 0x00 },
        { 0x81, 0x81, 0x81, 0x8f, 0, 0, 0, 0x80 }
    };

    // Per-frame merged index table. Same construction as the CPU path.
    uint8_t merged_idx[64];
    if (ctrl == 0) {
        for (int p0 = 0; p0 < 8; p0++)
            for (int p1 = 0; p1 < 8; p1++)
                merged_idx[(p0 << 3) | p1] = (uint8_t)p0;
    } else if (ctrl == 3) {
        for (int p0 = 0; p0 < 8; p0++)
            for (int p1 = 0; p1 < 8; p1++)
                merged_idx[(p0 << 3) | p1] = (uint8_t)(8 + p1);
    } else {
        const uint8_t* pmap_row = priority_map[ctrl - 1];
        for (int p0 = 0; p0 < 8; p0++) {
            const uint8_t row = pmap_row[p0];
            for (int p1 = 0; p1 < 8; p1++)
                merged_idx[(p0 << 3) | p1] =
                    (uint8_t)(((row >> p1) & 1) ? (8 + p1) : p0);
        }
    }

    uint8_t slot8r0[8], slot8r1[8];
    for (int p = 0; p < 8; p++) {
        slot8r0[p] = merged_idx[(p << 3) | 3];
        slot8r1[p] = merged_idx[(3 << 3) | p];
    }
    const uint8_t oob_slot = merged_idx[(3 << 3) | 3];
    const uint8_t oob_pair = (uint8_t)((oob_slot << 4) | oob_slot);

    // Frame-level state into the RSP-shared buffer (uncached).
    rsp::state_uc->ctrl     = ctrl;
    rsp::state_uc->oob_pair = oob_pair;
    for (int i = 0; i < 8; i++) {
        rsp::state_uc->slot8r0[i] = slot8r0[i];
        rsp::state_uc->slot8r1[i] = slot8r1[i];
    }
    for (int i = 0; i < 64; i++)
        rsp::state_uc->merged_idx[i] = merged_idx[i];

    auto compute_span = [](int32_t hpos, int W,
                           int& s_start, int& s_end, int& t_base)
    {
        if (hpos < 0x200) {
            s_start = 0;
            s_end   = (0x200 - hpos < W) ? (0x200 - hpos) : W;
            t_base  = hpos;
        } else {
            const int wrap = 0x1000 - hpos;
            if (wrap >= W) { s_start = W; s_end = W; t_base = 0; }
            else           { s_start = wrap;
                             s_end   = (wrap + 0x200 < W) ? (wrap + 0x200) : W;
                             t_base  = 0; }
        }
    };

    for (int y = 0; y < MAX_LINES; y++)
    {
        const uint32_t data0 = roadram[0x000 + y];
        const uint32_t data1 = roadram[0x100 + y];

        if (((data0 & 0x800) != 0) && ((data1 & 0x800) != 0))
        { line[y].kind = SKIP; rsp::desc_uc[y].kind = 0; continue; }
        if (ctrl == 0 && ((data0 & 0x800) != 0))
        { line[y].kind = SKIP; rsp::desc_uc[y].kind = 0; continue; }
        if (ctrl == 3 && ((data1 & 0x800) != 0))
        { line[y].kind = SKIP; rsp::desc_uc[y].kind = 0; continue; }

        const int32_t color0_idx = ((road_control & 4) != 0)
                                      ? y : (int32_t)(data0 & 0x1ff);
        const int32_t color1_idx = ((road_control & 4) != 0)
                                      ? (0x100 + y) : (int32_t)(data1 & 0x1ff);
        const int32_t color0 = roadram[0x600 + color0_idx];
        const int32_t color1 = roadram[0x600 + color1_idx];
        int32_t hpos0 = roadram[0x200 + color0_idx] & 0xfff;
        int32_t hpos1 = roadram[0x400 + color1_idx] & 0xfff;

        uint8_t* src0 = ((data0 & 0x800) != 0)
                          ? roads + 256 * 2 * 512
                          : roads + (0x000 + ((data0 >> 1) & 0xff)) * 512;
        uint8_t* src1 = ((data1 & 0x800) != 0)
                          ? roads + 256 * 2 * 512
                          : roads + (0x100 + ((data1 >> 1) & 0xff)) * 512;

        uint16_t* tlut = tlut_ptr(y);
        tlut[0] = rgb_lut[color_offset1 ^ 0x00 ^ ((color0 >> 0) & 1)];
        tlut[1] = rgb_lut[color_offset1 ^ 0x02 ^ ((color0 >> 1) & 1)];
        tlut[2] = rgb_lut[color_offset1 ^ 0x04 ^ ((color0 >> 2) & 1)];
        tlut[7] = rgb_lut[color_offset1 ^ 0x06 ^ ((color0 >> 3) & 1)];
        {
            int32_t bg = (color0 >> 8) & 0xf;
            tlut[3] = ((data0 & 0x200) != 0)
                         ? tlut[0]
                         : rgb_lut[color_offset2 ^ 0x00 ^ bg];
        }
        tlut[8]  = rgb_lut[color_offset1 ^ 0x08 ^ ((color1 >> 4) & 1)];
        tlut[9]  = rgb_lut[color_offset1 ^ 0x0a ^ ((color1 >> 5) & 1)];
        tlut[10] = rgb_lut[color_offset1 ^ 0x0c ^ ((color1 >> 6) & 1)];
        tlut[15] = rgb_lut[color_offset1 ^ 0x0e ^ ((color1 >> 7) & 1)];
        {
            int32_t bg = (color1 >> 8) & 0xf;
            tlut[11] = ((data1 & 0x200) != 0)
                          ? tlut[8]
                          : rgb_lut[color_offset2 ^ 0x10 ^ bg];
        }
        tlut[4] = 0; tlut[5] = 0; tlut[6] = 0;
        tlut[12] = 0; tlut[13] = 0; tlut[14] = 0;

        hpos0 = (hpos0 - (s16_x + x_offset)) & 0xfff;
        hpos1 = (hpos1 - (s16_x + x_offset)) & 0xfff;

        int s0s, s0e, t0b, s1s, s1e, t1b;
        compute_span(hpos0, W, s0s, s0e, t0b);
        compute_span(hpos1, W, s1s, s1e, t1b);

        const uint16_t c_oob = tlut[oob_slot];

        int span_start, span_end;
        if (ctrl == 0)      { span_start = s0s; span_end = s0e; }
        else if (ctrl == 3) { span_start = s1s; span_end = s1e; }
        else {
            span_start = (s0s < s1s) ? s0s : s1s;
            span_end   = (s0e > s1e) ? s0e : s1e;
        }

        line[y].s_start   = (uint16_t)span_start;
        line[y].s_end     = (uint16_t)span_end;
        line[y].tex_start = (uint16_t)span_start;
        line[y].tex_end   = (uint16_t)span_end;
        line[y].c_oob     = c_oob;

        if (span_end <= span_start) {
            line[y].kind = OOB_ONLY;
            rsp::desc_uc[y].kind = 1;
            continue;
        }

        line[y].kind = DRAW;

        // Per-row descriptor. src*_phys is the row base + t_offset so RSP
        // reads sequentially from [0, span_count) into its DMEM buffer.
        // tlut_phys / runs_phys point to the same buffers the CPU build
        // path uses (tlut_ptr(y) / runs_ptr(y)); both are uncached aliases
        // so RDP/RSP DMAs see fresh data without explicit writebacks.
        rsp::DescriptorRDP* d = &rsp::desc_uc[y];
        d->kind        = 2;
        d->ctrl_case   = ctrl;
        d->span_start  = (uint16_t)span_start;
        d->span_end    = (uint16_t)span_end;
        d->s0s         = (uint16_t)s0s;
        d->s0e         = (uint16_t)s0e;
        d->s1s         = (uint16_t)s1s;
        d->s1e         = (uint16_t)s1e;
        d->c_oob_color = c_oob;
        d->tlut_phys   = PhysicalAddr(tlut);
        d->src0_phys   = PhysicalAddr(src0 + t0b);
        d->src1_phys   = PhysicalAddr(src1 + t1b);
        d->runs_phys   = PhysicalAddr(runs_ptr(y));
    }

    uint64_t t_cpu_end = get_ticks_us();
    rsp::cpu_us = (rsp::cpu_us * 7 + (uint32_t)(t_cpu_end - t0)) >> 3;

    // Kick the overlay. RSP processes all MAX_LINES descriptors and writes
    // runs[] + per-row n_runs back to RDRAM via DMA. We do NOT wait here —
    // the caller is responsible for calling sync_runs() before invoking
    // emit_foreground_lores_rdp. This lets the RSP grind in parallel with
    // CPU work that queues road_bg + tile_layer rdpq commands.
    rspq_write(rsp::overlay_id, 0,
               MAX_LINES,
               PhysicalAddr(rsp::desc_cached),
               PhysicalAddr(rsp::state_cached));

    uint64_t t_kick_end = get_ticks_us();
    rsp::last_us = (rsp::last_us * 7 + (uint32_t)(t_kick_end - t0)) >> 3;

    // ---- Validation path -------------------------------------------------
    // Snapshot the RSP-produced runs[] + n_runs, re-run the CPU build into
    // the shared runs_buf / line[].n_runs, then diff. Emit ends up reading
    // the CPU result, so visuals stay correct even when the RSP path has a
    // bug. 2x prep cost — keep this off for perf testing.
    if (rsp::validate) {
        rspq_wait();
        // Snapshot RSP runs + n_runs.
        std::memcpy(rsp::shadow_buf, runs_buf,
                    (size_t)MAX_LINES * MAX_RUNS_PER_ROW * sizeof(Run));
        uint16_t shadow_n[MAX_LINES];
        for (int y = 0; y < MAX_LINES; y++)
            shadow_n[y] = (line[y].kind == DRAW) ? rsp::n_runs_uc[y] : 0;

        // Re-run CPU build into runs_buf + line[].n_runs.
        build_foreground_lores_rdp(rgb_lut);

        // Compare. Mismatch counted as: differing n_runs OR any Run byte
        // mismatch within the CPU-reported n_runs window.
        uint32_t mism = 0;
        int      first_row = -1;
        int      first_run = -1;
        for (int y = 0; y < MAX_LINES; y++) {
            if (line[y].kind != DRAW) continue;
            const uint16_t cpu_n = line[y].n_runs;
            const uint16_t rsp_n = shadow_n[y];
            if (cpu_n != rsp_n) {
                if (first_row < 0) { first_row = y; first_run = -1; }
                mism++;
                continue;
            }
            const Run* cpu_runs = runs_ptr(y);
            const Run* rsp_runs = (const Run*)
                (rsp::shadow_buf + (size_t)y * MAX_RUNS_PER_ROW * sizeof(Run));
            for (int r = 0; r < cpu_n; r++) {
                if (cpu_runs[r].x_end     != rsp_runs[r].x_end ||
                    cpu_runs[r].color5551 != rsp_runs[r].color5551) {
                    if (first_row < 0) { first_row = y; first_run = r; }
                    mism++;
                }
            }
        }
        if (mism > 0) {
            rsp::v_mismatches += mism;
            if (rsp::v_first_row < 0) {
                rsp::v_first_row  = first_row;
                rsp::v_first_byte = first_run;  // repurposed: first mismatching run index
            }
        }
        if ((rsp::s_frame % 60) == 0) {
            debugf("hwroad_rdp_rsp: validate mism=%lu first=(row=%d,run=%d)\n",
                   (unsigned long)rsp::v_mismatches,
                   rsp::v_first_row, rsp::v_first_byte);
        }
    }

    if ((rsp::s_frame % 60) == 0 && display_get_fps() < 30.0f) {
        debugf("hwroad_rdp_rsp: wait=%lu cpu=%lu kick=%lu\n",
               (unsigned long)sub_wait,
               (unsigned long)(t_cpu_end - t0),
               (unsigned long)(t_kick_end - t_cpu_end));
    }
}
