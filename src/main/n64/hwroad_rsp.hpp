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
}
}
