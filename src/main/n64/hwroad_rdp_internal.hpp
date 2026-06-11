/***************************************************************************
    Internal-use header shared by hwroad_rdp.cpp (CPU build path) and
    hwroad_rdp_rsp.cpp (RSP build path). Exposes the storage and types
    that both builders populate and emit_foreground_lores_rdp consumes.
    Not part of the public n64/hwroad_rdp.hpp API.
***************************************************************************/

#pragma once

#include "n64/platform.hpp"

namespace n64
{
namespace hwroad_rdp
{
namespace detail
{
    constexpr int MAX_LINES    = S16_HEIGHT;    // 224
    constexpr int MAX_SPAN_PX  = 320;           // s16 active width
    constexpr int TLUT_ENTRIES = 16;

    // Per-row run list — replaces the CI4 mask + TLUT pair for the CPU
    // build/emit path. Each Run describes a span of identical RGBA5551
    // color that ends at x_end (exclusive). The implicit start is either
    // line[y].s_start (for runs[0]) or the previous run's x_end. Runs are
    // contiguous and cover [s_start, s_end). Emit skips any run whose
    // color equals c_oob because Phase 2a already paints the row with
    // c_oob via a single fill_rectangle.
    struct Run
    {
        uint16_t x_end;
        uint16_t color5551;
    };
    constexpr int MAX_RUNS_PER_ROW = 64;

    // SKIP / OOB_ONLY behave as before. DRAW = run list in runs_buf row.
    // DRAW_DIRECT_R0 was used by the CI4 path; retained as an enum value
    // for ABI compatibility with hwroad_rdp_rsp.cpp but no longer emitted.
    enum LineKind : uint8_t
    {
        SKIP            = 0,
        OOB_ONLY        = 1,
        DRAW            = 2,
        DRAW_DIRECT_R0  = 3,
    };

    struct LineState
    {
        uint8_t  kind;
        // s_start/s_end is the union span. CPU path also uses s_start as
        // the implicit start of runs[0]. tex_start/tex_end and src_phys/
        // src_s_offset are vestigial from the CI4 path — kept for the RSP
        // overlay file's LineState layout compatibility.
        uint16_t s_start, s_end;
        uint16_t tex_start, tex_end;
        uint16_t c_oob;
        uint16_t n_runs;
        uint32_t src_phys;
        uint8_t  src_s_offset;
    };

    // Defined in hwroad_rdp.cpp. Uncached aliases (KSEG1) — writes go
    // through the R4300 store buffer with no cache traffic; RDP DMAs read
    // fresh data without an explicit writeback.
    extern uint16_t* tlut_buf;
    extern Run*      runs_buf;
    extern LineState line[MAX_LINES];

    inline uint16_t* tlut_ptr(int y) { return tlut_buf + (size_t)y * TLUT_ENTRIES; }
    inline Run*      runs_ptr(int y) { return runs_buf + (size_t)y * MAX_RUNS_PER_ROW; }
}
}
}
