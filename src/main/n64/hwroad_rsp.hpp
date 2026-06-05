/***************************************************************************
    RSP offload of HWRoad foreground rasteriser.

    The companion overlay lives in n64/hwroad_rsp.S. This header exposes the
    CPU-side entry points:
      * hwroad_rsp::init()  — once at startup, registers the rspq overlay
        and allocates the per-scanline descriptor buffer.
      * HWRoad::render_foreground_lores_rsp() — drop-in replacement for the
        CPU render_foreground_lores; builds descriptors and kicks the RSP.
***************************************************************************/

#pragma once

#include "platform.hpp"

namespace n64
{
namespace hwroad_rsp
{
    // Register the rsp_hwroad overlay and allocate the descriptor buffer.
    // Must be called once before render_foreground_lores_rsp is invoked.
    void init();

    // Tear down — release descriptor memory and unregister the overlay.
    // Not normally called (ROM exits to libdragon abort on shutdown).
    void shutdown();

    // Last frame's RSP execution time, EMA-smoothed (µs). Read by the
    // on-screen profiler / debug overlay.
    extern uint32_t last_us;

    // Runtime selector. When true, video.cpp routes render_foreground_lores
    // through the RSP overlay (HWRoad::render_foreground_lores_rsp); when
    // false, the CPU path runs. Toggleable at runtime so we can A/B test
    // and compare timings.
    extern bool enabled;

    // Per-case RSP enable bitmask. Bit N set ⇒ frames whose control bits ==
    // N go through the RSP overlay; bit clear ⇒ that case falls back to the
    // CPU rasteriser inside render_foreground_lores_rsp. control is a
    // per-frame value (not per-scanline), so a single frame is either
    // entirely RSP or entirely CPU — no hybrid scanline mixing. Default
    // 0x0F (all four cases on RSP). Only consulted while enabled==true.
    extern uint8_t rsp_case_mask;

    // Validation mode. When true, render_foreground_lores_rsp also runs the
    // CPU reference into a shadow buffer and diffs the two scratch surfaces,
    // logging the first mismatching pixel per frame (rate-limited). Costs a
    // full extra CPU pass per frame, so leave this off for perf runs.
    extern bool validate;

    // Last validation result (only updated while validate==true). mismatches
    // == 0 means CPU == RSP; first_y/first_x give the first differing pixel.
    extern uint32_t v_mismatches;
    extern int      v_first_y;
    extern int      v_first_x;

    // Per-case scanline distribution from the most recent frame. Populated
    // unconditionally during descriptor build, so they reflect the actual
    // workload regardless of whether the RSP overlay runs.
    //   case_scanlines[N] : scanlines with control==N that the RSP actually
    //                        rasterises (both roads not low-pri, etc).
    //   case_skipped[N]   : scanlines with control==N marked CTRL_SKIP (both
    //                        roads low-pri, or case-0/3 own-road low-pri).
    //   case_total[N]     : = case_scanlines[N] + case_skipped[N], i.e. all
    //                        scanlines that fell into case N (only one of
    //                        the four is non-zero per frame since control is
    //                        a per-frame value).
    extern uint32_t case_scanlines[4];
    extern uint32_t case_skipped[4];
    extern uint32_t case_total[4];
}
}
