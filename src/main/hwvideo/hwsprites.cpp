#include "video.hpp"
#include "hwvideo/hwsprites.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"

#include <libdragon.h>
#include <cstring>
#include <malloc.h>

namespace n64_profile {
    extern uint32_t prim_count;
    extern uint32_t spr_call_vis;
    extern uint32_t spr_call_prims;
    extern uint32_t spr_call_loads;
    extern uint32_t spr_call_tlut_uploads;
    extern uint32_t spr_call_us;
    extern uint32_t spr_call_ovf;
}

// Set to 1 to log heap stats around atlas_init. Useful when tuning the
// atlas pool size against the heap ceiling — leave off in checked-in builds
// so the ISViewer log isn't cluttered.
#define CANNONBALL_LOG_HEAP 0

// Diagnostic toggle. When set to 1, the baked-atlas fast path is skipped
// entirely and every extract goes through the CPU EOR-walk spillover. Used
// to bisect sprite-corruption regressions involving the cart-side sprite
// ROM path: if corruption disappears with this on, the bug is in the baked
// path; if it persists, the bug is in the spillover slice-DMA / rebase
// logic. Leave at 0 in checked-in builds.
#define HWSPR_DIAG_FORCE_SPILLOVER 0

/***************************************************************************
    Video Emulation: OutRun Sprite Rendering Hardware.
    Based on MAME source code.

    Copyright Aaron Giles.
    All rights reserved.
***************************************************************************/

/*******************************************************************************************
*  Out Run/X-Board-style sprites
*
*      Offs  Bits               Usage
*       +0   e------- --------  Signify end of sprite list
*       +0   -h-h---- --------  Hide this sprite if either bit is set
*       +0   ----bbb- --------  Sprite bank
*       +0   -------t tttttttt  Top scanline of sprite + 256
*       +2   oooooooo oooooooo  Offset within selected sprite bank
*       +4   ppppppp- --------  Signed 7-bit pitch value between scanlines
*       +4   -------x xxxxxxxx  X position of sprite (position $BE is screen position 0)
*       +6   -s------ --------  Enable shadows
*       +6   --pp---- --------  Sprite priority, relative to tilemaps
*       +6   ------vv vvvvvvvv  Vertical zoom factor (0x200 = full size, 0x100 = half size, 0x300 = 2x size)
*       +8   y------- --------  Render from top-to-bottom (1) or bottom-to-top (0) on screen
*       +8   -f------ --------  Horizontal flip: read the data backwards if set
*       +8   --x----- --------  Render from left-to-right (1) or right-to-left (0) on screen
*       +8   ------hh hhhhhhhh  Horizontal zoom factor (0x200 = full size, 0x100 = half size, 0x300 = 2x size)
*       +E   dddddddd dddddddd  Scratch space for current address
*
*  Out Run only:
*       +A   hhhhhhhh --------  Height in scanlines - 1
*       +A   -------- -ccccccc  Sprite color palette
*
*  X-Board only:
*       +A   ----hhhh hhhhhhhh  Height in scanlines - 1
*       +C   -------- cccccccc  Sprite color palette
*
*  Final bitmap format:
*
*            -s------ --------  Shadow control
*            --pp---- --------  Sprite priority
*            ----cccc cccc----  Sprite color palette
*            -------- ----llll  4-bit pixel data
*
 *******************************************************************************************/

// CPU EOR-walk spillover slice scratch. The legacy path read from a 1 MiB
// resident `sprites[]` member; we instead PI-DMA just the word range this
// extract will touch from /sprites/sprites_native.bin (BE on disk) into BSS
// scratch. The slice is sized by (source_h × |pitch|) + 32-word cushion to
// cover the 32-word/row EOR walk; a 64 KiB scratch fits every observed
// OutRun sprite without clamping. Cost per fallback ≈ 0.2-0.8 ms for typical
// frames vs. ~50 ms for a full bank DMA. Bank-cache thrashing across many
// fallbacks-per-frame is what made the bank-granularity approach unviable.
namespace {
    constexpr uint32_t SPRITE_BANK_WORDS    = 0x10000u;           // 64K words per bank
    constexpr uint32_t SPRITE_BANK_BYTES    = SPRITE_BANK_WORDS * 4u;
    constexpr uint32_t SPRITE_SCRATCH_BYTES = 64u * 1024u;        // 64 KiB
    // 16-byte alignment matches the R4300 D-cache line size, so the
    // data_cache_hit_writeback_invalidate() call below operates on whole
    // lines without touching memory ahead of the buffer.
    alignas(16) uint8_t s_sprite_slice_scratch[SPRITE_SCRATCH_BYTES];
}

hwsprites::hwsprites()
    : sprites_pi_addr(0),
      atlas_pool(nullptr), atlas_pool_bytes(0), atlas_segment_bytes(0),
      atlas_current_segment(0),
      atlas_extracts(0), atlas_hits(0), atlas_overflows(0),
      atlas_segment_retirements(0),
      shadow_body_ring_idx(0)
{
    std::memset(atlas_entries, 0, sizeof(atlas_entries));
    std::memset(atlas_segment_used, 0, sizeof(atlas_segment_used));
}

hwsprites::~hwsprites()
{
    if (atlas_pool)
    {
        free(atlas_pool);
        atlas_pool = nullptr;
    }
}

void hwsprites::init(const uint8_t* src_sprites)
{
    reset();

    // Legacy path byte-swapped src_sprites (the 1 MiB sprite ROM) into a
    // resident sprites[] member. That member is gone — bank slices live on
    // cart and are PI-DMA'd into BSS scratch on EOR-walk fallback. src_sprites
    // is unused here; callers still free it after init returns.
    (void)src_sprites;
}

void hwsprites::reset()
{
    // Clear Sprite RAM buffers
    for (uint16_t i = 0; i < SPRITE_RAM_SIZE; i++)
    {
        ram[i] = 0;
        ramBuff[i] = 0;
    }
}

// Clip areas of the screen in wide-screen mode
void hwsprites::set_x_clip(bool on)
{
    // Clip to central 320 width window.
    if (on)
    {
        x1 = config.s16_x_off;
        x2 = x1 + S16_WIDTH;
    }
    // Allow full wide-screen.
    else
    {
        x1 = 0;
        x2 = config.s16_width;
    }
}

uint8_t hwsprites::read(const uint16_t adr)
{
    uint16_t a = adr >> 1;
    if ((adr & 1) == 1)
        return ram[a] & 0xff;
    else
        return ram[a] >> 8;
}

void hwsprites::write(const uint16_t adr, const uint16_t data)
{
    ram[adr >> 1] = data;
}

// Copy back buffer to main ram, ready for blit
void hwsprites::swap()
{
    uint16_t *src = (uint16_t *)ram;
    uint16_t *dst = (uint16_t *)ramBuff;

    // swap the halves of the road RAM
    for (uint16_t i = 0; i < SPRITE_RAM_SIZE; i++)
    {
        uint16_t temp = *src;
        *src++ = *dst;
        *dst++ = temp;
    }
}

// ============================================================================
// Sprite atlas cache (N64 RDP path)
//
// All sprite rendering flows through render_rdp(): each unique (bank, addr,
// height, pitch) sprite frame is pre-extracted once into a rectangular CI4
// surface — short EOR-terminated rows are padded with 0 (transparent in the
// TLUT). Runtime is just TLUT upload + rdpq_tex_blit with scale + flip.
// Shadow-flagged sprites draw in two passes (darken + body) so the cast
// shadow correctly darkens whatever is on the framebuffer underneath
// (road_fg from the engine scratch composite, plus road_bg / tile layers
// from the prior RDP passes).
//
// Lookup is open-addressed (linear probing) into ATLAS_CAPACITY slots; values
// allocate from a single bump pool sized at init (MAX with MIN fallback). On
// pool overflow the cache resets wholesale (re-extract on next miss) so no
// LRU walk is needed.
// ============================================================================

