#include "video.hpp"
#include "hwvideo/hwsprites.hpp"
#include "hwvideo/hwsprites_baked.h"
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
}

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

hwsprites::hwsprites()
    : atlas_pool(nullptr), atlas_used(0),
      atlas_extracts(0), atlas_hits(0), atlas_overflows(0),
      baked_blob_pi_addr(0), baked_extracts(0),
      shadow_body_ring_idx(0)
{
    std::memset(atlas_entries, 0, sizeof(atlas_entries));
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

    if (src_sprites)
    {
        // Convert S16 tiles to a more useable format
        const uint8_t *spr = src_sprites;

        for (uint32_t i = 0; i < SPRITES_LENGTH; i++)
        {
            uint8_t d3 = *spr++;
            uint8_t d2 = *spr++;
            uint8_t d1 = *spr++;
            uint8_t d0 = *spr++;

            sprites[i] = (d0 << 24) | (d1 << 16) | (d2 << 8) | d3;
        }
    }

    // Atlas pool allocation is deferred to the first render_rdp() call. By the
    // time we render, Video::init has freed the ROM source buffers (sprites/
    // tiles/road, ~2.5 MiB), leaving a much larger contiguous block for the
    // atlas. Allocating here failed on N64 even with Expansion Pak — heap was
    // too fragmented during boot.
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

        if (config.video.hires)
        {
            x1 <<= 1;
            x2 <<= 1;
        }
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
    // Try the max size first; fall back to MIN if the heap can't honour it.
    atlas_pool_bytes = ATLAS_POOL_BYTES_MAX;
    atlas_pool = (uint8_t*)memalign(8, atlas_pool_bytes);
    if (!atlas_pool) {
        atlas_pool_bytes = ATLAS_POOL_BYTES_MIN;
        atlas_pool = (uint8_t*)memalign(8, atlas_pool_bytes);
    }
    atlas_reset();

    // Resolve the cart-side baked atlas blob. dfs_rom_addr returns a PI
    // address into ROM space (0x10000000+) that we PI-DMA from on cache
    // miss. Returns 0 if the file is missing — extracts fall back to the
    // CPU EOR-walk spillover and we lose the speedup but stay correct.
    baked_blob_pi_addr = dfs_rom_addr("sprites/sprite_atlas.bin");

    debugf("atlas_init: pool=%p bytes=%u baked_blob=%08lx entries=%lu\n",
           atlas_pool, (unsigned)atlas_pool_bytes,
           (unsigned long)baked_blob_pi_addr,
           (unsigned long)hwsprites_baked_count);
}

struct HwspritesBakedHit {
    const HwspritesBakedEntry* be;
    uint16_t row_offset;   // request addr = be->addr + row_offset * pitch
};

