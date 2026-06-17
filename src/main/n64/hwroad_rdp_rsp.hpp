/***************************************************************************
    RSP-build variant of the hwroad RDP foreground path.

    Same on-frame contract as HWRoad::build_foreground_lores_rdp:
    populates the shared CI4 mask + per-line TLUT + LineState arrays in
    n64::hwroad_rdp so emit_foreground_lores_rdp can paint the frame
    untouched. The split:

      * CPU still computes per-row state (data0/data1, color_idx, hpos,
        src pointers, span coords, 16-entry TLUT) and writes a compact
        descriptor table.
      * CPU pre-fills mask rows with oob_pair via uncached 4-byte stores
        so OOB regions are already correct on RSP entry.
      * RSP DMAs per row's source rows in, scalar-packs the in-span
        nibble pairs into the mask, DMAs the mask back. Double-buffered
        so per-row DMA hides behind compute.

    This file ships with the RSP path stubbed (acks command, does no
    pack work) — visual contract is "uniform OOB-coloured road area"
    so the wiring is verifiable end-to-end before the real pack lands.
***************************************************************************/

#pragma once

#include "platform.hpp"

namespace n64
{
namespace hwroad_rdp_rsp
{
    // Register the rsp_hwroad_rdp overlay and allocate descriptor +
    // frame-state buffers. Safe to call before display_init.
    void init();

    // Tear down — release buffers and unregister the overlay. Not normally
    // called (ROM exits to libdragon abort on shutdown).
    void shutdown();

    // Runtime selector. When true, video.cpp routes the build phase
    // through HWRoad::build_foreground_lores_rdp_rsp instead of the
    // CPU-only build. The emit phase is unchanged either way.
    // Default false — flip to A/B against the CPU-build path.
    extern bool enabled;

    // Wait for the kicked overlay to finish and copy the RSP-written
    // per-row n_runs back into line[y].n_runs so the emit phase can
    // walk runs[][]. Must be called between build_foreground_lores_rdp_rsp
    // and emit_foreground_lores_rdp. Cheap no-op if already drained.
    void sync_runs();

    // Last frame's RSP-side cost (EMA µs). Tracks RSP execution + the
    // CPU descriptor-build phase that has to run before kicking.
    extern uint32_t last_us;
    extern uint32_t cpu_us;   // CPU descriptor build only
    extern uint32_t rsp_us;   // RSP exec (measured via rspq_wait timing)

    // Per-row OOB-colour fill dispatch — RSP-side emit of vertically
    // coalesced fill_rectangles for the c_oob strip that backs every active
    // road row. Caller (emit phase) must have already issued
    // rdpq_set_mode_fill() so the RDP is in fill mode when our queued command
    // flushes through the rspq buffer. Args are framebuffer-space (x_off,
    // y_off) and span width W.
    void dispatch_coob_fill(int x_off, int y_off, int W);

    // True iff the build pass populated the per-row c_oob array for this
    // frame. Lets the emit phase fall back to the CPU loop when the build
    // was CPU-driven (rsp::enabled == false) or hasn't run yet.
    bool coob_fill_ready();

    // Per-row body fill dispatch — RSP-side emit of fill_rectangles for the
    // per-row run lists produced by BuildRuns. Reads n_runs from RDRAM (async
    // written by the BuildRuns DMAOut) and the per-row runs[][] from the
    // shared runs_buf. Walks each row's runs, emitting SET_FILL_COLOR +
    // FILL_RECTANGLE pairs for runs whose colour differs from c_oob (Phase 2a
    // already paints those) and is non-zero (vestigial CI4 transparent slot).
    // Must be queued AFTER dispatch_coob_fill so the c_oob backstop lands
    // first in the RDP command stream.
    void dispatch_emit_runs(int x_off, int y_off);

    // True iff the build pass populated the per-row emit state for this
    // frame. Lets the emit phase fall back to the CPU Phase 2b loop when
    // the build was CPU-driven or hasn't run yet.
    bool emit_runs_ready();
}
}