void hwsprites::atlas_init()
{
    if (atlas_pool) return;

    // Expansion Pak (8 MiB) → 2 MiB pool; base console (4 MiB) → 256 KiB pool.
    atlas_pool_bytes = is_memory_expanded()
                          ? ATLAS_POOL_BYTES_EXPANSION
                          : ATLAS_POOL_BYTES_BASE;
    // Both pool sizes are exact multiples of ATLAS_SEGMENTS=4, so the
    // segments tile the pool with no padding.
    atlas_segment_bytes = atlas_pool_bytes / ATLAS_SEGMENTS;

#if CANNONBALL_LOG_HEAP
    heap_stats_t hs0; sys_get_heap_stats(&hs0);
    debugf("atlas_init: heap before total=%d used=%d free=%d\n",
           hs0.total, hs0.used, hs0.total - hs0.used);
#endif
    atlas_pool = (uint8_t*)memalign(8, atlas_pool_bytes);
    assertf(atlas_pool,
            "hwsprites: atlas_pool memalign(%u) failed — sprite rendering "
            "cannot proceed without a backing pool",
            (unsigned)atlas_pool_bytes);
    atlas_reset();

    // Source sprite ROM (BE on disk = matches PI-DMA target layout). Every
    // sprite extract slices a window of this and EOR-walks it on CPU.
    sprites_pi_addr = dfs_rom_addr("sprites/sprites_native.bin");

#if CANNONBALL_LOG_HEAP
    heap_stats_t hs1; sys_get_heap_stats(&hs1);
    debugf("atlas_init: pool=%p bytes=%u sprites_blob=%08lx heap after used=%d free=%d\n",
           atlas_pool, (unsigned)atlas_pool_bytes,
           (unsigned long)sprites_pi_addr,
           hs1.used, hs1.total - hs1.used);
#else
    debugf("atlas_init: pool=%p bytes=%u sprites_blob=%08lx\n",
           atlas_pool, (unsigned)atlas_pool_bytes,
           (unsigned long)sprites_pi_addr);
#endif
}

void hwsprites::atlas_reset()
{
    std::memset(atlas_entries, 0, sizeof(atlas_entries));
    std::memset(atlas_segment_used, 0, sizeof(atlas_segment_used));
    atlas_current_segment = 0;
}

// Advance the bump ring. Tombstones any real hash entry whose ci4 falls into
// the segment that's about to become current; clears that segment's cursor.
// Caller is responsible for draining queued RDP work before calling — the
// retire invalidates the addresses any in-flight LOAD_BLOCK would reference.
void hwsprites::atlas_advance_segment()
{
    const uint32_t new_seg = (atlas_current_segment + 1) & (ATLAS_SEGMENTS - 1);
    const uint8_t* seg_lo = atlas_pool + (uintptr_t)new_seg * atlas_segment_bytes;
    const uint8_t* seg_hi = seg_lo + atlas_segment_bytes;
    for (uint32_t i = 0; i < ATLAS_CAPACITY; ++i)
    {
        AtlasEntry& e = atlas_entries[i];
        // 0 = empty, 1 = tombstone — neither holds a ci4 reference.
        if (e.key < 2) continue;
        if (e.ci4 >= seg_lo && e.ci4 < seg_hi)
        {
            e.key        = 1;       // tombstone — probes walk past, inserts can reuse
            e.ci4        = nullptr;
            e.w          = 0;
            e.h          = 0;
            e.has_shadow = 0;
        }
    }
    atlas_segment_used[new_seg] = 0;
    atlas_current_segment       = new_seg;
    atlas_segment_retirements++;
}

uint8_t* hwsprites::atlas_pool_alloc(uint32_t bytes, bool drain_on_overflow)
{
    // Each extract must fit in a single segment. On 4 MiB this is 64 KiB;
    // the largest OutRun sprite (256x256 CI4 = 32 KiB) fits with headroom.
    // If this ever fires, either ATLAS_SEGMENTS must shrink or the pool
    // must grow — silently dropping a sprite would corrupt rendering.
    assertf(bytes <= atlas_segment_bytes,
            "hwsprites: extract %u bytes exceeds segment size %u "
            "(pool=%u, segments=%u)",
            (unsigned)bytes, (unsigned)atlas_segment_bytes,
            (unsigned)atlas_pool_bytes, (unsigned)ATLAS_SEGMENTS);

    if (atlas_segment_used[atlas_current_segment] + bytes > atlas_segment_bytes)
    {
        // Segment full. Drain queued LOAD_BLOCK refs before reusing the next
        // segment's pool addresses — see project_spr_spike_atlas_overflow.
        // Pass-1 prepass sets drain_on_overflow=false because no LOAD_BLOCK
        // has been queued against this snapshot yet.
        if (drain_on_overflow)
        {
            rspq_wait();
            atlas_overflows++;
        }
        atlas_advance_segment();
    }
    uint8_t* dst = atlas_pool
                 + (uintptr_t)atlas_current_segment * atlas_segment_bytes
                 + atlas_segment_used[atlas_current_segment];
    atlas_segment_used[atlas_current_segment] += bytes;
    return dst;
}

uint32_t hwsprites::atlas_extract_count()  const { return atlas_extracts; }
uint32_t hwsprites::atlas_hit_count()      const { return atlas_hits; }
uint32_t hwsprites::atlas_overflow_count() const { return atlas_overflows; }
uint32_t hwsprites::atlas_used_bytes()     const
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < ATLAS_SEGMENTS; ++i) s += atlas_segment_used[i];
    return s;
}

