/***************************************************************************
    RDP path for HWRoad foreground rasteriser — all 4 road_control cases.

    Two phases per frame, both called under finalize_frame:

      1. CPU build pass. For each scanline:
           - Replay the same color_offset / data0 / hpos / color_table
             math as the CPU render_foreground_lores.
           - Compute the in-bounds spans s0 / s1.
           - Walk the union span pixel-by-pixel, looking up the merged
             slot via per-frame merged_idx[(p0, p1)] and resolving each
             slot to an RGBA5551 colour via a per-row 16-entry TLUT built
             on the stack.
           - Emit a per-row list of solid-colour runs (Run{x_end, color})
             into runs_buf. Adjacent same-colour pixels collapse into one
             run; typical OutRun rows produce only a handful of runs.

      2. RDP emit pass.
         a) Fill mode: one rdpq_fill_rectangle per active line painting the
            full row with c_oob (the both-bg merged colour). Doubles as the
            OOB fill outside the road extent and as the backstop wherever
            the road samples bg.
         b) Per active line, walk runs_buf and emit one rdpq_fill_rectangle
            per visible run (colour != c_oob). No texturing, no TLUT load,
            no alpha-compare — fill mode is pixel-fill-rate-limited and on
            paraLLEl-RDP runs an order of magnitude faster than the CI4 +
            TLUT + alpha-compare path it replaces.
***************************************************************************/

#include "n64/hwroad_rdp.hpp"
#include "n64/hwroad_rdp_internal.hpp"
#include "n64/platform.hpp"
#include "hwvideo/hwroad.hpp"
#include "frontend/config.hpp"

#include <libdragon.h>
#include <malloc.h>
#include <cstddef>
#include <cstring>

namespace n64
{
namespace hwroad_rdp
{

// All-cases RDP overlay. Toggle to A/B against the CPU rasterizer; runtime
// gate goes through should_skip_cpu().
bool     enabled = true;
uint32_t last_us = 0;
static uint32_t s_frame = 0;

// Shared with hwroad_rdp_rsp.cpp via the internal header.
namespace detail
{
    uint8_t*  mask_buf = nullptr;
    uint16_t* tlut_buf = nullptr;
    Run*      runs_buf = nullptr;
    LineState line[MAX_LINES];
}

using detail::MAX_LINES;
using detail::MAX_SPAN_PX;
using detail::MASK_BYTES;
using detail::TLUT_ENTRIES;
using detail::MAX_RUNS_PER_ROW;
using detail::LineState;
using detail::Run;
using detail::SKIP;
using detail::OOB_ONLY;
using detail::DRAW;
using detail::mask_buf;
using detail::tlut_buf;
using detail::runs_buf;
using detail::line;
using detail::mask_ptr;
using detail::tlut_ptr;
using detail::runs_ptr;

namespace
{
    // Per-line CI4 mask + TLUT — vestigial from the CI4 path, retained so
    // the RSP overlay file (hwroad_rdp_rsp.cpp) continues to compile against
    // the shared LineState layout. The CPU path no longer touches them.
    constexpr size_t MASK_BUF_BYTES = (size_t)MAX_LINES * MASK_BYTES;
    constexpr size_t TLUT_BUF_BYTES = (size_t)MAX_LINES * TLUT_ENTRIES * 2;

    // Per-line run lists. 224 * 64 * 4 = 56 KB.
    constexpr size_t RUNS_BUF_BYTES =
        (size_t)MAX_LINES * MAX_RUNS_PER_ROW * sizeof(Run);

    // Cached owner pointers (for free()). The CPU build writes runs_buf
    // through its uncached alias; sequential 32-bit Run stores go through
    // the R4300 store buffer and coalesce into 32-byte RDRAM bursts —
    // same trick the CPU rasteriser uses for its KSEG1 scratch surface.
    void*      mask_buf_cached = nullptr;
    void*      tlut_buf_cached = nullptr;
    void*      runs_buf_cached = nullptr;

    inline color_t rgba32_from_5551(uint16_t p)
    {
        uint8_t r = ((p >> 11) & 0x1F) << 3;
        uint8_t g = ((p >>  6) & 0x1F) << 3;
        uint8_t b = ((p >>  1) & 0x1F) << 3;
        return RGBA32(r, g, b, 0xFF);
    }

