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

    // Last frame's RSP-side cost (EMA µs). Tracks RSP execution + the
    // CPU descriptor-build phase that has to run before kicking.
    extern uint32_t last_us;
    extern uint32_t cpu_us;   // CPU descriptor build only
    extern uint32_t rsp_us;   // RSP exec (measured via rspq_wait timing)

    // Validation mode. When true, the CPU mask is also built into a
    // shadow buffer and diffed against the RSP-built mask. Logs the
    // first mismatching row + byte index per frame, rate-limited.
    // Adds a full CPU build per frame — off for perf runs.
    extern bool validate;

    // Last validation result (only updated while validate==true).
    extern uint32_t v_mismatches;
    extern int      v_first_row;
    extern int      v_first_byte;
}
}
