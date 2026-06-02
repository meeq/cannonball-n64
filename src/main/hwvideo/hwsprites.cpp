#include "video.hpp"
#include "hwvideo/hwsprites.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"

#include <libdragon.h>
#include <cstring>
#include <malloc.h>

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
// allocate from a single ATLAS_POOL_BYTES bump pool. On pool overflow the
// cache resets wholesale (re-extract on next miss) so no LRU walk is needed.
// ============================================================================

void hwsprites::atlas_init()
{
    if (atlas_pool) return;
    atlas_pool = (uint8_t*)memalign(8, ATLAS_POOL_BYTES);
    atlas_reset();
    debugf("atlas_init: pool=%p bytes=%u\n",
           atlas_pool, (unsigned)ATLAS_POOL_BYTES);
}

void hwsprites::atlas_reset()
{
    std::memset(atlas_entries, 0, sizeof(atlas_entries));
    atlas_used = 0;
}

uint32_t hwsprites::atlas_extract_count()  const { return atlas_extracts; }
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

    // Miss — extract into the bump pool.
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

    const uint16_t w = (uint16_t)(max_words * 8);
    const uint16_t h = source_h;
    const uint32_t ci4_stride = (uint32_t)w / 2;
    const uint32_t bytes = (ci4_stride * (uint32_t)h + 7u) & ~7u;

    if (atlas_used + bytes > ATLAS_POOL_BYTES)
    {
        atlas_overflows++;
        if (bytes > ATLAS_POOL_BYTES) return nullptr;
        atlas_reset();
        idx = hwsprites_mix64(key) & mask;  // table is empty; first slot is free
    }

    uint8_t* dst = atlas_pool + atlas_used;
    atlas_used += bytes;
    std::memset(dst, 0, bytes);

    // Pass 2: decode pixels into CI4. Leftmost pixel of a byte sits in the
    // high nibble — matches the layout the RDP expects on N64. Pixels are
    // emitted in canonical left-to-right screen order regardless of flip, so
    // mirror_x at blit time depends only on xdelta.
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
    e.key = key;
    e.ci4 = dst;
    e.w   = w;
    e.h   = h;
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

    // TLUT-upload cache. Each rdpq_tex_upload_tlut emits ~3 RDP commands
    // (set_texture_image_raw + set_tile + load_tile) and writes to TILE7
    // (RDPQ_TILE_INTERNAL). Many consecutive sprites in OutRun share a
    // palette slot (crowd extras, banner pixels, smoke); skipping a repeat
    // upload is the same win that PF4 got on the tile layer. We key off the
    // source pointer because each unique palette (color_tlut[color*16],
    // shadow_mask_tlut, or a fresh shadow-body scratch slot) has a distinct
    // address. Scratch slots come from a ring, so identity-comparison is
    // safe — pass-2 shadow uploads never alias prior slots within a frame.
    const uint16_t* last_tlut = NULL;

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
        // split. Shadow sprites switch into a 50%-darken pipeline below.
        const bool shadow = ((ramBuff[data+3] >> 14) & 1) != 0;

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

        if (screen_x + zoomed_w <= 0.0f) continue;
        if (screen_x >= (float)config.s16_width) continue;
        if (screen_y + zoomed_h <= 0.0f) continue;
        if (screen_y >= (float)config.s16_height) continue;

        surface_t spr_surf = surface_make_linear(e->ci4, FMT_CI4, e->w, e->h);
        rdpq_blitparms_t parms = {};
        parms.scale_x = scale_x;
        parms.scale_y = scale_y;
        parms.flip_x  = mirror_x;
        parms.flip_y  = mirror_y;
        const float dst_x = screen_x + (float)x_offset;
        const float dst_y = screen_y + (float)y_offset;

        // Sprite TLUT layout: 128 slots x 16 entries (engine sprite palette
        // is 0x800 + color*16 + pix).
        const uint16_t* color_tlut = &sprite_tlut[(uint32_t)color * 16];

        if (shadow)
        {
            // Pass 1 — shadow darken. Switch into the constant-50%-multiply
            // pipeline, upload the mask TLUT (only slot 0xa carries alpha),
            // blit. Darkens whatever is underneath at the slot-0xa pixels.
            if (pipeline != 1)
            {
                rdpq_set_fog_color(RGBA32(0, 0, 0, 128));
                rdpq_mode_combiner(
                    RDPQ_COMBINER1((0, 0, 0, 0), (0, 0, 0, TEX0)));
                rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY_CONST);
                pipeline = 1;
            }
            if (last_tlut != shadow_mask_tlut)
            {
                rdpq_tex_upload_tlut((uint16_t*)shadow_mask_tlut, 0, 16);
                last_tlut = shadow_mask_tlut;
            }
            rdpq_tex_blit(&spr_surf, dst_x, dst_y, &parms);

            // Pass 2 — opaque body. Switch back, allocate a scratch TLUT in
            // the ring with slot 0xa zeroed (the shadow texel renders as a
            // transparent pixel here, so we never overdraw the darkened
            // background with palette colour 10). Then blit normally.
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
            // Write through an uncached view so the 16 halfword stores go
            // straight to RDRAM via the store buffer and the RDP TLUT-load
            // DMA below sees fresh data without a data_cache writeback.
            uint16_t* scratch_uc = (uint16_t*)UncachedAddr(scratch);
            for (int i = 0; i < 16; i++) scratch_uc[i] = color_tlut[i];
            scratch_uc[10] = 0;
            rdpq_tex_upload_tlut(scratch, 0, 16);
            last_tlut = scratch;
            rdpq_tex_blit(&spr_surf, dst_x, dst_y, &parms);
        }
        else
        {
            if (pipeline != 0)
            {
                rdpq_mode_combiner(RDPQ_COMBINER_TEX);
                rdpq_mode_blender(0);
                pipeline = 0;
            }
            if (last_tlut != color_tlut)
            {
                rdpq_tex_upload_tlut((uint16_t*)color_tlut, 0, 16);
                last_tlut = color_tlut;
            }
            rdpq_tex_blit(&spr_surf, dst_x, dst_y, &parms);
        }
    }
}