    // Scan p[i..len) for the largest j such that p[i..j) == b. u32-chunked
    // once j is 4-aligned. Road source rows have long runs of same-value
    // bytes (gutter, road body, lane edges), so byte-run extension collapses
    // the per-pixel walk into one EMIT per byte transition.
    inline int extend_byte_run(const uint8_t* p, int i, int len, uint8_t b)
    {
        int j = i + 1;
        while (j < len && ((uintptr_t)(p + j) & 3) != 0) {
            if (p[j] != b) return j;
            j++;
        }
        const uint32_t uniform = (uint32_t)b * 0x01010101u;
        while (j + 4 <= len) {
            uint32_t w;
            __builtin_memcpy(&w, p + j, 4);
            if (w != uniform) break;
            j += 4;
        }
        while (j < len && p[j] == b) j++;
        return j;
    }

    // Pair version for ctrl=1/2 both-road overlap pieces. Byte-only — u32
    // chunking, u64 chunking, dual-stream merge, and cross-row transition
    // caching all regressed or no-op'd here: per-iter setup outweighs the
    // win at OutRun's ~24 px run lengths (u64 chunking measured 14.5ms vs
    // byte-loop 12.6ms at 152 rows), and pointer-keyed vertical coherence
    // misses ~100% because each screen row picks its own texture row from
    // the perspective table. This tight byte loop is the 152-row floor.
    inline int extend_byte_run_pair(const uint8_t* p0, const uint8_t* p1,
                                    int i, int len, uint8_t b0, uint8_t b1)
    {
        int j = i + 1;
        while (j < len && p0[j] == b0 && p1[j] == b1) j++;
        return j;
    }

    // Inline copy of HWRoad::compute_road_span — kept private to hwroad.cpp.
    inline void compute_span(int32_t hpos, int W,
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
    }
}

void init()
{
    if (!mask_buf_cached) {
        mask_buf_cached = memalign(16, MASK_BUF_BYTES);
        mask_buf        = (uint8_t*)UncachedAddr(mask_buf_cached);
    }
    if (!tlut_buf_cached) {
        tlut_buf_cached = memalign(16, TLUT_BUF_BYTES);
        tlut_buf        = (uint16_t*)UncachedAddr(tlut_buf_cached);
    }
    if (!runs_buf_cached) {
        runs_buf_cached = memalign(16, RUNS_BUF_BYTES);
        runs_buf        = (Run*)UncachedAddr(runs_buf_cached);
    }
    if (mask_buf) std::memset(mask_buf, 0, MASK_BUF_BYTES);
    if (tlut_buf) std::memset(tlut_buf, 0, TLUT_BUF_BYTES);
    if (runs_buf) std::memset(runs_buf, 0, RUNS_BUF_BYTES);
    for (int y = 0; y < MAX_LINES; y++) {
        line[y].kind   = SKIP;
        line[y].n_runs = 0;
    }
}

void shutdown()
{
    if (mask_buf_cached) { free(mask_buf_cached); mask_buf_cached = nullptr; mask_buf = nullptr; }
    if (tlut_buf_cached) { free(tlut_buf_cached); tlut_buf_cached = nullptr; tlut_buf = nullptr; }
    if (runs_buf_cached) { free(runs_buf_cached); runs_buf_cached = nullptr; runs_buf = nullptr; }
}

bool should_skip_cpu(uint8_t /*road_control*/)
{
    return enabled;
}

} // namespace hwroad_rdp
} // namespace n64