// MurmurHash3 finalizer — small and well-distributed for our 64-bit keys.
static inline uint32_t hwsprites_mix64(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

// Pack (bank, addr, source_h, pitch, flip) into a 64-bit key. Bit 63 is set
// so the key is never 0 (empty-slot sentinel). flip is in the key because the
// same ROM bytes decode to mirrored content depending on flip. source_h is in
// the key (rather than height) so sprites sharing source row count share an
// atlas entry even when their nominal output height differs.
static inline uint64_t hwsprites_atlas_key(uint16_t bank, uint16_t addr,
                                           uint16_t source_h, int16_t pitch,
                                           bool flip)
{
    return ((uint64_t)1 << 63)
         | ((uint64_t)(flip ? 1 : 0) << 62)
         | ((uint64_t)(bank     & 0x07) << 56)
         | ((uint64_t)(addr     & 0xffff) << 32)
         | ((uint64_t)(source_h & 0xffff) << 16)
         | ((uint64_t)(uint16_t)pitch);
}

const hwsprites::AtlasEntry* hwsprites::atlas_get_or_extract(
    uint16_t bank, uint16_t addr, uint16_t height, int16_t pitch, bool flip,
    uint16_t vzoom, bool drain_on_overflow)
{
    // Hard fail on any condition that would silently drop a sprite. Callers
    // are responsible for filtering hidden/zero-height slots upstream — see
    // [[feedback-never-silent-drop-content]].
    assertf(atlas_pool,
            "hwsprites::atlas_get_or_extract called before atlas_init "
            "(bank=%u addr=0x%04x h=%u pitch=%d flip=%u vzoom=%u)",
            (unsigned)bank, (unsigned)addr, (unsigned)height,
            (int)pitch, (unsigned)flip, (unsigned)vzoom);
    assertf(height != 0,
            "hwsprites::atlas_get_or_extract height=0 (bank=%u addr=0x%04x "
            "pitch=%d flip=%u vzoom=%u) — caller must filter zero-height "
            "ramBuff slots before invoking the atlas",
            (unsigned)bank, (unsigned)addr, (int)pitch,
            (unsigned)flip, (unsigned)vzoom);
    assertf(vzoom != 0,
            "hwsprites::atlas_get_or_extract vzoom=0 (bank=%u addr=0x%04x "
            "h=%u pitch=%d flip=%u) — caller must clamp vzoom to >= 0x40",
            (unsigned)bank, (unsigned)addr, (unsigned)height,
            (int)pitch, (unsigned)flip);

    // Match CPU render(): row[i] reads source row floor(i * vzoom / 512).
    // After height iterations the last row touched is (height-1)*vzoom/512,
    // so total distinct source rows = that + 1. The on-screen height stays
    // ~= height because rdpq scale_y = 512/vzoom undoes the row stretch.
    const uint32_t source_h32 = ((uint32_t)(height - 1) * (uint32_t)vzoom) / 512u + 1u;
    const uint16_t source_h = (source_h32 > 0xffffu) ? 0xffffu : (uint16_t)source_h32;

    const uint64_t key  = hwsprites_atlas_key(bank, addr, source_h, pitch, flip);
    const uint32_t mask = ATLAS_CAPACITY - 1;
    uint32_t idx = hwsprites_mix64(key) & mask;

    // Bounded linear probe with tombstone-aware insert tracking.
    // Sentinel layout: 0 = empty, 1 = tombstone (slot whose old entry was
    // retired with its segment), >=2^62 = real entry (hwsprites_atlas_key
    // always sets bit 63). The probe walks past tombstones — they might sit
    // between an entry's ideal slot and the slot it actually landed in. On
    // miss, we insert at the first tombstone encountered (if any), else at
    // the empty slot where the probe stopped.
    //
    // Table-full case (no empty, no tombstone seen in 1024 probes) is the
    // rare safety net: wholesale reset, rehash, treat as fresh insert.
    uint32_t insert_idx = ATLAS_CAPACITY;  // sentinel = "not chosen yet"
    for (uint32_t probes = 0; probes < ATLAS_CAPACITY; ++probes)
    {
        const uint64_t k = atlas_entries[idx].key;
        if (k == 0)
        {
            // Empty slot — probe terminates. Use the earliest tombstone we
            // saw if any, else this slot.
            if (insert_idx == ATLAS_CAPACITY) insert_idx = idx;
            break;
        }
        if (k == 1)
        {
            // Tombstone — remember it but keep probing past it (the real
            // entry may live further down the chain).
            if (insert_idx == ATLAS_CAPACITY) insert_idx = idx;
        }
        else if (k == key)
        {
            atlas_hits++;
            return &atlas_entries[idx];
        }
        idx = (idx + 1) & mask;
        if (probes + 1 == ATLAS_CAPACITY)
        {
            // All 1024 slots probed without finding the key and without
            // finding an empty/tombstone — table is fully saturated with
            // real-entry mismatches. Recover identically to the old
            // wholesale overflow path: drain (if caller has LOAD_BLOCKs
            // queued — see project_spr_spike_atlas_overflow), full reset,
            // rehash from scratch.
            atlas_overflows++;
            if (drain_on_overflow) rspq_wait();
            atlas_reset();
            insert_idx = hwsprites_mix64(key) & mask;
            break;
        }
    }

    // ---- Lazy decode: every miss runs the CPU EOR walk and inserts into
    // atlas_entries. The 1024-entry hash cache amortizes the ~500us decode
    // across subsequent frames; steady-state hit rate ≈ 100% after the
    // first frame of any scene. The original "static bake → PI-DMA the
    // pre-decoded blob" path was removed because it required us to find
    // every descriptor offline, including runtime-y_adj-computed addresses
    // that don't appear in rom0 as stored sub-descriptors — see the
    // hwsprites discussion in 1a974f7.
    assertf(sprites_pi_addr,
            "hwsprites: spillover invoked before atlas_init resolved "
            "sprites_native.bin");
    const uint32_t* spritedata;
    {
        const int32_t pitch_s = (int32_t)pitch;
        // Compute the slice in *absolute* blob-word coordinates (bank * 0x10000
        // + bank-relative word) instead of clamping each bank to [0, 0xffff].
        // OutRun's sprite ROM is one contiguous blob laid out as four banks,
        // and the original sprites[] array let a row's EOR walk underflow
        // bank-relative cur into the previous bank — a legitimate cross-bank
        // read used by real sprite data. Clamping per-bank silently truncated
        // those reads to OOB scratch, producing horizontal-line corruption at
        // the top of crowd sprites near bank edges.
        //
        // Walk extension is ASYMMETRIC: the per-row EOR walk only steps in
        // one direction (cur++ for flip=0, cur-- for flip=1), so we need
        // cushion only on the walk side. The legacy 1 MiB sprites[] member
        // sat in zero-initialised BSS, so reads that overran the blob just
        // returned zero — never satisfied the EOR sentinel, walk ran to
        // MAX_WORDS_PER_ROW, and the extra phantom words decoded to slot 0
        // (transparent). We reproduce that by clamping the DMA to blob
        // bounds and pre-zeroing the slack region in scratch.
        constexpr int32_t  WALK_OVERSHOOT  = 32;  // == MAX_WORDS_PER_ROW
        constexpr int64_t  BLOB_LAST_WORD  = (int64_t)SPRITES_LENGTH - 1;
        const int64_t abs_addr  = (int64_t)bank * 0x10000 + (int64_t)addr;
        const int64_t abs_first = abs_addr;
        const int64_t abs_last  =
            abs_addr + ((int64_t)source_h - 1) * pitch_s;
        const int64_t row_lo    = abs_first < abs_last ? abs_first : abs_last;
        const int64_t row_hi    = abs_first > abs_last ? abs_first : abs_last;
        int64_t want_min = flip
            ? (row_lo - WALK_OVERSHOOT) : row_lo;
        const int64_t want_max  = flip
            ? row_hi : (row_hi + WALK_OVERSHOOT);
        // Round want_min down to even (in word units) so dma_byte_off =
        // (dma_lo - want_min) * 4 stays a multiple of 8. An odd want_min
        // leaves dma_byte_off at 4-mod-8, which trips dma_read_async's
        // misalign handler (libdragon/src/dma.c:147): the handler does the
        // leading bytes via a CACHED CPU write while PI DMA writes the rest
        // of the same 16-byte cacheline directly to RAM. The cacheline ends
        // up dirty with correct CPU bytes + stale (pre-DMA) bytes; the EOR
        // walk then reads the stale half from cache and decodes garbage CI4
        // into the atlas. Manifests as horizontal stripe corruption on
        // sprites whose source addr/pitch puts row_lo on an odd word
        // (e.g. Stage 1 palm tree foliage).
        want_min &= ~(int64_t)1;
        // Intersect with blob bounds. dma_lo/dma_hi span the words we'll
        // actually fetch from cart; want_min/want_max span the scratch
        // layout including the legacy-OOB phantom slots.
        int64_t dma_lo = want_min < 0 ? 0 : want_min;
        int64_t dma_hi = want_max > BLOB_LAST_WORD ? BLOB_LAST_WORD : want_max;
        // Caller asked for a fully-out-of-blob slice — that's a real bug:
        // every bank/addr the engine emits should land inside the 1 MiB
        // ROM. See [[feedback-never-silent-drop-content]].
        assertf(dma_lo <= dma_hi,
                "hwsprites: slice fully outside blob (bank=%u addr=%u h=%u "
                "pitch=%d flip=%u want=[%lld..%lld])",
                (unsigned)bank, (unsigned)addr, (unsigned)source_h, (int)pitch,
                (unsigned)flip, (long long)want_min, (long long)want_max);
        // Round dma_lo down (even) / dma_hi up (odd) so the DMA dest offset
        // stays 8-byte aligned and the length is a multiple of 8 bytes.
        // BLOB_LAST_WORD is odd (SPRITES_LENGTH = 0x40000), so |1 caps safely.
        dma_lo &= ~(int64_t)1;
        dma_hi |= (int64_t)1;
        if (dma_hi > BLOB_LAST_WORD) dma_hi = BLOB_LAST_WORD;
        const uint32_t total_words = (uint32_t)(want_max - want_min + 1);
        uint32_t n_bytes = (total_words * 4u + 7u) & ~7u;
        assertf(n_bytes <= SPRITE_SCRATCH_BYTES,
                "hwsprites: slice %u > scratch %u (bank=%u addr=%u h=%u "
                "pitch=%d)",
                (unsigned)n_bytes, (unsigned)SPRITE_SCRATCH_BYTES,
                (unsigned)bank, (unsigned)addr, (unsigned)source_h, (int)pitch);
        // Pre-zero the entire scratch range so any OOB walk slot reads as 0.
        std::memset(s_sprite_slice_scratch, 0, n_bytes);
        const uint32_t dma_word_off = (uint32_t)(dma_lo - want_min);
        const uint32_t dma_byte_off = dma_word_off * 4u;
        const uint32_t dma_words    = (uint32_t)(dma_hi - dma_lo + 1);
        const uint32_t dma_bytes    = dma_words * 4u;          // already 8-aligned
        const uint32_t src = sprites_pi_addr + (uint32_t)dma_lo * 4u;
        // dma_read does NOT touch the CPU cache: it just programs PI and
        // waits. The scratch is a static buffer reused across extracts, so
        // (a) the memset zeros above live only in cache until we push them
        // to RAM, and (b) any cache lines still resident from the previous
        // extract's walk reads would return stale bytes after the PI write.
        // Flush + invalidate the full slice range so RAM gets the zeros in
        // the OOB slack and the CPU reloads fresh data after the DMA.
        data_cache_hit_writeback_invalidate(s_sprite_slice_scratch, n_bytes);
        dma_read(s_sprite_slice_scratch + dma_byte_off, src, dma_bytes);
        // Rebase so spritedata[cur] (cur in bank-relative words) resolves to
        // scratch[(bank*0x10000 + cur - want_min) words]. The offset is signed
        // because want_min can be smaller than bank*0x10000 when last_row
        // sits in a prior bank or when the flip=1 walk extends below it.
        const int64_t rebase_words = (int64_t)bank * 0x10000 - want_min;
        spritedata =
            (const uint32_t*)s_sprite_slice_scratch + (ptrdiff_t)rebase_words;
    }

    // Cap row width at 32 words (256 px). Real OutRun sprites top out below
    // that; the bound stops a malformed/never-terminating row from runaway
    // reads past the bank.
    constexpr int MAX_WORDS_PER_ROW = 32;

    // Pass 1: find max width across all rows. flip selects the word-walk
    // direction and EOR position — see notes above the function.
    int max_words = 0;
    {
        uint32_t row_base = addr;
        for (int row = 0; row < (int)source_h; row++)
        {
            uint32_t cur = row_base;
            int words = 0;
            for (;;)
            {
                uint32_t pixels = flip ? spritedata[cur--] : spritedata[cur++];
                words++;
                const uint32_t eor_mask = flip ? 0x0f000000u : 0x000000f0u;
                if ((pixels & eor_mask) == eor_mask) break;
                if (words >= MAX_WORDS_PER_ROW) break;
            }
            if (words > max_words) max_words = words;
            row_base += (uint32_t)(int32_t)pitch;
        }
    }

    // Pad max_words to even so the CI4 row pitch (w/2 bytes) is always a
    // multiple of 8. That's the LOAD_BLOCK alignment constraint and the
    // texture-image alignment the RDP needs, which lets render_rdp() take the
    // fast bypass path unconditionally. Cost: at most 4 bytes of zero (=
    // transparent) padding per row.
    const int padded_words = (max_words + 1) & ~1;
    const uint16_t w = (uint16_t)(padded_words * 8);
    const uint16_t h = source_h;
    const uint32_t ci4_stride = (uint32_t)w / 2;
    const uint32_t bytes = (ci4_stride * (uint32_t)h + 7u) & ~7u;

    uint8_t* dst = atlas_pool_alloc(bytes, drain_on_overflow);
    std::memset(dst, 0, bytes);

    // Pass 2: decode pixels into CI4. Leftmost pixel of a byte sits in the
    // high nibble — matches the layout the RDP expects on N64. Pixels are
    // emitted in canonical left-to-right screen order regardless of flip, so
    // mirror_x at blit time depends only on xdelta. Tracks whether any pixel
    // resolves to slot 0xa (shadow slot) plus its tight bounding box, so the
    // render path can elide or shrink the 2-cycle shadow darken pass.
    uint8_t  has_shadow = 0;
    uint16_t sh_x0 = 0xFFFF, sh_y0 = 0xFFFF, sh_x1 = 0, sh_y1 = 0;
    {
        uint32_t row_base = addr;
        for (int row = 0; row < (int)h; row++)
        {
            uint32_t cur = row_base;
            uint8_t* row_dst = dst + (uint32_t)row * ci4_stride;
            int col = 0;
            int words = 0;
            for (;;)
            {
                uint32_t pixels = flip ? spritedata[cur--] : spritedata[cur++];
                for (int n = 0; n < 8; n++)
                {
                    // flip=0: MSB-first (28,24,...,0). flip=1: LSB-first (0,4,...,28).
                    uint32_t pix = flip
                        ? ((pixels >> (4 * n)) & 0xf)
                        : ((pixels >> (28 - 4 * n)) & 0xf);
                    if (pix == 0xf) pix = 0;  // EOR sentinel collapses to transparent
                    if (pix == 0xa)
                    {
                        has_shadow = 1;
                        if ((uint16_t)col < sh_x0) sh_x0 = (uint16_t)col;
                        if ((uint16_t)col >= sh_x1) sh_x1 = (uint16_t)(col + 1);
                        if ((uint16_t)row < sh_y0) sh_y0 = (uint16_t)row;
                        if ((uint16_t)row >= sh_y1) sh_y1 = (uint16_t)(row + 1);
                    }
                    uint8_t* bp = row_dst + (col >> 1);
                    if (col & 1) *bp = (uint8_t)((*bp & 0xf0) | pix);
                    else         *bp = (uint8_t)(pix << 4);
                    col++;
                }
                words++;
                const uint32_t eor_mask = flip ? 0x0f000000u : 0x000000f0u;
                if ((pixels & eor_mask) == eor_mask) break;
                if (words >= MAX_WORDS_PER_ROW) break;
            }
            row_base += (uint32_t)(int32_t)pitch;
        }
    }

    data_cache_hit_writeback(dst, bytes);

    AtlasEntry& e = atlas_entries[insert_idx];
    e.key        = key;
    e.ci4        = dst;
    e.w          = w;
    e.h          = h;
    e.has_shadow = has_shadow;
    e.shadow_x0  = sh_x0;
    e.shadow_y0  = sh_y0;
    e.shadow_x1  = sh_x1;
    e.shadow_y1  = sh_y1;
    atlas_extracts++;
    return &e;
}

// RDP path. Walks ramBuff exactly like render() — same priority filter,
// hide/height check, address/pitch/zoom/flip decode, x clamp — but defers
// the actual rasterisation to atlas lookup + rdpq_tex_blit. Shadow-flagged
// sprites draw in two passes: a darken pass (constant 50% multiply, slot 0xa
// only) followed by a body pass (regular TLUT with slot 0xa masked off) so
// the sprite's opaque pixels still render on top of the shadow they cast.
// Diagnostic flag — set to 1 from gdb to dump every render_rdp emit one
// frame at a time. Auto-decrements each call so e.g. set =4 to capture the
// next 4 priority calls (one frame = 4 priorities).
volatile int hwsprites_dump_emit = 0;

void hwsprites::render_rdp(uint8_t priority, const uint16_t* sprite_tlut,
                           int x_offset, int y_offset)
{
    // Lazy-init guard. Normally atlas_init() runs explicitly from main()
    // before ROM load so the pool gets a clean contiguous region; this is
    // just a safety net for unit-test / replay paths that don't.
    if (!atlas_pool) atlas_init();
    assertf(atlas_pool,
            "hwsprites::render_rdp invoked but atlas_pool is NULL after "
            "lazy atlas_init — refusing to silently drop a frame's worth "
            "of sprites");

    // Per-call telemetry — mirrors hwtiles::render_rdp_tile_layers. Locals
    // are incremented at the relevant sites and flushed to n64_profile at
    // function exit so the OUT/PLS logger can correlate spr= cost against
    // sprite count, LOAD_BLOCK count, and TLUT cache miss count.
    const uint32_t spr_pre_prim_count = n64_profile::prim_count;
    const uint32_t spr_pre_ovf        = atlas_overflows;
    const uint64_t spr_t0_us = get_ticks_us();
    uint32_t spr_vis = 0;
    uint32_t spr_loads = 0;
    uint32_t spr_tlut_uploads = 0;

    // ONE-SHOT DIAGNOSTIC — dump every priority-8 sprite's input + emit state
    // for the first 4 calls after activation, then disable. Activated by
    // setting hwsprites_dump_emit=1 from gdb. Logs go to the ISViewer.
    extern volatile int hwsprites_dump_emit;
    int dump_emit = hwsprites_dump_emit;
    if (dump_emit) {
        hwsprites_dump_emit = dump_emit - 1;
        debugf("[spr-dump] call pri=%u  x_off=%d y_off=%d  remaining=%d\n",
               (unsigned)priority, x_offset, y_offset, dump_emit - 1);
        // Dump EVERY slot (including hidden / wrong-priority / zero-height)
        // exactly once per frame — only on the first priority call of a
        // frame, to avoid 4× duplication.
        if (priority == 1u || dump_emit == 4) {
            for (uint16_t da = 0; da < SPRITE_RAM_SIZE; da += 8) {
                uint16_t w0 = ramBuff[da+0];
                if ((w0 & 0x8000) != 0) {
                    debugf("[spr-dump-all] slot=%3u TERM\n", (unsigned)(da/8));
                    break;
                }
                uint16_t hide_r   = (w0 & 0x5000);
                int32_t  height_r = ((ramBuff[da+5] >> 8) & 0xff) + 1;
                uint32_t spr_pri  = 1u << ((ramBuff[da+3] >> 12) & 3);
                int16_t  bank_r   = (w0 >> 9) & 7;
                uint32_t addr_r   = ramBuff[da+1];
                int32_t  pitch_r  = ((ramBuff[da+2] >> 1)
                    | ((ramBuff[da+4] & 0x1000) << 3)) >> 8;
                int32_t  xpos_r   = ramBuff[da+6];
                int32_t  top_r    = (w0 & 0x1ff) - 0x100;
                int32_t  vzoom_r  = ramBuff[da+3] & 0x7ff;
                int32_t  flip_r   = (~ramBuff[da+4] >> 14) & 1;
                int32_t  xdelta_r = ((ramBuff[da+4] & 0x2000) != 0) ? 1 : -1;
                debugf("[spr-dump-all] slot=%3u pri=%lu hide=0x%04x h=%3d "
                       "b=%d a=0x%04x pit=%d flip=%d xpos=%4d top=%4d "
                       "xd=%+d vz=%4d\n",
                       (unsigned)(da/8), (unsigned long)spr_pri,
                       (unsigned)hide_r, (int)height_r,
                       (int)bank_r, (unsigned)addr_r, (int)pitch_r,
                       (int)flip_r, (int)xpos_r, (int)top_r,
                       (int)xdelta_r, (int)vzoom_r);
            }
        }
    }

    // Shadow-mask TLUT: only slot 0xa carries alpha=1 (RGBA5551 LSB), all
    // other entries are alpha=0 and get culled by rdpq_mode_alphacompare.
    // RGB doesn't matter because the shadow combiner outputs constant black.
    static const uint16_t shadow_mask_tlut[16] __attribute__((aligned(8))) = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0001, 0, 0, 0, 0, 0
    };

    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);

    // 0 = opaque pipeline (combiner=TEX, blender disabled),
    // 1 = darken pipeline (combiner outputs black/TEX0_alpha, blender = MULTIPLY_CONST).
    int pipeline = 0;
    shadow_body_ring_idx = 0;

    // Multi-slot CI4 TLUT cache mirrored in TMEM. CI4 has 16 palette slots
    // and TILE.palette is a 4-bit field, so we can hold up to 16 simultaneous
    // 16-entry palettes in the high half of TMEM and pick one per draw via
    // set_tile (1 RDP command) instead of re-uploading a full TLUT each draw
    // (3 RDP commands: set_texture_image_raw + set_tile + load_tile). Layout:
    //   slot 0      : shadow_mask_tlut (pinned, uploaded once per render call)
    //   slot 1      : shadow body scratch (always re-uploaded — contents vary
    //                 per sprite, so no point caching by tag)
    //   slot 2..15  : LRU cache of opaque color_tlut palettes, keyed by the
    //                 sprite_tlut+color*16 pointer (stable across the render
    //                 call). Per OutRun profile only a handful of distinct
    //                 sprite colours appear per frame, so 14 slots covers the
    //                 working set with near-100% hit rate after warm-up.
    constexpr int TLUT_SLOT_SHADOW_MASK = 0;
    constexpr int TLUT_SLOT_SHADOW_BODY = 1;
    constexpr int TLUT_SLOT_OPAQUE_BASE = 2;
    constexpr int TLUT_N_OPAQUE_SLOTS   = 14;
    const uint16_t* opaque_tag[TLUT_N_OPAQUE_SLOTS] = {};
    uint32_t        opaque_seq[TLUT_N_OPAQUE_SLOTS] = {};
    uint32_t        next_seq = 1;
    bool            shadow_mask_loaded = false;
    int             cur_tile_palette = -1;  // last palette bound to TILE0
    // Single-element tag→slot cache in front of the 14-slot LRU. OutRun
    // sprite batches frequently share a palette (e.g. tree clusters), so this
    // skips the linear scan on repeat hits.
    const uint16_t* prev_opaque_tag = NULL;
    int             prev_opaque_slot = 0;

    // Pixel-load cache. The bypass path below configures TILE0 (CI4 draw
    // view) + TILE1 (RGBA16 LOAD_BLOCK view) and runs LOAD_BLOCK once per
    // unique atlas surface. Identity-comparison on (ci4 ptr, w, h) is enough:
    // atlas entries live in a bump pool, so a new entry always has a new ptr.
    // Crucially, for shadow sprites this lets the mask+body pair share a
    // single pixel load — the two passes only differ by TLUT.
    const uint8_t* last_atlas_ci4 = NULL;
    uint16_t       last_atlas_w   = 0;
    uint16_t       last_atlas_h   = 0;

    const uint32_t numbanks = SPRITES_LENGTH / 0x10000;

    // -------------------------------------------------------------------
    // Pass 1 — atlas prepass. Walk this priority's sprites and call
    // atlas_get_or_extract to populate the pool, but emit nothing. The
    // overflow-recovery rspq_wait() drain is skipped here because no
    // LOAD_BLOCK has been queued against the pool yet — there is nothing
    // in flight that could be reading the about-to-be-recycled addresses.
    // This is the whole point of the 2-pass split: on 4 MiB the 128 KiB
    // pool was overflowing ~1.2x per frame inside pass 2, and each drain
    // stalled the CPU on the full RDP queue. Pass 1 absorbs the overflow
    // cost (just memset+ptr reset, ~µs) so pass 2 sees a stable pool.
    //
    // When the working set fits in the pool, pass 2 hits the atlas 100%
    // and never overflows — zero drains, no extract cost. When the
    // working set exceeds the pool, pass 2 still misses and can overflow;
    // we keep the drain there for correctness. Worst case = today.
    //
    // CPU cost of pass 1: re-parses ramBuff and re-derives bank/addr/
    // pitch/flip/vzoom, which is ~10us total across ~60 visible sprites.
    // Atlas hits in pass 1 are O(1) hash probes. The duplicate parse is
    // a rounding error against the saved drain cost.
    // -------------------------------------------------------------------
    for (uint16_t data = 0; data < SPRITE_RAM_SIZE; data += 8)
    {
        if ((ramBuff[data+0] & 0x8000) != 0) break;

        uint32_t sprpri = 1u << ((ramBuff[data+3] >> 12) & 3);
        if (sprpri != priority) continue;

        int16_t hide   = (ramBuff[data+0] & 0x5000);
        int32_t height = (ramBuff[data+5] >> 8) + 1;
        if (hide != 0 || height == 0) continue;

        int16_t  bank   = (ramBuff[data+0] >> 9) & 7;
        uint32_t addr   = ramBuff[data+1];
        int32_t  pitch  = ((ramBuff[data+2] >> 1) | ((ramBuff[data+4] & 0x1000) << 3)) >> 8;
        int32_t  vzoom  = ramBuff[data+3] & 0x7ff;
        int32_t  flip   = (~ramBuff[data+4] >> 14) & 1;

        if (numbanks) bank %= numbanks;
        if (vzoom < 0x40) vzoom = 0x40;

        const AtlasEntry* warmed = atlas_get_or_extract(
            (uint16_t)bank, (uint16_t)addr,
            (uint16_t)height, (int16_t)pitch, flip != 0,
            (uint16_t)vzoom, /*drain_on_overflow=*/false);
        // Pass 1 must succeed: the pool was sized to absorb a full priority's
        // working set with the no-drain reset path. A null here means the pool
        // is mis-sized or atlas extraction broke. Don't silently continue —
        // pass 2 would assert anyway but the failure mode is murkier without
        // the pass-1 context. See [[feedback-never-silent-drop-content]].
        assertf(warmed,
                "hwsprites: pass-1 atlas miss for enabled sprite slot %u "
                "(bank=%u addr=0x%04x h=%d pitch=%d flip=%d vzoom=%d)",
                (unsigned)(data / 8),
                (unsigned)bank, (unsigned)addr, (int)height,
                (int)pitch, (int)flip, (int)vzoom);
    }

    // After pass 1 the atlas should hold every sprite for this priority. If
    // pass 2 below sees ANY further overflow, the working set exceeds the
    // pool — drain + reset evicts entries pass 2 had every reason to expect
    // would still be live, and we silently render with wrong CI4 data. Snap
    // the counter and assert no change at end-of-pass-2.
    const uint32_t overflows_before_pass2 = atlas_overflows;

    // -------------------------------------------------------------------
    // Pass 2 — emit. Same walk + parse, but now also computes screen
    // coordinates, runs the frustum cull, manages the TLUT/atlas/pipeline
    // bypass cache, and emits LOAD_BLOCK + texture rectangles.
    // -------------------------------------------------------------------
    for (uint16_t data = 0; data < SPRITE_RAM_SIZE; data += 8)
    {
        if ((ramBuff[data+0] & 0x8000) != 0) break;

        uint32_t sprpri = 1u << ((ramBuff[data+3] >> 12) & 3);
        if (sprpri != priority) continue;

        int16_t hide   = (ramBuff[data+0] & 0x5000);
        int32_t height = (ramBuff[data+5] >> 8) + 1;
        if (hide != 0 || height == 0) continue;

        // All visible pri=8 sprites flow through RDP so sprite-RAM slot order
        // (truck behind tree, etc.) is preserved across the shadow/non-shadow
        // split. Shadow sprites switch into a 50%-darken pipeline below — but
        // sprites whose CI4 has no slot-0xa pixels (detected at extract time)
        // would render the shadow pass as a no-op, so we collapse to the
        // single-pass body path even when the engine flagged shadow=1.
        bool shadow = ((ramBuff[data+3] >> 14) & 1) != 0;

        int16_t  bank   = (ramBuff[data+0] >> 9) & 7;
        int32_t  top    = (ramBuff[data+0] & 0x1ff) - 0x100;
        uint32_t addr   = ramBuff[data+1];
        int32_t  pitch  = ((ramBuff[data+2] >> 1) | ((ramBuff[data+4] & 0x1000) << 3)) >> 8;
        int32_t  xpos   = ramBuff[data+6];
        int32_t  vzoom  = ramBuff[data+3] & 0x7ff;
        int32_t  ydelta = ((ramBuff[data+4] & 0x8000) != 0) ? 1 : -1;
        int32_t  flip   = (~ramBuff[data+4] >> 14) & 1;
        int32_t  xdelta = ((ramBuff[data+4] & 0x2000) != 0) ? 1 : -1;
        int32_t  hzoom  = ramBuff[data+4] & 0x7ff;
        int32_t  color  = (ramBuff[data+5] & 0x7f);  // sprite_tlut slot

        if (xpos < 0x80 && xdelta < 0) xpos += 0x200;
        xpos -= 0xbe;

        if (numbanks) bank %= numbanks;

        if (vzoom < 0x40) vzoom = 0x40;
        if (hzoom < 0x40) hzoom = 0x40;

        xpos += config.s16_x_off;

        // Pass 2 (emit). drain_on_overflow=true: any miss here that triggers
        // overflow recovery must rspq_wait() first because we've already
        // queued LOAD_BLOCKs against the pool earlier in this same loop.
        const AtlasEntry* e = atlas_get_or_extract(
            (uint16_t)bank, (uint16_t)addr,
            (uint16_t)height, (int16_t)pitch, flip != 0,
            (uint16_t)vzoom, /*drain_on_overflow=*/true);
        // No `if (!e) continue;` — atlas_get_or_extract is contractually
        // required to produce an entry for every enabled sprite slot the
        // engine emits. Silently skipping here would drop content (e.g.
        // overpass scenery sub-sprites) without any visible signal. See
        // [[feedback-never-silent-drop-content]].
        assertf(e,
                "hwsprites: pass-2 atlas miss for enabled sprite slot %u "
                "(bank=%u addr=0x%04x h=%d pitch=%d flip=%d vzoom=%d)",
                (unsigned)(data / 8),
                (unsigned)bank, (unsigned)addr, (int)height,
                (int)pitch, (int)flip, (int)vzoom);
        // Non-null isn't enough — an entry with zero geometry or a null CI4
        // pointer would silently render an empty rect. Catch the extractor
        // bug rather than the downstream "missing sprite" symptom.
        assertf(e->w > 0 && e->h > 0 && e->ci4 != nullptr,
                "hwsprites: pass-2 atlas entry corrupt slot %u "
                "(bank=%u addr=0x%04x w=%u h=%u ci4=%p)",
                (unsigned)(data / 8),
                (unsigned)bank, (unsigned)addr,
                (unsigned)e->w, (unsigned)e->h, (const void*)e->ci4);

        // Shadow pass is a 2-cycle RDP operation (darken blender). If this
        // sprite carries no shadow silhouette in its CI4 data, the pass
        // produces zero visible writes — skip it.
        if (shadow && !e->has_shadow)
        {
            shadow = false;
        }

        const float scale_x  = 512.0f / (float)hzoom;
        const float scale_y  = 512.0f / (float)vzoom;
        const float zoomed_w = (float)e->w * scale_x;
        const float zoomed_h = (float)e->h * scale_y;


        // The atlas is always extracted in canonical left-to-right order, so
        // mirror axes depend purely on xdelta/ydelta sign.
        const bool mirror_x = (xdelta < 0);
        const bool mirror_y = (ydelta < 0);

        const float screen_x = (xdelta > 0)
            ? (float)xpos
            : ((float)xpos - zoomed_w + 1.0f);
        const float screen_y = (ydelta > 0)
            ? (float)top
            : ((float)top  - zoomed_h + 1.0f);

        bool culled = false;
        const char* cull_reason = "";
        if      (screen_x + zoomed_w <= 0.0f)        { culled = true; cull_reason = "left-of-screen"; }
        else if (screen_x >= (float)config.s16_width){ culled = true; cull_reason = "right-of-screen"; }
        else if (screen_y + zoomed_h <= 0.0f)        { culled = true; cull_reason = "above-screen"; }
        else if (screen_y >= (float)config.s16_height){culled = true; cull_reason = "below-screen"; }

        if (dump_emit) {
            debugf("[spr-dump] slot=%3u b=%u a=0x%04x h=%3d pit=%2d flip=%d "
                   "xpos=%4d top=%4d xd=%+d vz=%4d hz=%4d "
                   "atlas=%ux%u zw=%5.1f zh=%5.1f screen=(%5.1f,%5.1f) "
                   "cull=%s%s\n",
                   (unsigned)(data/8),
                   (unsigned)bank, (unsigned)addr, (int)height, (int)pitch, (int)flip,
                   (int)xpos, (int)top, (int)xdelta, (int)vzoom, (int)hzoom,
                   (unsigned)e->w, (unsigned)e->h,
                   (double)zoomed_w, (double)zoomed_h,
                   (double)screen_x, (double)screen_y,
                   culled ? "Y:" : "N", cull_reason);
        }
        if (culled) continue;

        spr_vis++;

        const float dst_x = screen_x + (float)x_offset;
        const float dst_y = screen_y + (float)y_offset;

        // Sprite TLUT layout: 128 slots x 16 entries (engine sprite palette
        // is 0x800 + color*16 + pix).
        const uint16_t* color_tlut = &sprite_tlut[(uint32_t)color * 16];

        // ---- Bypass rdpq_tex_blit ----------------------------------------
        // Atlas widths are padded so the CI4 row pitch (w/2) is always a
        // multiple of 8, so we can always run LOAD_BLOCK without the
        // strip-walker dispatch. The 4 KB TMEM holds 2 KB of TLUT + 2 KB of
        // pixels (LSB), so any sprite whose CI4 fits 2048 bytes (= w*h ≤
        // 4096 source pixels) loads in a single shot. Anything bigger is
        // routed back to rdpq_tex_blit; per profile, this fallback fires on
        // ~5% of sprite draws.
        const uint32_t ci4_stride = (uint32_t)e->w / 2;
        const uint32_t ci4_bytes  = ci4_stride * (uint32_t)e->h;

        if (ci4_bytes <= 2048u)
        {
            bool atlas_changed =
                (e->ci4 != last_atlas_ci4) || (e->w != last_atlas_w) || (e->h != last_atlas_h);

            // Flip via swapped dst extents (matches the convention rdpq_tex_blit
            // uses internally — see tex_xblit_norotate in rdpq_tex.c).
            float x0 = dst_x;
            float x1 = dst_x + zoomed_w;
            float y0 = dst_y;
            float y1 = dst_y + zoomed_h;
            if (mirror_x) { float t = x0; x0 = x1; x1 = t; }
            if (mirror_y) { float t = y0; y0 = y1; y1 = t; }

            // Helper: bind a palette slot to TILE0 (and emit the LOAD_BLOCK
            // + tile pair if the atlas changed). Folds the palette change
            // into the atlas-change set_tile when both happen at once.
            auto bind_tile0 = [&](int slot) {
                if (atlas_changed)
                {
                    rdpq_set_texture_image_raw(0, PhysicalAddr(e->ci4),
                                               FMT_RGBA16, (e->w + 1) / 4, e->h);
                    rdpq_set_tile(TILE1, FMT_RGBA16, 0, 0, NULL);
                    rdpq_tileparms_t parms = {};
                    parms.palette = (uint8_t)slot;
                    rdpq_set_tile(TILE0, FMT_CI4, 0, (uint16_t)ci4_stride, &parms);
                    rdpq_load_block(TILE1, 0, 0,
                                    (uint16_t)(ci4_bytes / 2),  // RGBA16 texels = w*h/4
                                    (uint16_t)ci4_stride);
                    rdpq_set_tile_size(TILE0, 0, 0, e->w, e->h);
                    spr_loads++;
                    last_atlas_ci4 = e->ci4;
                    last_atlas_w   = e->w;
                    last_atlas_h   = e->h;
                    cur_tile_palette = slot;
                    atlas_changed = false;  // shadow pass 2 sees same atlas
                }
                else if (slot != cur_tile_palette)
                {
                    rdpq_tileparms_t parms = {};
                    parms.palette = (uint8_t)slot;
                    rdpq_set_tile(TILE0, FMT_CI4, 0, (uint16_t)ci4_stride, &parms);
                    cur_tile_palette = slot;
                }
            };

            if (shadow)
            {
                // Pass 1 — shadow darken. Constant-50% multiply pipeline.
                if (pipeline != 1)
                {
                    rdpq_set_fog_color(RGBA32(0, 0, 0, 128));
                    rdpq_mode_combiner(
                        RDPQ_COMBINER1((0, 0, 0, 0), (0, 0, 0, TEX0)));
                    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY_CONST);
                    pipeline = 1;
                }
                if (!shadow_mask_loaded)
                {
                    rdpq_tex_upload_tlut((uint16_t*)shadow_mask_tlut,
                                         TLUT_SLOT_SHADOW_MASK * 16, 16);
                    shadow_mask_loaded = true;
                    spr_tlut_uploads++;
                }
                bind_tile0(TLUT_SLOT_SHADOW_MASK);
                // Tight shadow rect — the darken pass uses a 2-cycle blender,
                // so trimming the rasterised area to the slot-0xa bounding box
                // is roughly twice as valuable per pixel as the body pass.
                {
                    const uint16_t sx0 = e->shadow_x0;
                    const uint16_t sy0 = e->shadow_y0;
                    const uint16_t sx1 = e->shadow_x1;
                    const uint16_t sy1 = e->shadow_y1;
                    const float sdx0 = mirror_x
                        ? (dst_x + zoomed_w - (float)sx0 * scale_x)
                        : (dst_x + (float)sx0 * scale_x);
                    const float sdx1 = mirror_x
                        ? (dst_x + zoomed_w - (float)sx1 * scale_x)
                        : (dst_x + (float)sx1 * scale_x);
                    const float sdy0 = mirror_y
                        ? (dst_y + zoomed_h - (float)sy0 * scale_y)
                        : (dst_y + (float)sy0 * scale_y);
                    const float sdy1 = mirror_y
                        ? (dst_y + zoomed_h - (float)sy1 * scale_y)
                        : (dst_y + (float)sy1 * scale_y);
                    rdpq_texture_rectangle_scaled(TILE0, sdx0, sdy0, sdx1, sdy1,
                                                  sx0, sy0, sx1, sy1);
                    n64_profile::prim_count++;
                }

                // Pass 2 — opaque body. Pixel data already in TMEM from the
                // mask pass, so only the TLUT and combiner/blender need to
                // change. This is the central PF11 win: shadow sprites went
                // from 2 × LOAD_BLOCK to 1 × LOAD_BLOCK.
                if (pipeline != 0)
                {
                    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
                    rdpq_mode_blender(0);
                    pipeline = 0;
                }
                uint16_t* scratch =
                    &shadow_body_tluts[shadow_body_ring_idx * 16];
                if (++shadow_body_ring_idx >= SHADOW_TLUT_RING)
                    shadow_body_ring_idx = 0;
                uint16_t* scratch_uc = (uint16_t*)UncachedAddr(scratch);
                for (int i = 0; i < 16; i++) scratch_uc[i] = color_tlut[i];
                scratch_uc[10] = 0;
                rdpq_tex_upload_tlut(scratch, TLUT_SLOT_SHADOW_BODY * 16, 16);
                spr_tlut_uploads++;
                bind_tile0(TLUT_SLOT_SHADOW_BODY);
                rdpq_texture_rectangle_scaled(TILE0, x0, y0, x1, y1,
                                              0, 0, e->w, e->h);
                n64_profile::prim_count++;
            }
            else
            {
                if (pipeline != 0)
                {
                    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
                    rdpq_mode_blender(0);
                    pipeline = 0;
                }
                // Look up color_tlut in the 14-slot LRU. The tag is the
                // sprite_tlut+color*16 pointer — stable across the call.
                int slot;
                if (color_tlut == prev_opaque_tag)
                {
                    slot = prev_opaque_slot;
                }
                else
                {
                    slot = -1;
                    uint32_t best_seq = ~0u;
                    int best_i = 0;
                    for (int i = 0; i < TLUT_N_OPAQUE_SLOTS; i++)
                    {
                        if (opaque_tag[i] == color_tlut) { slot = i; break; }
                        if (opaque_seq[i] < best_seq)
                        {
                            best_seq = opaque_seq[i];
                            best_i = i;
                        }
                    }
                    if (slot < 0)
                    {
                        slot = best_i;
                        rdpq_tex_upload_tlut((uint16_t*)color_tlut,
                                             (TLUT_SLOT_OPAQUE_BASE + slot) * 16, 16);
                        opaque_tag[slot] = color_tlut;
                        spr_tlut_uploads++;
                    }
                    prev_opaque_tag  = color_tlut;
                    prev_opaque_slot = slot;
                }
                opaque_seq[slot] = ++next_seq;
                bind_tile0(TLUT_SLOT_OPAQUE_BASE + slot);
                rdpq_texture_rectangle_scaled(TILE0, x0, y0, x1, y1,
                                              0, 0, e->w, e->h);
                n64_profile::prim_count++;
            }
            continue;
        }

        // ---- Custom strip walker (sprite too big to fit one TMEM strip)
        //
        // We used to call rdpq_tex_blit here. It silently failed to emit
        // any primitives for some big CI4 sprites with scale + no flip
        // (overpass top-beam right half on stage 1, Japan), causing
        // visible holes in scenery. Rather than try to localize the
        // libdragon bug, we walk strips manually: LOAD_BLOCK as many full
        // rows as fit in 2 KB of TMEM, then emit one rdpq_texture_
        // rectangle_scaled per strip — the exact pattern the bypass path
        // uses, just repeated across the sprite. Uses the same LRU TLUT
        // cache as the bypass path so there's no palette-zero collision.
        //
        // [[feedback-never-silent-drop-content]] — this path must not
        // silently no-op for any (w, h) tuple.
        const uint32_t fb_pre_prims = n64_profile::prim_count;
        const uint32_t rows_per_strip = 2048u / ci4_stride;
        assertf(rows_per_strip > 0,
                "hwsprites: atlas stride %u exceeds 2 KB TMEM (w=%u h=%u)",
                (unsigned)ci4_stride, (unsigned)e->w, (unsigned)e->h);

        auto emit_strips = [&](int palette_slot,
                               int sx0, int sy0, int sx1, int sy1) {
            // Per-strip LOAD_BLOCK + texture_rectangle. (sx0,sy0)–(sx1,sy1)
            // is the requested source sub-rect (used for shadow tight
            // bbox); the strip walker clamps each strip to its rows. We
            // bind TILE0/TILE1 once and only re-issue set_texture_image_raw
            // + LOAD_BLOCK per strip.
            rdpq_set_tile(TILE1, FMT_RGBA16, 0, 0, NULL);
            rdpq_tileparms_t tparms = {};
            tparms.palette = (uint8_t)palette_slot;
            rdpq_set_tile(TILE0, FMT_CI4, 0, (uint16_t)ci4_stride, &tparms);
            cur_tile_palette = palette_slot;

            const int row_lo = sy0;
            const int row_hi = sy1;
            for (int row = row_lo; row < row_hi; ) {
                int strip_h = (int)rows_per_strip;
                if (row + strip_h > row_hi) strip_h = row_hi - row;

                uint8_t* strip_src = e->ci4 + (uint32_t)row * ci4_stride;
                uint32_t strip_texels =
                    ((uint32_t)strip_h * ci4_stride) / 2;  // RGBA16 texels

                rdpq_set_texture_image_raw(0, PhysicalAddr(strip_src),
                                           FMT_RGBA16,
                                           (e->w + 1) / 4, strip_h);
                rdpq_load_block(TILE1, 0, 0,
                                (uint16_t)strip_texels,
                                (uint16_t)ci4_stride);
                rdpq_set_tile_size(TILE0, 0, 0, e->w, strip_h);

                // Local strip coords (TMEM is rebased to row 0 per strip).
                int strip_sx0 = sx0;
                int strip_sx1 = sx1;
                int strip_ty0 = 0;
                int strip_ty1 = strip_h;

                // Destination y-range for this strip in screen space.
                const float dstrip_y0 = dst_y + (float)row       * scale_y;
                const float dstrip_y1 = dst_y + (float)(row + strip_h) * scale_y;
                // X range covers the requested sub-rect, scaled to screen.
                const float dstrip_x0 = dst_x + (float)sx0 * scale_x;
                const float dstrip_x1 = dst_x + (float)sx1 * scale_x;

                float rx0 = dstrip_x0, rx1 = dstrip_x1;
                float ry0 = dstrip_y0, ry1 = dstrip_y1;
                if (mirror_x) { float t = rx0; rx0 = rx1; rx1 = t; }
                if (mirror_y) { float t = ry0; ry0 = ry1; ry1 = t; }

                rdpq_texture_rectangle_scaled(
                    TILE0, rx0, ry0, rx1, ry1,
                    strip_sx0, strip_ty0, strip_sx1, strip_ty1);
                n64_profile::prim_count++;
                spr_loads++;

                row += strip_h;
            }
        };

        if (shadow)
        {
            // Pass 1: shadow darken. Uses the slot-0xa-only TLUT in slot 0.
            if (pipeline != 1)
            {
                rdpq_set_fog_color(RGBA32(0, 0, 0, 128));
                rdpq_mode_combiner(
                    RDPQ_COMBINER1((0, 0, 0, 0), (0, 0, 0, TEX0)));
                rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY_CONST);
                pipeline = 1;
            }
            if (!shadow_mask_loaded)
            {
                rdpq_tex_upload_tlut((uint16_t*)shadow_mask_tlut,
                                     TLUT_SLOT_SHADOW_MASK * 16, 16);
                shadow_mask_loaded = true;
                spr_tlut_uploads++;
            }
            emit_strips(TLUT_SLOT_SHADOW_MASK,
                        e->shadow_x0, e->shadow_y0,
                        e->shadow_x1, e->shadow_y1);

            // Pass 2: opaque body. Reload mode and a fresh body TLUT (slot
            // 0xa zeroed) into the shadow-body ring.
            if (pipeline != 0)
            {
                rdpq_mode_combiner(RDPQ_COMBINER_TEX);
                rdpq_mode_blender(0);
                pipeline = 0;
            }
            uint16_t* scratch =
                &shadow_body_tluts[shadow_body_ring_idx * 16];
            if (++shadow_body_ring_idx >= SHADOW_TLUT_RING)
                shadow_body_ring_idx = 0;
            uint16_t* scratch_uc = (uint16_t*)UncachedAddr(scratch);
            for (int i = 0; i < 16; i++) scratch_uc[i] = color_tlut[i];
            scratch_uc[10] = 0;
            rdpq_tex_upload_tlut(scratch, TLUT_SLOT_SHADOW_BODY * 16, 16);
            spr_tlut_uploads++;
            emit_strips(TLUT_SLOT_SHADOW_BODY, 0, 0, e->w, e->h);
        }
        else
        {
            if (pipeline != 0)
            {
                rdpq_mode_combiner(RDPQ_COMBINER_TEX);
                rdpq_mode_blender(0);
                pipeline = 0;
            }
            // Same opaque-LRU lookup the bypass path uses.
            int slot;
            if (color_tlut == prev_opaque_tag)
            {
                slot = prev_opaque_slot;
            }
            else
            {
                slot = -1;
                uint32_t best_seq = ~0u;
                int best_i = 0;
                for (int i = 0; i < TLUT_N_OPAQUE_SLOTS; i++)
                {
                    if (opaque_tag[i] == color_tlut) { slot = i; break; }
                    if (opaque_seq[i] < best_seq)
                    {
                        best_seq = opaque_seq[i];
                        best_i = i;
                    }
                }
                if (slot < 0)
                {
                    slot = best_i;
                    rdpq_tex_upload_tlut((uint16_t*)color_tlut,
                                         (TLUT_SLOT_OPAQUE_BASE + slot) * 16, 16);
                    opaque_tag[slot] = color_tlut;
                    spr_tlut_uploads++;
                }
                prev_opaque_tag  = color_tlut;
                prev_opaque_slot = slot;
            }
            opaque_seq[slot] = ++next_seq;
            emit_strips(TLUT_SLOT_OPAQUE_BASE + slot, 0, 0, e->w, e->h);
        }
        // We re-bound TILE0/TILE1, so invalidate the bypass cache so the
        // next regular-bypass sprite re-binds (cur_tile_palette was set
        // inside emit_strips for diagnostic clarity).
        last_atlas_ci4 = NULL;

        if (dump_emit) {
            const uint32_t emitted = n64_profile::prim_count - fb_pre_prims;
            debugf("[spr-dump]   STRIPS slot=%u w=%u h=%u ci4_b=%u "
                   "rows/strip=%u scale=(%.3f,%.3f) flip=(%d,%d) "
                   "dst=(%.1f,%.1f) shadow=%d has_shadow=%u "
                   "prims=%u\n",
                   (unsigned)(data/8),
                   (unsigned)e->w, (unsigned)e->h, (unsigned)ci4_bytes,
                   (unsigned)rows_per_strip,
                   (double)scale_x, (double)scale_y,
                   (int)mirror_x, (int)mirror_y,
                   (double)dst_x, (double)dst_y,
                   (int)shadow, (unsigned)e->has_shadow,
                   (unsigned)emitted);
        }
    }


    // Pass 1 is supposed to absorb any overflow; pass 2's drain reset is a
    // safety net but if it fires it has just evicted entries pass 2 itself
    // queued LOAD_BLOCKs against earlier in this same loop — silent wrong-
    // pixels. The pool size was tuned to cover the per-priority working set
    // on real ROM data; bumping into this means the working set grew (new
    // descriptor variants, larger zoom range, etc.) and the pool needs to
    // grow too. See [[project-spr-spike-atlas-overflow]].
    assertf(atlas_overflows == overflows_before_pass2,
            "hwsprites: pass-2 atlas overflow (working set > pool); "
            "pool exhaustion during emit will silently corrupt sprites");

    n64_profile::spr_call_vis          = spr_vis;
    n64_profile::spr_call_loads        = spr_loads;
    n64_profile::spr_call_tlut_uploads = spr_tlut_uploads;
    n64_profile::spr_call_prims        = n64_profile::prim_count - spr_pre_prim_count;
    n64_profile::spr_call_ovf          = atlas_overflows - spr_pre_ovf;
    n64_profile::spr_call_us           = (uint32_t)(get_ticks_us() - spr_t0_us);
}
