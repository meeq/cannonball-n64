/***************************************************************************
    RDP offload of HWRoad foreground rasteriser — all 4 control values.

    Builds per-scanline IA4 alpha masks in *cached* RAM (up to 8 masks per
    active line — 4 for road0 and 4 for road1, one per non-OOB pixel value
    in {0, 1, 2, 7}) and lets the RDP composite them into the framebuffer
    at finalize_frame time. The blender + combiner are configured so texels
    with alpha=1 receive PRIM_COLOR, texels with alpha=0 leave the
    framebuffer untouched — so each textured rect paints one road colour
    over the in-bounds span where that road won the priority match. The
    (pix0=3, pix1=3) merged colour is handled by a single up-front fill
    rect per row, which also subsumes any in-span cell where both roads
    resolve to their bg.
***************************************************************************/

#pragma once

#include "platform.hpp"

namespace n64
{
namespace hwroad_rdp
{
    // Allocate the per-scanline mask buffer and the per-line state arrays.
    // Cheap — single static + one cached buffer. Safe to call before
    // display_init / rdpq is ready (it doesn't touch the RDP).
    void init();

    // Tear down. Not normally invoked.
    void shutdown();

    // Single source of truth for "should ANY road_fg render this frame?"
    // Returns false when the engine has suppressed road_fg (currently:
    // fix_bugs is on AND oroad.horizon_base == HORIZON_OFF — the
    // music-select screen sets that to hide the road). Both
    // Video::prepare_frame and Render_RDP::finalize_frame consult this
    // function so build and emit can never disagree on whether road_fg
    // should run; previously the build site held the predicate inline
    // and the emit site replayed stale line[]/runs_buf on skip frames.
    bool should_render_road_fg();

    // Last frame's RDP-side cost (EMA µs). Currently bundles the CPU mask
    // build + rspq emit. Drain time is best measured via rspq_wait at the
    // call site; this counter is just the emit time.
    extern uint32_t last_us;
}
}