// ---------------------------------------------------------------------------
// HWRoad::build_foreground_lores_rdp
//
// CPU-only phase. Replays the same color_offset / data0 / hpos / color_table
// math the CPU rasteriser uses, but emits its result into the uncached mask
// + per-line TLUT buffers instead of writing RGBA5551 into a scratch surface.
// No RDP commands are issued here. Called from prepare_frame so the previous
// frame's RDP work has time to drain; an rspq_wait() at entry guarantees we
// never overwrite a mask/TLUT the RDP is still reading. Empirically, running
// concurrently with RDP DMA inflates per-pixel cost 3-4× via RDRAM bus
// contention.
// ---------------------------------------------------------------------------
void HWRoad::build_foreground_lores_rdp(const uint16_t* rgb_lut)
{
    using namespace n64::hwroad_rdp;

    if (!runs_buf) return;

    const uint8_t ctrl = road_control & 3;
    s_frame++;

    if ((s_frame % 60) == 0)
        debugf("hwroad_rdp: ctrl=%u last_us=%lu\n",
               (unsigned)ctrl, (unsigned long)last_us);

    uint64_t t_wait0 = get_ticks_us();
    // Block on the previous frame's RDP work so this frame's run writes
    // don't race the RDP reading last frame's. Also keeps the RDRAM bus
    // empty for the upcoming uncached stores.
    rspq_wait();
    uint64_t t0 = get_ticks_us();
    const uint32_t sub_wait = (uint32_t)(t0 - t_wait0);

    // Sub-phase counters inside the per-row loop:
    //   sub_tlut: per-row 16-entry TLUT build (stack array)
    //   sub_spans: span computation, color_idx + hpos lookups
    //   sub_fill : per-pixel walk + run emission
    //   sub_rows : count of DRAW rows
    //   sub_runs : total runs emitted (for sizing diagnostics)
    uint32_t sub_tlut = 0, sub_spans = 0, sub_fill = 0;
    uint32_t sub_rows = 0, sub_runs = 0;
    uint32_t sub_overflow = 0;

    uint16_t* roadram = ramBuff;
    const int W      = config.s16_width;
    const uint16_t s16_x = 0x5f8 + config.s16_x_off;

    static const uint8_t priority_map[2][8] =
    {
        { 0x80, 0x81, 0x81, 0x87, 0, 0, 0, 0x00 },
        { 0x81, 0x81, 0x81, 0x8f, 0, 0, 0, 0x80 }
    };

    // Per-frame merged index LUT. merged_idx[(p0 << 3) | p1] gives the
    // 4-bit slot for the (pix0=p0, pix1=p1) decision:
    //   0..7  -> road0 colour table entry p0
    //   8..15 -> road1 colour table entry p1
    // For single-road controls (0/3) the table is independent of the
    // unused road's pixel value.
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

    // Collapsed slot LUTs used by single-road segments. slot8r0[p] ==
    // merged_idx[(p,3)] (in-road0, road1 OOB); slot8r1[p] ==
    // merged_idx[(3,p)] (in-road1, road0 OOB). oob_slot ==
    // merged_idx[(3,3)] — the both-bg colour, matches Phase 2a's per-row
    // fill rect.
    uint8_t slot8r0[8], slot8r1[8];
    for (int p = 0; p < 8; p++) {
        slot8r0[p] = merged_idx[(p << 3) | 3];
        slot8r1[p] = merged_idx[(3 << 3) | p];
    }
    const uint8_t oob_slot = merged_idx[(3 << 3) | 3];

    // ---- Phase 1: build per-row run lists -------------------------------

    for (int y = 0; y < MAX_LINES; y++)
    {
        const uint32_t data0 = roadram[0x000 + y];
        const uint32_t data1 = roadram[0x100 + y];

        // Same skip conditions as the CPU path
        if (((data0 & 0x800) != 0) && ((data1 & 0x800) != 0))
        { line[y].kind = SKIP; line[y].n_runs = 0; continue; }
        if (ctrl == 0 && ((data0 & 0x800) != 0))
        { line[y].kind = SKIP; line[y].n_runs = 0; continue; }
        if (ctrl == 3 && ((data1 & 0x800) != 0))
        { line[y].kind = SKIP; line[y].n_runs = 0; continue; }

        uint64_t r_t0 = get_ticks_us();

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

        uint64_t r_t1 = get_ticks_us();
        sub_spans += (uint32_t)(r_t1 - r_t0);

        // Per-row 16-entry TLUT on the stack. Slot layout matches the old
        // CI4 path: 0..7 = road0 c[0..7], 8..15 = road1 c[0..7]. Slots
        // 4/5/6 and 12/13/14 are "unused" and get 0 (RGBA5551 transparent)
        // — runs that resolve to them are skipped by emit, same as the
        // c_oob skip.
        uint16_t tlut_l[16];
        tlut_l[0] = rgb_lut[color_offset1 ^ 0x00 ^ ((color0 >> 0) & 1)];
        tlut_l[1] = rgb_lut[color_offset1 ^ 0x02 ^ ((color0 >> 1) & 1)];
        tlut_l[2] = rgb_lut[color_offset1 ^ 0x04 ^ ((color0 >> 2) & 1)];
        tlut_l[7] = rgb_lut[color_offset1 ^ 0x06 ^ ((color0 >> 3) & 1)];
        {
            int32_t bg = (color0 >> 8) & 0xf;
            tlut_l[3] = ((data0 & 0x200) != 0)
                           ? tlut_l[0]
                           : rgb_lut[color_offset2 ^ 0x00 ^ bg];
        }
        tlut_l[8]  = rgb_lut[color_offset1 ^ 0x08 ^ ((color1 >> 4) & 1)];
        tlut_l[9]  = rgb_lut[color_offset1 ^ 0x0a ^ ((color1 >> 5) & 1)];
        tlut_l[10] = rgb_lut[color_offset1 ^ 0x0c ^ ((color1 >> 6) & 1)];
        tlut_l[15] = rgb_lut[color_offset1 ^ 0x0e ^ ((color1 >> 7) & 1)];
        {
            int32_t bg = (color1 >> 8) & 0xf;
            tlut_l[11] = ((data1 & 0x200) != 0)
                            ? tlut_l[8]
                            : rgb_lut[color_offset2 ^ 0x10 ^ bg];
        }
        tlut_l[4] = 0; tlut_l[5] = 0; tlut_l[6] = 0;
        tlut_l[12] = 0; tlut_l[13] = 0; tlut_l[14] = 0;

        uint64_t r_t2 = get_ticks_us();
        sub_tlut += (uint32_t)(r_t2 - r_t1);

        hpos0 = (hpos0 - (s16_x + x_offset)) & 0xfff;
        hpos1 = (hpos1 - (s16_x + x_offset)) & 0xfff;

        int s0s, s0e, t0b, s1s, s1e, t1b;
        compute_span(hpos0, W, s0s, s0e, t0b);
        compute_span(hpos1, W, s1s, s1e, t1b);

        const uint16_t c_oob = tlut_l[oob_slot];

        int span_start, span_end;
        if (ctrl == 0)      { span_start = s0s; span_end = s0e; }
        else if (ctrl == 3) { span_start = s1s; span_end = s1e; }
        else {
            span_start = (s0s < s1s) ? s0s : s1s;
            span_end   = (s0e > s1e) ? s0e : s1e;
        }

        line[y].s_start = (uint16_t)span_start;
        line[y].s_end   = (uint16_t)span_end;
        line[y].c_oob   = c_oob;

        if (span_end <= span_start) {
            line[y].kind   = OOB_ONLY;
            line[y].n_runs = 0;
            continue;
        }

        // Fold the per-pixel slot lookup into the colour table. Each entry
        // of color_r0/color_r1 is tlut_l[slot8rX[p]] for road-X pixel value
        // p in 0..7 — collapses two chained loads per pixel into one. For
        // ctrl=1/2 the merged decision uses color_merged[(p0<<3)|p1] over
        // [overlap_lo, overlap_hi); the precompute is 64 loads so we skip
        // it entirely when the roads don't overlap on this scanline.
        uint16_t color_r0[8], color_r1[8];
        for (int p = 0; p < 8; p++) {
            color_r0[p] = tlut_l[slot8r0[p]];
            color_r1[p] = tlut_l[slot8r1[p]];
        }
        uint16_t color_merged[64];
        int overlap_lo = 0, overlap_hi = 0;
        if (ctrl == 1 || ctrl == 2) {
            overlap_lo = (s0s > s1s) ? s0s : s1s;
            overlap_hi = (s0e < s1e) ? s0e : s1e;
            if (overlap_lo < overlap_hi) {
                for (int i = 0; i < 64; i++)
                    color_merged[i] = tlut_l[merged_idx[i]];
            }
        }

        // Walk [span_start, span_end), look up the merged colour per pixel,
        // emit a new Run whenever the colour changes. runs[i].x_end is the
        // exclusive end of that run; the implicit start is span_start
        // (i == 0) or runs[i-1].x_end. The emit path skips runs whose
        // colour equals c_oob (Phase 2a paints that row-wide), so a leading
        // c_oob region naturally collapses to a single skipped run.
        Run* runs = runs_ptr(y);
        int  n = 0;
        uint16_t prev_color = 0;

        // EMIT(X, COLOR): close the active run at X (exclusive) and open a
        // new run with COLOR. No-op if colour matches the previous run.
        // Overflow drops new appends but keeps closing the trailing run —
        // the visual fallout is the (MAX-1)th colour bleeding to span_end.
        #define EMIT(X, COLOR)                                              \
            do {                                                            \
                const uint16_t _c = (COLOR);                                \
                if (n == 0 || _c != prev_color) {                           \
                    if (n > 0) runs[n - 1].x_end = (uint16_t)(X);           \
                    if (n < MAX_RUNS_PER_ROW) {                             \
                        runs[n].color5551 = _c;                             \
                        n++;                                                \
                    } else {                                                \
                        sub_overflow++;                                     \
                    }                                                       \
                    prev_color = _c;                                        \
                }                                                           \
            } while (0)

        if (ctrl == 0)
        {
            const uint8_t* p0d = src0 + t0b;
            const int len = s0e - s0s;
            int i = 0;
            while (i < len) {
                const uint8_t b = p0d[i];
                EMIT(s0s + i, color_r0[b]);
                i = extend_byte_run(p0d, i, len, b);
            }
        }
        else if (ctrl == 3)
        {
            const uint8_t* p1d = src1 + t1b;
            const int len = s1e - s1s;
            int i = 0;
            while (i < len) {
                const uint8_t b = p1d[i];
                EMIT(s1s + i, color_r1[b]);
                i = extend_byte_run(p1d, i, len, b);
            }
        }
        else
        {
            // ctrl=1/2 dual-road. Walk pieces of [span_start, span_end) with
            // uniform in0/in1 status by partitioning at s0s/s0e/s1s/s1e.
            // p0d[x] / p1d[x] are valid for x in their respective road's
            // span; we only dereference them inside the corresponding piece.
            const uint8_t* p0d = src0 + (t0b - s0s);
            const uint8_t* p1d = src1 + (t1b - s1s);

            int bounds[6];
            int nb = 0;
            bounds[nb++] = span_start;
            if (s0s > span_start && s0s < span_end) bounds[nb++] = s0s;
            if (s0e > span_start && s0e < span_end) bounds[nb++] = s0e;
            if (s1s > span_start && s1s < span_end) bounds[nb++] = s1s;
            if (s1e > span_start && s1e < span_end) bounds[nb++] = s1e;
            bounds[nb++] = span_end;
            // Insertion sort the boundary list (nb <= 6).
            for (int i = 1; i < nb; i++) {
                int v = bounds[i], j = i;
                while (j > 0 && bounds[j - 1] > v) {
                    bounds[j] = bounds[j - 1]; j--;
                }
                bounds[j] = v;
            }

            for (int piece = 0; piece + 1 < nb; piece++) {
                const int lo = bounds[piece];
                const int hi = bounds[piece + 1];
                if (lo >= hi) continue;
                const bool in0 = (lo >= s0s) && (lo < s0e);
                const bool in1 = (lo >= s1s) && (lo < s1e);
                if (in0 && in1) {
                    int x = lo;
                    while (x < hi) {
                        const uint8_t b0 = p0d[x];
                        const uint8_t b1 = p1d[x];
                        EMIT(x, color_merged[(b0 << 3) | b1]);
                        x = extend_byte_run_pair(p0d, p1d, x, hi, b0, b1);
                    }
                } else if (in0) {
                    int x = lo;
                    while (x < hi) {
                        const uint8_t b = p0d[x];
                        EMIT(x, color_r0[b]);
                        x = extend_byte_run(p0d, x, hi, b);
                    }
                } else if (in1) {
                    int x = lo;
                    while (x < hi) {
                        const uint8_t b = p1d[x];
                        EMIT(x, color_r1[b]);
                        x = extend_byte_run(p1d, x, hi, b);
                    }
                } else {
                    // Inter-road OOB piece: single c_oob run spanning [lo, hi).
                    EMIT(lo, c_oob);
                }
            }
        }

        #undef EMIT

        if (n > 0) {
            runs[n - 1].x_end = (uint16_t)span_end;
            line[y].n_runs    = (uint16_t)n;
            line[y].kind      = DRAW;
            sub_rows++;
            sub_runs += (uint32_t)n;
        } else {
            line[y].n_runs = 0;
            line[y].kind   = OOB_ONLY;
        }

        uint64_t r_t3 = get_ticks_us();
        sub_fill += (uint32_t)(r_t3 - r_t2);
    }

    uint64_t tA = get_ticks_us();
    last_us = (last_us * 7 + (uint32_t)(tA - t0)) >> 3;

    if ((s_frame % 60) == 0) {
        debugf("hwroad_rdp build: wait=%lu tlut=%lu spans=%lu fill=%lu rows=%lu runs=%lu ovf=%lu total=%lu\n",
               (unsigned long)sub_wait,
               (unsigned long)sub_tlut,
               (unsigned long)sub_spans,
               (unsigned long)sub_fill,
               (unsigned long)sub_rows,
               (unsigned long)sub_runs,
               (unsigned long)sub_overflow,
               (unsigned long)(tA - t0));
    }
}

