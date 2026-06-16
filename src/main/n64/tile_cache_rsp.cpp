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

    constexpr uint32_t TEST_PING_CMD  = 0;
    constexpr uint32_t FILL_CACHE_CMD = 1;
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

bool test_fill(uint32_t bytes)
{
    assertf(initialised, "tile_cache_rsp: test_fill before init");
    assertf((bytes & 0x7ff) == 0 && bytes > 0 && bytes <= 0x100000,
            "tile_cache_rsp: bytes must be 2KiB-aligned, positive, ≤ 1MiB (got %lu)",
            (unsigned long)bytes);

    uint8_t* buf_uc = (uint8_t*)malloc_uncached_aligned(64, bytes);
    assertf(buf_uc != nullptr,
            "tile_cache_rsp: test_fill alloc(%lu) failed", (unsigned long)bytes);

    std::memset(buf_uc, 0xAA, bytes);
    status_uc[0] = 0;
    status_uc[1] = 0;

    const uint32_t buf_phys    = PhysicalAddr(buf_uc);
    const uint32_t status_phys = PhysicalAddr(status_uc);

    const uint64_t t0 = get_ticks_us();
    rspq_write(overlay_id, FILL_CACHE_CMD, buf_phys, bytes, status_phys);

    // Surface any RSP assertion immediately. rspq_wait()'s RSP_WAIT_LOOP
    // calls __rsp_check_assert per iteration; if the RSP halted on
    // assertion_failed, __rsp_crash is invoked which loads the crash ucode
    // and prints PC + GPRs + assert code from $at. That's far more useful
    // than the previous silent 100 ms timeout.
    rspq_wait();

    if (status_uc[0] != 0xDEADC0DEu) {
        debugf("tile_cache_rsp: test_fill(%lu) FAIL — status=0x%08lx after rspq_wait\n",
               (unsigned long)bytes, (unsigned long)status_uc[0]);
        free_uncached(buf_uc);
        return false;
    }
    const uint64_t t1 = get_ticks_us();

    // Verify every word is zero.
    bool ok = true;
    uint32_t first_bad = 0;
    const uint32_t* w = (const uint32_t*)buf_uc;
    const uint32_t nw = bytes / 4;
    for (uint32_t i = 0; i < nw; i++) {
        if (w[i] != 0) { ok = false; first_bad = i * 4; break; }
    }

    const uint32_t us = (uint32_t)(t1 - t0);
    const uint32_t mbps = (us > 0)
        ? (uint32_t)((uint64_t)bytes * 1000000ull / us / (1024 * 1024))
        : 0;
    debugf("tile_cache_rsp: test_fill bytes=%lu took=%lu us "
           "(%lu MB/s) -> %s",
           (unsigned long)bytes, (unsigned long)us,
           (unsigned long)mbps, ok ? "OK" : "FAIL");
    if (!ok)
        debugf(" first_bad_byte=%lu val=0x%08lx",
               (unsigned long)first_bad, (unsigned long)w[first_bad / 4]);
    debugf("\n");

    free_uncached(buf_uc);
    return ok;
}

void zero_async(void* cache_buf, uint32_t bytes)
{
    assertf(initialised, "tile_cache_rsp: zero_async before init");
    assertf(cache_buf != nullptr, "tile_cache_rsp: zero_async null buf");
    assertf((bytes & 0x7ff) == 0 && bytes > 0 && bytes <= 0x100000,
            "tile_cache_rsp: zero_async bytes must be 2KiB-aligned, "
            "positive, ≤ 1MiB (got %lu)", (unsigned long)bytes);

    status_uc[0] = 0;
    status_uc[1] = 0;

    const uint32_t buf_phys    = PhysicalAddr(cache_buf);
    const uint32_t status_phys = PhysicalAddr(status_uc);

    rspq_write(overlay_id, FILL_CACHE_CMD, buf_phys, bytes, status_phys);
    rspq_flush();
}

void zero_sync()
{
    assertf(initialised, "tile_cache_rsp: zero_sync before init");
    // rspq_wait both flushes and surfaces any RSP assertion via
    // __rsp_check_assert / __rsp_crash. Cheap if work is already drained.
    rspq_wait();
}

} // namespace tile_cache_rsp
} // namespace n64
