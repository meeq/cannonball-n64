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
#include "n64/hwroad_rdp_rsp.hpp"
#include "n64/platform.hpp"
#include "hwvideo/hwroad.hpp"
#include "engine/oroad.hpp"          // ORoad::HORIZON_OFF, ::oroad
#include "frontend/config.hpp"

#include <libdragon.h>
#include <malloc.h>
#include <cstddef>
#include <cstring>

namespace n64_profile { extern uint32_t prim_count; }

namespace n64
{
namespace hwroad_rdp
{

uint32_t last_us = 0;
static uint32_t s_frame = 0;

// Shared with hwroad_rdp_rsp.cpp via the internal header.
namespace detail
{
    uint16_t* tlut_buf = nullptr;
    Run*      runs_buf = nullptr;
    LineState line[MAX_LINES];
}

using detail::MAX_LINES;
using detail::MAX_SPAN_PX;
using detail::TLUT_ENTRIES;
using detail::MAX_RUNS_PER_ROW;
using detail::LineState;
using detail::Run;
using detail::SKIP;
using detail::OOB_ONLY;
using detail::DRAW;
using detail::tlut_buf;
using detail::runs_buf;
using detail::line;
using detail::tlut_ptr;
using detail::runs_ptr;

namespace
{
    // Per-line CI4 TLUT — written by the CPU build (or RSP build via
    // hwroad_rdp_rsp) and read by RDP at emit time via TILE0 palette load.
    constexpr size_t TLUT_BUF_BYTES = (size_t)MAX_LINES * TLUT_ENTRIES * 2;

    // Per-line run lists. 224 * 64 * 4 = 56 KB.
    constexpr size_t RUNS_BUF_BYTES =
        (size_t)MAX_LINES * MAX_RUNS_PER_ROW * sizeof(Run);

    // Buffers live in the 0xA0… uncached segment. The CPU build writes
    // runs_buf through its uncached alias; sequential 32-bit Run stores
    // go through the R4300 store buffer and coalesce into 32-byte RDRAM
    // bursts — same trick the CPU rasteriser uses for its KSEG1 scratch
    // surface. We allocate via malloc_uncached_aligned which guarantees
    // no cache-line sharing with other heap regions, so there's no need
    // to invalidate stale cached lines after malloc.

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
    if (!tlut_buf) {
        tlut_buf = (uint16_t*)malloc_uncached_aligned(16, TLUT_BUF_BYTES);
        assertf(tlut_buf, "hwroad_rdp: tlut_buf alloc failed (%u bytes)",
                (unsigned)TLUT_BUF_BYTES);
    }
    if (!runs_buf) {
        runs_buf = (Run*)malloc_uncached_aligned(16, RUNS_BUF_BYTES);
        assertf(runs_buf, "hwroad_rdp: runs_buf alloc failed (%u bytes)",
                (unsigned)RUNS_BUF_BYTES);
    }
    std::memset(tlut_buf, 0, TLUT_BUF_BYTES);
    std::memset(runs_buf, 0, RUNS_BUF_BYTES);
    for (int y = 0; y < MAX_LINES; y++) {
        line[y].kind   = SKIP;
        line[y].n_runs = 0;
    }
}

void shutdown()
{
    if (tlut_buf) { free_uncached(tlut_buf); tlut_buf = nullptr; }
    if (runs_buf) { free_uncached(runs_buf); runs_buf = nullptr; }
}

bool should_render_road_fg()
{
    // Mirrors the engine-side suppression check formerly inlined at
    // video.cpp's prepare_frame and applied (until now) only there.
    // OMusic::enable sets horizon_base = HORIZON_OFF to drop the road on
    // the music-select screen; with fix_bugs on we honour that. Without
    // fix_bugs (legacy SDL behaviour) we keep rendering road_fg so the
    // change is a no-op outside the bug-fix path.
    return !config.engine.fix_bugs ||
           oroad.horizon_base != ORoad::HORIZON_OFF;
}

} // namespace hwroad_rdp
} // namespace n64

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
    //
    // When the RSP build path is active and has populated the per-row c_oob
    // array, hand the whole phase off to the RSP — it emits the same coalesced
    // SET_FILL_COLOR + FILL_RECTANGLE pairs straight into the RDP buffer,
    // running async behind the CPU's Phase 2b walk.
    uint32_t phase_a_rects = 0;
    if (n64::hwroad_rdp_rsp::coob_fill_ready()) {
        n64::hwroad_rdp_rsp::dispatch_coob_fill(x_off, y_off, W);
        // RSP emits its own SET_FILL_COLOR commands, so any colour we had
        // cached on the CPU side is now stale relative to what the RDP will
        // see when Phase 2b runs. Reset so Phase 2b's first run re-emits.
        color_valid = false;
    } else {
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
                n64_profile::prim_count++;
                phase_a_rects++;
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

    uint32_t rects = 0;

    if (n64::hwroad_rdp_rsp::emit_runs_ready()) {
        // Hand the per-run fill emit off to the RSP. It DMA's emit_row +
        // n_runs once, then per row DMA's runs[] and emits SET_FILL_COLOR +
        // FILL_RECTANGLE pairs straight into the RDP buffer — same dedup +
        // skip semantics as emit_open below (skip color==c_oob, color==0,
        // zero-width). No vertical coalesce: prior measurement of the CPU
        // path showed coalesce wins came from static-x bands the dispatch
        // can recoup later if needed.
        n64::hwroad_rdp_rsp::dispatch_emit_runs(x_off, y_off);
        // RSP emits its own SET_FILL_COLORs, so any cached last colour on
        // the CPU side is stale relative to the RDP stream.
        color_valid = false;
    } else {
        struct OpenRect { uint16_t x0, x1, color; uint16_t y_start; };
        // Bound: at most one open per visible run per row, capped by
        // MAX_RUNS_PER_ROW. Double-buffer for "current row about to open" vs
        // "carried from prior row".
        OpenRect open[MAX_RUNS_PER_ROW];
        OpenRect next_open[MAX_RUNS_PER_ROW];
        int n_open = 0;

        auto emit_open = [&](const OpenRect& o, int y_end) {
            if (!color_valid || o.color != last_color5551) {
                rdpq_set_fill_color(rgba32_from_5551(o.color));
                last_color5551 = o.color;
                color_valid    = true;
            }
            rdpq_fill_rectangle(x_off + o.x0, y_off + o.y_start,
                                x_off + o.x1, y_off + y_end);
            n64_profile::prim_count++;
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
    }

    uint64_t tC = get_ticks_us();

    {
        float fps = display_get_fps();
        if (fps > 10.0f && fps < 30.0f && (s_frame & 7) == 0) {
            debugf("DIP hwroad_rdp emit: fill=%lu (%lup) runs=%lu (%lup) total=%lu\n",
                   (unsigned long)(tB - t0),
                   (unsigned long)phase_a_rects,
                   (unsigned long)(tC - tB),
                   (unsigned long)rects,
                   (unsigned long)(tC - t0));
        }
    }
}