// ---------------------------------------------------------------------------
// HWRoad::emit_foreground_lores_rdp
//
// RDP-only phase. Reads per-line state + runs_buf produced by
// build_foreground_lores_rdp and emits two passes of fill rectangles:
//   2a) one per active line painting c_oob row-wide.
//   2b) one per visible run (colour != c_oob) within each DRAW line.
// Caller must have a target rdpq_attach'd; coordinates are framebuffer-space.
// ---------------------------------------------------------------------------
void HWRoad::emit_foreground_lores_rdp(int x_off, int y_off)
{
    using namespace n64::hwroad_rdp;

    if (!runs_buf) return;

    uint64_t t0 = get_ticks_us();
    const int W = config.s16_width;

    // Track the last colour we pushed to the RDP across both phases so we can
    // skip redundant set_fill_color emits. In tunnel scenes typical row
    // sequences share their OOB colour and many adjacent runs repeat the same
    // body colour, so dedup cuts both CPU emit work and RDP command bandwidth.
    rdpq_set_mode_fill(RGBA32(0, 0, 0, 0));
    uint16_t last_color5551 = 0;
    bool     color_valid    = false;

    // ---- Phase 2a: per-line c_oob fill ------------------------------------
    //
    // Coalesce adjacent non-SKIP rows sharing the same c_oob into one
    // multi-row rectangle. Tunnel rows have long stretches of identical
    // OOB colour, so a worst-case 224 single-pixel fills collapses to a
    // few dozen taller rects.
    {
        int      run_start = -1;
        uint16_t run_color = 0;
        for (int y = 0; y <= MAX_LINES; y++)
        {
            const bool active = (y < MAX_LINES) && (line[y].kind != SKIP);
            const uint16_t c  = active ? line[y].c_oob : 0;
            const bool ends   = (run_start >= 0) && (!active || c != run_color);
            if (ends) {
                if (!color_valid || run_color != last_color5551) {
                    rdpq_set_fill_color(rgba32_from_5551(run_color));
                    last_color5551 = run_color;
                    color_valid    = true;
                }
                rdpq_fill_rectangle(x_off, y_off + run_start,
                                    x_off + W, y_off + y);
                run_start = -1;
            }
            if (active && run_start < 0) {
                run_start = y;
                run_color = c;
            }
        }
    }

    uint64_t tB = get_ticks_us();

    // ---- Phase 2b: per-run fill rectangles --------------------------------
    //
    // Single-pass vertical coalesce: keep a list of "open" rects from the
    // previous row. For each visible run in row y, find a matching open rect
    // (same x0/x1/color) — if found, extend the open rect's y range; if not,
    // start a new open. Open rects that didn't continue are emitted at the
    // top of the next row's pass. Tunnel scenes have many short vertical
    // bars (overpass beams, sky strips) that collapse from N 1-pixel rects
    // into one taller rect, cutting both CPU emit work and RDP command
    // bandwidth roughly in half.
    //
    // x0/x1 perspective-shift between rows in the road body kills matching
    // for most road runs, so the gains come from the static-x regions
    // (above-horizon sky bands, tunnel-wall strips) where x0/x1 are constant
    // across many rows.

    struct OpenRect { uint16_t x0, x1, color; uint16_t y_start; };
    // Bound: at most one open per visible run per row, capped by
    // MAX_RUNS_PER_ROW. Double-buffer for "current row about to open" vs
    // "carried from prior row".
    OpenRect open[MAX_RUNS_PER_ROW];
    OpenRect next_open[MAX_RUNS_PER_ROW];
    int n_open = 0;

    uint32_t rects = 0;

    auto emit_open = [&](const OpenRect& o, int y_end) {
        if (!color_valid || o.color != last_color5551) {
            rdpq_set_fill_color(rgba32_from_5551(o.color));
            last_color5551 = o.color;
            color_valid    = true;
        }
        rdpq_fill_rectangle(x_off + o.x0, y_off + o.y_start,
                            x_off + o.x1, y_off + y_end);
        rects++;
    };

    for (int y = 0; y < MAX_LINES; y++)
    {
        int n_next = 0;
        // Track which prior-row opens get continued; the rest emit at y.
        uint32_t consumed = 0;

        if (line[y].kind == DRAW) {
            const Run*     runs   = runs_ptr(y);
            const int      n      = line[y].n_runs;
            const uint16_t c_oob  = line[y].c_oob;
            int prev_x = line[y].s_start;
            for (int r = 0; r < n; r++) {
                const uint16_t color = runs[r].color5551;
                const int      xe    = runs[r].x_end;
                if (color != c_oob && color != 0 && xe > prev_x) {
                    // In-row horizontal coalesce: if the immediately previous
                    // opened entry abuts (x1==prev_x) with the same color,
                    // extend its x1 instead of opening a new rect. Both
                    // continued and freshly-opened entries are fair game —
                    // mutating a continued open's x1 just means future rows
                    // will need to match the wider extent to continue, which
                    // is the desired behaviour for static-x bands.
                    if (n_next > 0
                        && next_open[n_next - 1].color == color
                        && next_open[n_next - 1].x1    == (uint16_t)prev_x) {
                        next_open[n_next - 1].x1 = (uint16_t)xe;
                    } else {
                        int matched = -1;
                        for (int o = 0; o < n_open; o++) {
                            if (consumed & (1u << o)) continue;
                            if (open[o].x0 == prev_x && open[o].x1 == xe
                                && open[o].color == color) {
                                matched = o;
                                break;
                            }
                        }
                        if (matched >= 0) {
                            consumed |= (1u << matched);
                            next_open[n_next++] = open[matched];
                        } else {
                            next_open[n_next].x0      = (uint16_t)prev_x;
                            next_open[n_next].x1      = (uint16_t)xe;
                            next_open[n_next].color   = color;
                            next_open[n_next].y_start = (uint16_t)y;
                            n_next++;
                        }
                    }
                }
                prev_x = xe;
            }
        }

        // Close any prior-row opens that didn't continue.
        for (int o = 0; o < n_open; o++) {
            if (!(consumed & (1u << o)))
                emit_open(open[o], y);
        }

        // Swap.
        for (int i = 0; i < n_next; i++) open[i] = next_open[i];
        n_open = n_next;
    }

    // Flush any opens still active after the last row.
    for (int o = 0; o < n_open; o++) emit_open(open[o], MAX_LINES);

    uint64_t tC = get_ticks_us();

    {
        float fps = display_get_fps();
        if (fps > 10.0f && fps < 30.0f && (s_frame & 7) == 0) {
            debugf("DIP hwroad_rdp emit: fill=%lu runs=%lu rects=%lu total=%lu\n",
                   (unsigned long)(tB - t0),
                   (unsigned long)(tC - tB),
                   (unsigned long)rects,
                   (unsigned long)(tC - t0));
        }
    }
}
