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

    // Phase 2 fill test. Allocates a `bytes`-sized uncached buffer (must
    // be a positive multiple of 2 KiB, ≤ 1 MiB), pre-fills with 0xAA,
    // kicks FillCache, polls until RSP writes 0xDEADC0DE, then verifies
    // the buffer is all zeros. Returns true on full success.
    bool test_fill(uint32_t bytes);

    // Phase 4a: async zero-fill of `bytes` at `cache_buf` (physical addr
    // implied — pass a cached pointer; we resolve via PhysicalAddr). Issues
    // an SP DMA fill from a DMEM ZERO_CHUNK, flushes the rspq, and returns
    // immediately. SP DMA writes bypass the CPU dcache, so the caller
    // does NOT need to writeback this range afterwards. `bytes` must be a
    // positive multiple of 2 KiB. Caller MUST invoke zero_sync() before
    // touching cache_buf or kicking another zero_async.
    void zero_async(void* cache_buf, uint32_t bytes);

    // Block until the most recent zero_async completes. Cheap if it
    // already drained. Surfaces any RSP assertion via __rsp_check_assert.
    void zero_sync();

    extern bool initialised;
}
}