// Predecessor search on the sorted baked index (key = bank, flip, pitch, addr).
// A direct hit (row_offset = 0) covers descriptor-table entries; a partial hit
// (row_offset > 0) covers the runtime `inc_offset(y_adj)` path — when a sprite
// is partially clipped at the top, the runtime computes addr' = addr + y_adj
// * pitch and asks for a row-aligned suffix of the same baked blob.
static HwspritesBakedHit hwsprites_baked_lookup(
    uint16_t bank, uint16_t addr, int16_t pitch, bool flip,
    uint16_t source_h)
{
    HwspritesBakedHit miss = {nullptr, 0};
    if (hwsprites_baked_count == 0) return miss;
    const uint8_t flip_v = flip ? 1 : 0;

    // upper_bound by (bank, flip, pitch, addr) — find smallest index whose
    // key > (bank, flip_v, pitch, addr). The candidate is index-1.
    uint32_t lo = 0;
    uint32_t hi = hwsprites_baked_count;
    while (lo < hi)
    {
        uint32_t mid = (lo + hi) >> 1;
        const HwspritesBakedEntry& e = hwsprites_baked_index[mid];
        bool le;
        if      (e.bank  != bank)   le = (e.bank  < bank);
        else if (e.flip  != flip_v) le = (e.flip  < flip_v);
        else if (e.pitch != pitch)  le = (e.pitch < pitch);
        else                        le = (e.addr <= addr);
        if (le) lo = mid + 1;
        else    hi = mid;
    }
    if (lo == 0) return miss;

    // Walk back through entries with matching (bank, flip, pitch), picking
    // the FIRST one whose addr <= target, (target - addr) is a multiple of
    // pitch, and row_offset + source_h <= cand.h. The walk is closest-first
    // so we prefer smaller deltas (= exact / near-exact base entries). When
    // the runtime y_adj path lands on a small-zoom sibling's addr that has
    // h < source_h, walkback finds the big-zoom base whose row range covers
    // the full requested h. Bounded to keep worst-case dense clusters cheap.
    static const uint32_t WALKBACK_MAX = 16;
    uint32_t i = lo;
    uint32_t steps = 0;
    while (i > 0 && steps < WALKBACK_MAX) {
        --i; ++steps;
        const HwspritesBakedEntry& cand = hwsprites_baked_index[i];
        if (cand.bank != bank || cand.flip != flip_v || cand.pitch != pitch)
            return miss;
        if (cand.addr > addr) continue;
        const uint16_t delta = (uint16_t)(addr - cand.addr);
        if (delta == 0) {
            if (cand.h >= source_h) return {&cand, 0};
            continue;
        }
        if (pitch <= 0) continue;
        const uint16_t upitch = (uint16_t)pitch;
        if (delta % upitch) continue;
        const uint16_t off = delta / upitch;
        if ((uint32_t)off + (uint32_t)source_h > (uint32_t)cand.h) continue;
        return {&cand, off};
    }
    return miss;
}

void hwsprites::atlas_reset()
{
    std::memset(atlas_entries, 0, sizeof(atlas_entries));
    atlas_used = 0;
}

