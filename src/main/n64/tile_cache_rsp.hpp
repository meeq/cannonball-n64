/***************************************************************************
    RSP offload of the tile-layer cache rebuild — Phase 1 scaffolding.

    The companion overlay lives in n64/rsp_tile_cache.S. Future phases
    will add a build_layer() entry that rebuilds a tile_cache::Layer
    surface on the RSP in parallel with CPU rendering work. Phase 1
    proves the CPU↔RSP wiring works in isolation; nothing in the render
    path consumes this yet.
***************************************************************************/

#pragma once

#include "platform.hpp"

namespace n64
{
namespace tile_cache_rsp
{
    // Register the rsp_tile_cache overlay. Idempotent.
    void init();

    // Phase 1 wiring test. Sends TestPing with a payload + status address,
    // waits for RSP to complete, verifies the echoed word matches
    // (payload XOR 0xDEADBEEF) and the CAFEBABE sentinel landed. Returns
    // true on success; logs the result either way.
    bool test_ping(uint32_t payload);

    extern bool initialised;
}
}
