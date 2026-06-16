/***************************************************************************
    CPU-side dispatch for the rsp_tile_cache overlay.

    Phase 1: registers the overlay and provides a TestPing wiring check.
    Future phases will add the BuildCache command that actually rebuilds
    a tile_cache::Layer surface.
***************************************************************************/

#include "n64/tile_cache_rsp.hpp"
#include "n64/platform.hpp"

#include <libdragon.h>
#include <malloc.h>
#include <cstring>

DEFINE_RSP_UCODE(rsp_tile_cache);

namespace n64
{
namespace tile_cache_rsp
{

bool initialised = false;

namespace
{
    uint32_t  overlay_id = 0;
    uint32_t* status_uc  = nullptr;

    constexpr uint32_t TEST_PING_CMD = 0;
}

void init()
{
    if (initialised)
        return;

    overlay_id = rspq_overlay_register(&rsp_tile_cache);

    status_uc = (uint32_t*)malloc_uncached_aligned(8, 8);
    assertf(status_uc != nullptr,
            "tile_cache_rsp: failed to alloc status word");
    status_uc[0] = 0;
    status_uc[1] = 0;

    initialised = true;
    debugf("tile_cache_rsp: initialised, overlay_id=0x%08lx status@%p\n",
           (unsigned long)overlay_id, status_uc);
}

bool test_ping(uint32_t payload)
{
    assertf(initialised, "tile_cache_rsp: test_ping before init");

    status_uc[0] = 0;
    status_uc[1] = 0;

    const uint32_t status_phys = PhysicalAddr(status_uc);
    rspq_write(overlay_id, TEST_PING_CMD, status_phys, payload, 0);
    rspq_wait();

    const uint32_t expected_echo     = payload ^ 0xDEADBEEFu;
    const uint32_t expected_sentinel = 0xCAFEBABEu;
    const uint32_t got_echo          = status_uc[0];
    const uint32_t got_sentinel      = status_uc[1];

    const bool ok = (got_echo == expected_echo)
                 && (got_sentinel == expected_sentinel);
    debugf("tile_cache_rsp: test_ping payload=0x%08lx echo=0x%08lx "
           "(expect 0x%08lx) sentinel=0x%08lx (expect 0x%08lx) -> %s\n",
           (unsigned long)payload,
           (unsigned long)got_echo,     (unsigned long)expected_echo,
           (unsigned long)got_sentinel, (unsigned long)expected_sentinel,
           ok ? "OK" : "FAIL");
    return ok;
}

} // namespace tile_cache_rsp
} // namespace n64