uint32_t hwsprites::atlas_extract_count()  const { return atlas_extracts; }
uint32_t hwsprites::baked_extract_count()  const { return baked_extracts; }
uint32_t hwsprites::atlas_hit_count()      const { return atlas_hits; }
uint32_t hwsprites::atlas_overflow_count() const { return atlas_overflows; }
uint32_t hwsprites::atlas_used_bytes()     const { return atlas_used; }

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
    uint16_t vzoom)
{
    if (!atlas_pool || height == 0 || vzoom == 0) return nullptr;

    // Match CPU render(): row[i] reads source row floor(i * vzoom / 512).
    // After height iterations the last row touched is (height-1)*vzoom/512,
    // so total distinct source rows = that + 1. The on-screen height stays
    // ~= height because rdpq scale_y = 512/vzoom undoes the row stretch.
    const uint32_t source_h32 = ((uint32_t)(height - 1) * (uint32_t)vzoom) / 512u + 1u;
    const uint16_t source_h = (source_h32 > 0xffffu) ? 0xffffu : (uint16_t)source_h32;

    const uint64_t key  = hwsprites_atlas_key(bank, addr, source_h, pitch, flip);
    const uint32_t mask = ATLAS_CAPACITY - 1;
    uint32_t idx = hwsprites_mix64(key) & mask;

    while (atlas_entries[idx].key != 0)
    {
        if (atlas_entries[idx].key == key)
        {
            atlas_hits++;
            return &atlas_entries[idx];
        }
        idx = (idx + 1) & mask;
    }

    // ---- Baked-atlas fast path ------------------------------------------
    // The bake holds one CI4 blob per (bank, addr, pitch, flip) tuple sized
    // to the largest source_h ever seen for that sprite. Shorter source_h
    // variants are just row-prefixes of the same blob, so we can PI-DMA
    // only the first source_h rows (ci4_stride bytes each) from the cart
    // into the bump pool. That replaces ~500 us of CPU EOR-walk + bitplane
    // decode with a ~50-100 us PI transfer.
    if (baked_blob_pi_addr)
    {
        const HwspritesBakedHit hit =
            hwsprites_baked_lookup(bank, addr, pitch, flip, source_h);
        const HwspritesBakedEntry* be = hit.be;
        const uint16_t row_off = hit.row_offset;
        const uint16_t avail_h = be ? (uint16_t)(be->h - row_off) : 0;
        if (be && source_h <= avail_h)
        {
            const uint32_t ci4_stride = (uint32_t)be->w / 2;
            // bytes to DMA: source_h rows of CI4, padded to 8 (matches the
            // alignment we'd use for a freshly-extracted entry).
            const uint32_t bytes =
                (ci4_stride * (uint32_t)source_h + 7u) & ~7u;
            // Row-offset variant: skip the first row_off rows of the baked
            // blob. ci4_stride is a multiple of 4 (w is a multiple of 8), so
            // the PI source stays 2-byte aligned — fine for dma_read.
            const uint32_t skip_bytes = (uint32_t)row_off * ci4_stride;

            if (atlas_used + bytes > atlas_pool_bytes)
            {
                atlas_overflows++;
                if (bytes > atlas_pool_bytes) return nullptr;
                atlas_reset();
                idx = hwsprites_mix64(key) & mask;
            }

            uint8_t* dst = atlas_pool + atlas_used;
            atlas_used += bytes;

            // PI-DMA the source-h prefix. dma_read takes a PI address inside
            // cart space; the bake aligned each entry to 8 bytes, so the
            // base + offset is 8-byte aligned and the length is 2-aligned.
            dma_read(dst,
                     baked_blob_pi_addr + be->blob_offset + skip_bytes,
                     bytes);

            AtlasEntry& e = atlas_entries[idx];
            e.key        = key;
            e.ci4        = dst;
            e.w          = be->w;
            e.h          = source_h;             // requested variant height
            // Shadow bbox is reported in baked coords (rows 0..be->h-1). For a
            // row-offset variant, shift y by -row_off and clip to source_h so
            // the body pass doesn't sample rows we never DMA'd in.
            e.has_shadow = be->has_shadow;
            e.shadow_x0  = be->shadow_x0;
            e.shadow_x1  = be->shadow_x1;
            if (e.has_shadow)
            {
                const uint16_t y0 = be->shadow_y0;
                const uint16_t y1 = be->shadow_y1;
                if (y1 <= row_off) {
                    e.has_shadow = 0;
                } else {
                    e.shadow_y0 = (y0 > row_off) ? (uint16_t)(y0 - row_off) : 0;
                    const uint16_t y1_shift = (uint16_t)(y1 - row_off);
                    e.shadow_y1 = (y1_shift > source_h) ? source_h : y1_shift;
                }
            }
            if (e.has_shadow && e.shadow_y1 <= e.shadow_y0)
                e.has_shadow = 0;
            if (!e.has_shadow)
            {
                e.shadow_x0 = e.shadow_y0 = 0;
                e.shadow_x1 = e.shadow_y1 = 0;
            }

            baked_extracts++;
            atlas_extracts++;
            return &e;
        }
    }

    // ---- Spillover: CPU EOR walk + bitplane decode (the original path) --
    const uint32_t* spritedata = sprites + 0x10000u * (uint32_t)(bank & 7);

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

    if (atlas_used + bytes > atlas_pool_bytes)
    {
        atlas_overflows++;
        if (bytes > atlas_pool_bytes) return nullptr;
        atlas_reset();
        idx = hwsprites_mix64(key) & mask;  // table is empty; first slot is free
    }

    uint8_t* dst = atlas_pool + atlas_used;
    atlas_used += bytes;
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

    AtlasEntry& e = atlas_entries[idx];
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
void hwsprites::render_rdp(uint8_t priority, const uint16_t* sprite_tlut,
                           int x_offset, int y_offset)
{
    // Lazy-init the atlas pool. Deferred from hwsprites::init() because the
    // heap is too fragmented at boot (ROM sources + 1 MiB sprites[] both live
    // simultaneously). By the first render call, Video::init() has freed the
    // ROM source buffers and we have a contiguous block to grab.
    if (!atlas_pool) atlas_init();
    if (!atlas_pool) return;

    // Per-call telemetry — mirrors hwtiles::render_rdp_tile_layers. Locals
    // are incremented at the relevant sites and flushed to n64_profile at
    // function exit so the OUT/PLS logger can correlate spr= cost against
    // sprite count, LOAD_BLOCK count, and TLUT cache miss count.
    const uint32_t spr_pre_prim_count = n64_profile::prim_count;
    const uint64_t spr_t0_us = get_ticks_us();
    uint32_t spr_vis = 0;
    uint32_t spr_loads = 0;
    uint32_t spr_tlut_uploads = 0;

// Set to 1 to enable per-sprite counters (atlas loads, shadow tight-bbox %,
// palette-cache hit rate). Adds ~10 counter ops per sprite × ~80-150 sprites/
// frame, so leave off in steady-state measurement.
#define HWSPR_PROFILE 0
#if HWSPR_PROFILE
    uint64_t prof_t0 = get_ticks_us();
    uint32_t prof_total = 0;
    uint32_t prof_opaque = 0;
    uint32_t prof_shadow = 0;
    uint32_t prof_tlut_skipped = 0;   // last_tlut hit (upload elided)
    uint32_t prof_tlut_uploaded = 0;  // last_tlut miss (upload emitted)
    uint32_t prof_pix_total = 0;      // sum of e->w * e->h (pre-zoom)
    uint32_t prof_pix_zoomed = 0;     // sum of zoomed_w * zoomed_h (RDP fill)
    uint32_t prof_size_le2k = 0;      // sprites whose CI4 fits one TMEM strip
    uint32_t prof_atlas_loads = 0;    // # sprites that triggered LOAD_BLOCK
    uint32_t prof_palette_only = 0;   // # sprites that only rebound palette
    uint32_t prof_no_setup = 0;       // # sprites that reused atlas+palette
    uint32_t prof_shadow_demoted = 0; // # shadow-flagged sprites with no slot-0xa pixels
    uint32_t prof_pix_shadow_full = 0;  // shadow zoom_pix if rect was full sprite
    uint32_t prof_pix_shadow_tight = 0; // shadow zoom_pix using tight bbox
    uint32_t prof_pix_zoomed_max = 0; // largest single sprite (px)
    uint32_t prof_bucket_huge = 0;    // sprites with zoom_pix > 4000
    uint32_t prof_bucket_mid  = 0;    // sprites with 1000 < zoom_pix <= 4000
    uint32_t prof_bucket_small= 0;    // sprites with zoom_pix <= 1000
#endif

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

        if (config.video.hires)
        {
            xpos  <<= 1;
            top   <<= 1;
            hzoom >>= 1;
            vzoom >>= 1;
        }

        const AtlasEntry* e = atlas_get_or_extract(
            (uint16_t)bank, (uint16_t)addr,
            (uint16_t)height, (int16_t)pitch, flip != 0,
            (uint16_t)vzoom);
        if (!e) continue;

        // Shadow pass is a 2-cycle RDP operation (darken blender). If this
        // sprite carries no shadow silhouette in its CI4 data, the pass
        // produces zero visible writes — skip it.
        if (shadow && !e->has_shadow)
        {
            shadow = false;
#if HWSPR_PROFILE
            prof_shadow_demoted++;
#endif
        }
#if HWSPR_PROFILE
        prof_total++;
        prof_pix_total += (uint32_t)e->w * (uint32_t)e->h;
        // CI4 bytes = (w/2) * h. One-strip threshold: TMEM has 4 KB total
        // but with TLUT taking 2 KB we have 2 KB for pixels => 2048 bytes
        // for CI4 fits as one strip.
        if (((uint32_t)e->w * (uint32_t)e->h) <= 4096) prof_size_le2k++;
#endif

        const float scale_x  = 512.0f / (float)hzoom;
        const float scale_y  = 512.0f / (float)vzoom;
        const float zoomed_w = (float)e->w * scale_x;
        const float zoomed_h = (float)e->h * scale_y;

        // Skip the shadow pass on distant scenery: when the zoomed sprite is
        // small the shadow contributes only a few darkened pixels but still
        // costs a full mask+body draw (2 RDP primitives + TLUT setup).
        // Starting-line / stage-2 rock scenes draw ~80 shadow sprites per
        // frame, most of them tiny — this cuts the shadow draw count
        // substantially with no visible difference at this zoom level.
        if (shadow && (zoomed_w * zoomed_h) < 256.0f)
        {
            shadow = false;
#if HWSPR_PROFILE
            prof_shadow_demoted++;
#endif
        }
#if HWSPR_PROFILE
        {
            uint32_t zpx = (uint32_t)(zoomed_w * zoomed_h);
            prof_pix_zoomed += zpx;
            if (zpx > prof_pix_zoomed_max) prof_pix_zoomed_max = zpx;
            if (zpx > 4000)      prof_bucket_huge++;
            else if (zpx > 1000) prof_bucket_mid++;
            else                 prof_bucket_small++;
            if (shadow) prof_shadow++; else prof_opaque++;
        }
#endif

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

        if (screen_x + zoomed_w <= 0.0f) continue;
        if (screen_x >= (float)config.s16_width) continue;
        if (screen_y + zoomed_h <= 0.0f) continue;
        if (screen_y >= (float)config.s16_height) continue;

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
#if HWSPR_PROFILE
                {
                    bool ach = atlas_changed;
                    int  cp  = cur_tile_palette;
                    if      (ach) prof_atlas_loads++;
                    else if (TLUT_SLOT_SHADOW_MASK != cp) prof_palette_only++;
                    else          prof_no_setup++;
                }
#endif
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
#if HWSPR_PROFILE
                    prof_pix_shadow_full  += (uint32_t)(zoomed_w * zoomed_h);
                    {
                        float tw = (sdx1 > sdx0) ? (sdx1 - sdx0) : (sdx0 - sdx1);
                        float th = (sdy1 > sdy0) ? (sdy1 - sdy0) : (sdy0 - sdy1);
                        prof_pix_shadow_tight += (uint32_t)(tw * th);
                    }
#endif
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
#if HWSPR_PROFILE
                {
                    bool ach = atlas_changed;
                    int  cp  = cur_tile_palette;
                    if      (ach) prof_atlas_loads++;
                    else if (TLUT_SLOT_SHADOW_BODY != cp) prof_palette_only++;
                    else          prof_no_setup++;
                }
#endif
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
#if HWSPR_PROFILE
                {
                    bool ach = atlas_changed;
                    int  cp  = cur_tile_palette;
                    int  ns  = TLUT_SLOT_OPAQUE_BASE + slot;
                    if      (ach)        prof_atlas_loads++;
                    else if (ns != cp)   prof_palette_only++;
                    else                 prof_no_setup++;
                }
#endif
                bind_tile0(TLUT_SLOT_OPAQUE_BASE + slot);
                rdpq_texture_rectangle_scaled(TILE0, x0, y0, x1, y1,
                                              0, 0, e->w, e->h);
                n64_profile::prim_count++;
            }
            continue;
        }

        // ---- Fallback: rdpq_tex_blit (sprite too big to fit one strip) --
        // ~5% of sprites per profile. Strip walker re-loads pixels per
        // strip; shadow sprites pay it twice. tex_blit also writes TILE0/
        // TILE1 internally, so invalidate the bypass cache afterwards.
        surface_t spr_surf = surface_make_linear(e->ci4, FMT_CI4, e->w, e->h);
        rdpq_blitparms_t parms = {};
        parms.scale_x = scale_x;
        parms.scale_y = scale_y;
        parms.flip_x  = mirror_x;
        parms.flip_y  = mirror_y;

        if (shadow)
        {
            if (pipeline != 1)
            {
                rdpq_set_fog_color(RGBA32(0, 0, 0, 128));
                rdpq_mode_combiner(
                    RDPQ_COMBINER1((0, 0, 0, 0), (0, 0, 0, TEX0)));
                rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY_CONST);
                pipeline = 1;
            }
            rdpq_tex_upload_tlut((uint16_t*)shadow_mask_tlut, 0, 16);
            rdpq_tex_blit(&spr_surf, dst_x, dst_y, &parms);
            n64_profile::prim_count++;
            spr_tlut_uploads++;
            spr_loads++;

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
            rdpq_tex_upload_tlut(scratch, 0, 16);
            rdpq_tex_blit(&spr_surf, dst_x, dst_y, &parms);
            n64_profile::prim_count++;
            spr_tlut_uploads++;
            spr_loads++;
        }
        else
        {
            if (pipeline != 0)
            {
                rdpq_mode_combiner(RDPQ_COMBINER_TEX);
                rdpq_mode_blender(0);
                pipeline = 0;
            }
            rdpq_tex_upload_tlut((uint16_t*)color_tlut, 0, 16);
            rdpq_tex_blit(&spr_surf, dst_x, dst_y, &parms);
            n64_profile::prim_count++;
            spr_tlut_uploads++;
            spr_loads++;
        }
        // tex_blit clobbers TILE0/TILE1 and uploads its TLUT to palette
        // slot 0, so invalidate the bypass cache for slots 0/TILE0. Slots
        // 2..15 (opaque LRU) are untouched, so we leave those tags intact.
        last_atlas_ci4    = NULL;
        cur_tile_palette  = -1;
        shadow_mask_loaded = false;
    }

#if HWSPR_PROFILE
    static uint32_t prof_frame = 0;
    uint64_t prof_t1 = get_ticks_us();
    float prof_fps = display_get_fps();
    if (prof_fps > 10.0f && prof_fps < 30.0f && ((prof_frame++ & 7) == 0))
    {
        static uint32_t prev_extracts = 0;
        static uint32_t prev_hits = 0;
        uint32_t dx = atlas_extracts - prev_extracts;
        uint32_t dh = atlas_hits - prev_hits;
        prev_extracts = atlas_extracts;
        prev_hits     = atlas_hits;
        debugf("spr[%5lu] n=%3lu(o=%2lu,s=%2lu,demo=%2lu) zoom=%6lu "
               "shFull=%6lu shTight=%6lu (%2lu%%) load=%2lu palOnly=%2lu noSet=%2lu us=%5lu\n",
               (unsigned long)prof_frame,
               (unsigned long)prof_total,
               (unsigned long)prof_opaque,
               (unsigned long)prof_shadow,
               (unsigned long)prof_shadow_demoted,
               (unsigned long)prof_pix_zoomed,
               (unsigned long)prof_pix_shadow_full,
               (unsigned long)prof_pix_shadow_tight,
               (unsigned long)(prof_pix_shadow_full
                   ? (prof_pix_shadow_tight * 100UL / prof_pix_shadow_full) : 0),
               (unsigned long)prof_atlas_loads,
               (unsigned long)prof_palette_only,
               (unsigned long)prof_no_setup,
               (unsigned long)(prof_t1 - prof_t0));
        (void)dh; (void)dx;
    }
#endif

    n64_profile::spr_call_vis          = spr_vis;
    n64_profile::spr_call_loads        = spr_loads;
    n64_profile::spr_call_tlut_uploads = spr_tlut_uploads;
    n64_profile::spr_call_prims        = n64_profile::prim_count - spr_pre_prim_count;
    n64_profile::spr_call_us           = (uint32_t)(get_ticks_us() - spr_t0_us);
}