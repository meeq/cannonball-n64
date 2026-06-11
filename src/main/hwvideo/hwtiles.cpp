#include <cstring> // memcpy
#include <malloc.h> // memalign
#include <libdragon.h>
#include "globals.hpp"
#include "romloader.hpp"
#include "hwvideo/hwtiles.hpp"
#include "frontend/config.hpp"
#include <cstring>

namespace n64_profile {
    extern uint32_t prim_count;
    extern uint32_t tile_tlut_uploads;
    extern uint32_t text_tlut_uploads;
    extern uint32_t tile_call_vis;
    extern uint32_t tile_call_uniq_total;
    extern uint32_t tile_call_chunks;
    extern uint32_t tile_call_tlut_evicts;
    extern uint32_t tile_call_prims;
    extern uint32_t tile_call_pass1_us;
    extern uint32_t tile_call_pass2_us;
    extern uint32_t tile_call_dma_fetches;
    extern uint32_t tile_call_dma_us;
    extern uint32_t tile_call_dma_misses;
}

/***************************************************************************
    Video Emulation: OutRun Tilemap Hardware.
    Based on MAME source code.

    Copyright Aaron Giles.
    All rights reserved.
***************************************************************************/

/*******************************************************************************************
 *
 *  System 16B-style tilemaps
 *
 *  16 total pages
 *  Column/rowscroll enabled via bits in text layer
 *  Alternate tilemap support
 *
 *  Tile format:
 *      Bits               Usage
 *      p------- --------  Tile priority versus sprites
 *      -??----- --------  Unknown
 *      ---ccccc cc------  Tile color palette
 *      ---nnnnn nnnnnnnn  Tile index
 *
 *  Text format:
 *      Bits               Usage
 *      p------- --------  Tile priority versus sprites
 *      -???---- --------  Unknown
 *      ----ccc- --------  Tile color palette
 *      -------n nnnnnnnn  Tile index
 *
 *  Alternate tile format:
 *      Bits               Usage
 *      p------- --------  Tile priority versus sprites
 *      -??----- --------  Unknown
 *      ----cccc ccc-----  Tile color palette
 *      ---nnnnn nnnnnnnn  Tile index
 *
 *  Alternate text format:
 *      Bits               Usage
 *      p------- --------  Tile priority versus sprites
 *      -???---- --------  Unknown
 *      -----ccc --------  Tile color palette
 *      -------- nnnnnnnn  Tile index
 *
 *  Text RAM:
 *      Offset   Bits               Usage
 *      E80      aaaabbbb ccccdddd  Foreground tilemap page select
 *      E82      aaaabbbb ccccdddd  Background tilemap page select
 *      E84      aaaabbbb ccccdddd  Alternate foreground tilemap page select
 *      E86      aaaabbbb ccccdddd  Alternate background tilemap page select
 *      E90      c------- --------  Foreground tilemap column scroll enable
 *               -------v vvvvvvvv  Foreground tilemap vertical scroll
 *      E92      c------- --------  Background tilemap column scroll enable
 *               -------v vvvvvvvv  Background tilemap vertical scroll
 *      E94      -------v vvvvvvvv  Alternate foreground tilemap vertical scroll
 *      E96      -------v vvvvvvvv  Alternate background tilemap vertical scroll
 *      E98      r------- --------  Foreground tilemap row scroll enable
 *               ------hh hhhhhhhh  Foreground tilemap horizontal scroll
 *      E9A      r------- --------  Background tilemap row scroll enable
 *               ------hh hhhhhhhh  Background tilemap horizontal scroll
 *      E9C      ------hh hhhhhhhh  Alternate foreground tilemap horizontal scroll
 *      E9E      ------hh hhhhhhhh  Alternate background tilemap horizontal scroll
 *      F16-F3F  -------- vvvvvvvv  Foreground tilemap per-16-pixel-column vertical scroll
 *      F56-F7F  -------- vvvvvvvv  Background tilemap per-16-pixel-column vertical scroll
 *      F80-FB7  a------- --------  Foreground tilemap per-8-pixel-row alternate tilemap enable
 *               -------h hhhhhhhh  Foreground tilemap per-8-pixel-row horizontal scroll
 *      FC0-FF7  a------- --------  Background tilemap per-8-pixel-row alternate tilemap enable
 *               -------h hhhhhhhh  Background tilemap per-8-pixel-row horizontal scroll
 *
 *******************************************************************************************/

// RAM-resident tile pixel cache. tile graphics live cart-side
// ([[project-hwtiles-cart-side]]); a fresh PI-DMA per unique tile costs
// ~22 us of setup latency, so ~150 fetches per frame in OUT ≈ 3-4 ms of
// pass 1. Direct-mapped cache lets repeat tile codes skip the bus.
//
// One tile = 8 rows × 4 bytes = 32 bytes. Direct map by (Code & MASK):
// 256 slots × 32 B = 8 KiB pixels + 0.5 KiB tags. 4 MiB heap budget
// is the binding constraint: with the atlas pool (256 KiB), display
// FBs (307 KiB), ROM data (~1 MiB), and the audio mixer's per-channel
// buffers (4 KiB × 16 ch = 64 KiB priming alloc, see
// Audio::prime_mixer_buffers), post-roms free heap is ~80 KiB and
// 1024 slots × 32B = 32 KiB starves the audio prime.
//
// Hit rate scales with slots / working_set. At 256 slots vs ~150
// unique codes per frame, conflict misses are noticeable (~50%
// collision rate) but still cut DMA cost roughly in half vs no cache.
// 8 MiB consoles would have room for 1024 but the size is currently
// pinned to keep heap math identical across both targets.
//
// Heap-allocated in hwtiles::init() — has to run after roms.load on
// 4 MiB (ROM load needs ~1 MiB contiguous; pre-roms BSS growth bumps
// up against that ceiling). Slots double as DMA targets — invalidate
// before issuing the PI-DMA so the CPU sees fresh bytes through the
// cached alias. On a hit the inner pack loop reads through the same
// cached alias; lines stay hot in L1 across the chunk's inner loop.
namespace {
    constexpr uint32_t TILE_CACHE_SLOTS = 256;
    constexpr uint32_t TILE_CACHE_MASK  = TILE_CACHE_SLOTS - 1;
    constexpr uint32_t TILE_BYTES       = 32;

    uint8_t*  s_tile_cache_pix = nullptr;  // [SLOTS * TILE_BYTES]
    uint16_t* s_tile_cache_tag = nullptr;  // [SLOTS]

    inline const uint32_t* hwtiles_fetch_tile(uint32_t tiles_pi_addr,
                                              uint32_t code)
    {
        const uint64_t t0 = get_ticks_us();
        const uint32_t slot = code & TILE_CACHE_MASK;
        uint8_t* line = s_tile_cache_pix + (size_t)slot * TILE_BYTES;
        n64_profile::tile_call_dma_fetches++;
        if (s_tile_cache_tag[slot] != (uint16_t)code) {
            data_cache_hit_writeback_invalidate(line, TILE_BYTES);
            dma_read(line, tiles_pi_addr + code * TILE_BYTES, TILE_BYTES);
            s_tile_cache_tag[slot] = (uint16_t)code;
            n64_profile::tile_call_dma_misses++;
        }
        n64_profile::tile_call_dma_us += (uint32_t)(get_ticks_us() - t0);
        return (const uint32_t*)line;
    }
}

hwtiles::hwtiles(void)
    : tiles_pi_addr(0)
{
    for (int i = 0; i < 2; i++)
        tile_banks[i] = i;

    set_x_clamp(CENTRE);
}

hwtiles::~hwtiles(void)
{

}

void hwtiles::init(uint8_t* /*src_tiles*/, const bool hires)
{
    // CI4 tile data is pre-decoded offline (see tools/bake-sprites) and
    // shipped as /tiles/tiles_native.bin inside the DragonFS payload. We
    // resolve its cart PI address once here so the render hot path can
    // PI-DMA per-tile slices on demand instead of holding a 256 KiB
    // resident array. src_tiles is unused — the host bake tool reads the
    // same ROM set and does the bitplane→CI4 conversion at build time.
    tiles_pi_addr = dfs_rom_addr("/tiles/tiles_native.bin") & 0x1fffffff;
    assertf(tiles_pi_addr,
            "hwtiles: /tiles/tiles_native.bin missing from DFS — "
            "rebake required");

    // Tile pixel cache lives on the heap (not BSS) so its 34 KiB doesn't
    // squeeze the contiguous pre-roms heap that romloader needs for the
    // ~1 MiB ROM allocation. hwtiles::init runs from video.init, after
    // roms.load_revb_roms in main(), so the heap is past that pressure
    // point here. memalign(8, ...) so dma_read targets meet PI alignment.
    if (!s_tile_cache_pix) {
        s_tile_cache_pix = (uint8_t*)memalign(8, TILE_CACHE_SLOTS * TILE_BYTES);
        s_tile_cache_tag = (uint16_t*)memalign(2, TILE_CACHE_SLOTS * sizeof(uint16_t));
        assertf(s_tile_cache_pix && s_tile_cache_tag,
                "hwtiles: tile cache alloc failed (%u bytes)",
                (unsigned)(TILE_CACHE_SLOTS * (TILE_BYTES + sizeof(uint16_t))));
        std::memset(s_tile_cache_tag, 0xff, TILE_CACHE_SLOTS * sizeof(uint16_t));
    }

    // The legacy SDL build dispatched CPU rendering through
    // render8x8_tile_mask{,_clip} function pointers (lores/hires variants).
    // N64 uses RDP-based render_rdp_tile_layers / render_rdp_text_layer
    // exclusively (see src/main/n64/rendersurface.cpp), so the CPU paths
    // and the function-pointer setup are dropped. s16_width_noscale is
    // still consulted by the RDP renderers for clipping.
    s16_width_noscale = hires ? (config.s16_width >> 1) : config.s16_width;
}

// patch_tiles / restore_tiles are widescreen-only on N64. With
// video.widescreen hardcoded to 0 (frontend/config.cpp), OMusic's call
// site gates on config.s16_x_off > 0 and never enters. tiles_backup is
// dropped to save 256 KiB BSS — both methods are stubs so the OMusic
// gate stays the single source of truth.
void hwtiles::patch_tiles(RomLoader*) {}
void hwtiles::restore_tiles() {}

// Set Tilemap X Clamp
//
// This is used for the widescreen mode, in order to clamp the tilemap to
// a location of the screen. 
//
// In-Game we must clamp right to avoid page scrolling issues.
//
// The clamp will always be 192 for the non-widescreen mode.
void hwtiles::set_x_clamp(const uint16_t props)
{
    if (props == LEFT)
    {
        x_clamp = 192;
    }
    else if (props == RIGHT)
    {
        x_clamp = (512 - s16_width_noscale);
    }
    else if (props == CENTRE)
    {
        x_clamp = 192 - config.s16_x_off;
    }
}

void hwtiles::update_tile_values()
{
    for (int i = 0; i < 4; i++)
    {
        page[i] = ((text_ram[0xe80 + (i * 2) + 0] << 8) | text_ram[0xe80 + (i * 2) + 1]);

        scroll_x[i] = ((text_ram[0xe98 + (i * 2) + 0] << 8) | text_ram[0xe98 + (i * 2) + 1]);
        scroll_y[i] = ((text_ram[0xe90 + (i * 2) + 0] << 8) | text_ram[0xe90 + (i * 2) + 1]);
    }
}

// RDP path: emit one textured-rectangle per visible tile straight into the
// attached framebuffer at (x_offset, y_offset). Walks BG (page=1) then FG
// (page=0) in a single pass so atlas chunks can pack uniques from both pages,
// saving the second walk's setup + draw-call overhead and the page-boundary
// chunk close.
//
// Two-pass atlas variant: walk both tilemap pages once to collect the visible
// tiles and their unique codes; copy each unique code's 32-byte CI4 row block
// into a scratch atlas; upload the whole atlas to TMEM with one LOAD; then
// draw all visible tiles via texture_rectangle indexing into that atlas.
// Collapses ~50–80 per-tile LOAD_TILE pairs into a single LOAD per chunk.
//
// Layering: BG visibles are appended before FG visibles, so within a chunk
// that spans both pages the draw order is BG-first / FG-second — FG correctly
// occludes BG. Across chunks, chunk N draws strictly before chunk N+1.
//
// TMEM budget: bank 0 is 2 KB. We pack two CI4 8x8 tiles per 8-byte TMEM
// line (left/right halves of a 16-px-wide line), 32 pairs × 8 rows × 8 B
// = 2 KB exactly = 64 tiles. Upload uses LOAD_BLOCK through libdragon's
// 4bpp-as-RGBA16 trick (rdpq_tex.c texload_block_4bpp), which round-trips
// the wide line cleanly — see the TMEM swap probe in rendersurface.cpp.
void hwtiles::render_rdp_tile_layers(const uint16_t* tile_tlut,
                                     uint8_t priority_draw,
                                     int x_offset, int y_offset)
{
    // Per-tile pack: each tile owns its own 8 TMEM lines so any slot starts
    // at s=0 and a mask=3 wrap can horizontally repeat it. RDP requires
    // tmem_pitch >= 8, so each line still costs 8 bytes (16 CI4 texels) even
    // though only the first 4 bytes hold tile data — the right half is
    // padding and never sampled (set_tile_size caps s at 8). That halves
    // tiles-per-chunk vs. the old paired layout but every adjacent same-slot
    // visible (not just in_pair=0) can now be coalesced.
    constexpr int ATLAS_MAX        = 32;
    constexpr int ATLAS_PITCH      = 8;     // bytes per TMEM line (RDP min)
    constexpr int ATLAS_TILE_H     = 8;     // rows per tile
    constexpr int ATLAS_TILE_BYTES = ATLAS_TILE_H * ATLAS_PITCH; // 64
    constexpr int ATLAS_BYTES      = ATLAS_MAX * ATLAS_TILE_BYTES; // 2048
    // Atlas ring: each chunk packs into one of K_RING buffers and rotates.
    // BG+FG share the ring across one call. K leaves no in-flight buffer
    // exposed to clobbering. See [[rdp-deferred-dma-static-source]].
    //
    // Music-select uses fg_psel=bg_psel=0xFFFF (all four quadrants map to the
    // music-select tilemap in page F) so the renderer walks page F in both
    // passes — up to 2x more visibles than in-game. Combined with the
    // per-tile pack above halving ATLAS_MAX from 64 → 32, the worst-case
    // unique-code count per call doubled vs the paired layout. 32 chunks
    // restores the headroom 16 had at 64 tiles/chunk. The overflow path
    // below now counts + reports drops so a future ATLAS_MAX change won't
    // silently re-bite (see [[feedback-silent-overflow-break]]).
    constexpr int K_ATLAS_RING = 32;
    constexpr int MAX_CHUNKS_PER_CALL = 32;

    struct Visible { int16_t x, y; uint16_t slot; uint8_t colour; };

    static Visible  s_visible[8192];
    static uint16_t s_used_codes[ATLAS_MAX];
    static uint16_t s_code_to_slot[NUM_TILES];
    static bool     s_slot_map_initted = false;
    if (!s_slot_map_initted) {
        for (int i = 0; i < NUM_TILES; i++) s_code_to_slot[i] = 0xffff;
        s_slot_map_initted = true;
    }
    alignas(8) static uint8_t s_atlas_ring[K_ATLAS_RING][ATLAS_BYTES];
    static uint8_t s_ring_ix = 0;

    // Per-call chunk metadata: where the chunk's visibles end, how many
    // uniques it has, and which ring buffer holds its atlas.
    int chunk_vis_end[MAX_CHUNKS_PER_CALL];
    int chunk_uniq   [MAX_CHUNKS_PER_CALL];
    int chunk_ring_ix[MAX_CHUNKS_PER_CALL];
    int n_chunks = 0;
    uint8_t* atlas_cur = s_atlas_ring[s_ring_ix];

    // Drop counter for the chunk-cap safety break. Counted per-call (not
    // per-row) and reported via a periodic debugf below. Non-zero in steady
    // state means MAX_CHUNKS_PER_CALL is too low for the current scene and
    // tiles are being silently dropped — see music-select regression
    // history in [[project-music-select-dashboard-missing]].
    static uint32_t s_overflow_drops = 0;
    bool overflow_hit = false;

    // Snapshot the global TLUT upload counter so we can derive *this call's*
    // eviction count as a delta at function exit. Cheaper than threading a
    // local through every upload site. Same trick for prim_count to measure
    // how effective the horizontal run coalesce is (vis vs. emitted rects).
    const uint32_t pre_tlut_uploads = n64_profile::tile_tlut_uploads;
    const uint32_t pre_prim_count   = n64_profile::prim_count;
    n64_profile::tile_call_dma_fetches = 0;
    n64_profile::tile_call_dma_us      = 0;
    n64_profile::tile_call_dma_misses  = 0;
    const uint64_t pass1_t0 = get_ticks_us();

    // ---- Pass 1: collect visible tiles + build atlas chunks ---------------
    int n_visible = 0;
    int n_unique  = 0;

    // Walk BG (page=1) first, then FG (page=0). BG must draw under FG.
    for (int pass = 0; pass < 2; pass++)
    {
        const uint8_t page_index = (pass == 0) ? 1 : 0;

        const uint16_t EffPage = page[page_index];
        uint16_t xScroll = scroll_x[page_index];
        uint16_t yScroll = scroll_y[page_index];

        if ((xScroll & 0x8000) != 0)
            xScroll = (text_ram[0xf80 + (0x40 * page_index) + 0] << 8)
                    | text_ram[0xf80 + (0x40 * page_index) + 1];
        if ((yScroll & 0x8000) != 0)
            yScroll = (text_ram[0xf16 + (0x40 * page_index) + 0] << 8)
                    | text_ram[0xf16 + (0x40 * page_index) + 1];

        const int ox = (x_clamp - xScroll) & 0x3ff; // 0..1023
        const int oy = yScroll & 0x1ff;             // 0..511

        // Counter-walk only the visible window. y = 8*cy - oy and x = 8*cx -
        // ox are monotonic; actual_my = cy & 63 and actual_mx = cx & 127 fold
        // in the original wrap-around (+= 512 / += 1024). This iterates ~29
        // rows × ~41 cols ≈ 1200 cells per pass instead of 8192. Per-row
        // ActPage halves are hoisted so the inner loop is one branch + one
        // ALU op for the tilemap index.
        const int first_cy = oy >> 3;
        const int last_cy  = (oy + S16_HEIGHT - 1) >> 3;
        const int first_cx = ox >> 3;
        const int last_cx  = (ox + s16_width_noscale - 1) >> 3;

        for (int cy = first_cy; cy <= last_cy; cy++)
        {
            const int y  = 8 * cy - oy;
            const int my = cy & 63;
            const uint32_t my_offset = (2 * 64 * my) & 0xfff;
            const bool my_top = my < 32;
            const uint32_t base_L =
                64 * 32 * 2 * ((EffPage >> (my_top ? 0 : 8))  & 0x0f) + my_offset;
            const uint32_t base_R =
                64 * 32 * 2 * ((EffPage >> (my_top ? 4 : 12)) & 0x0f) + my_offset;

            for (int cx = first_cx; cx <= last_cx; cx++)
            {
                const int x  = 8 * cx - ox;
                const int mx = cx & 127;
                const uint32_t base = (mx < 64) ? base_L : base_R;
                const uint32_t TileIndex = base + ((2 * mx) & 0x7f);
                const uint16_t Data =
                    (tile_ram[TileIndex + 0] << 8) | tile_ram[TileIndex + 1];

                if (((Data >> 15) & 1) != priority_draw) continue;

                uint32_t Code = Data & 0x1fff;
                Code = tile_banks[Code / 0x1000] * 0x1000 + Code % 0x1000;
                Code &= (NUM_TILES - 1);
                if (Code == 0) continue;

                const int Colour = (Data >> 6) & 0x7f;

                uint16_t slot = s_code_to_slot[Code];
                if (slot == 0xffff) {
                    // Chunk full → close it, advance ring, reset slot map for
                    // codes that lived in this chunk, allocate this Code into
                    // the new chunk's slot 0. Code itself re-enters as a fresh
                    // unique in the new chunk's atlas.
                    if (n_unique >= ATLAS_MAX) {
                        if (n_chunks >= MAX_CHUNKS_PER_CALL) {
                            if (!overflow_hit) { s_overflow_drops++; overflow_hit = true; }
                            break; // cap exceeded — see s_overflow_drops debugf below
                        }
                        chunk_vis_end[n_chunks] = n_visible;
                        chunk_uniq   [n_chunks] = n_unique;
                        chunk_ring_ix[n_chunks] = s_ring_ix;
                        n_chunks++;
                        for (int k = 0; k < n_unique; k++)
                            s_code_to_slot[s_used_codes[k]] = 0xffff;
                        n_unique = 0;
                        s_ring_ix = (uint8_t)((s_ring_ix + 1) % K_ATLAS_RING);
                        atlas_cur = s_atlas_ring[s_ring_ix];
                    }
                    slot = (uint16_t)n_unique;
                    s_code_to_slot[Code] = slot;
                    s_used_codes[n_unique] = (uint16_t)Code;
                    // Pack tile into left half of its 8 TMEM lines (right
                    // half is padding; never sampled since set_tile_size
                    // caps s at 8). Padding is left uninitialized.
                    uint32_t* dst =
                        (uint32_t*)&atlas_cur[n_unique * ATLAS_TILE_BYTES];
                    const uint32_t* src =
                        hwtiles_fetch_tile(tiles_pi_addr, (uint32_t)Code);
                    for (int r = 0; r < 8; r++)
                        dst[r * 2] = src[r];
                    n_unique++;
                }

                s_visible[n_visible].x      = (int16_t)(x + x_offset);
                s_visible[n_visible].y      = (int16_t)(y + y_offset);
                s_visible[n_visible].slot   = slot;
                s_visible[n_visible].colour = (uint8_t)Colour;
                n_visible++;
            }
            if (overflow_hit) break;
        }
        if (overflow_hit) break;
    }

    // Periodic report when the cap is being exceeded. Cadence (every 32nd
    // call seen with non-zero drops) keeps the debugf out of the per-frame
    // hot path; non-zero output means MAX_CHUNKS_PER_CALL needs bumping
    // again. See [[feedback-silent-overflow-break]].
    if (overflow_hit) {
        static uint32_t s_overflow_probe = 0;
        if ((++s_overflow_probe & 31) == 0) {
            debugf("hwtiles: chunk cap %d hit %lu times — tiles dropped "
                   "(prio=%d vis=%d chunks=%d/%d uniq=%d)\n",
                   MAX_CHUNKS_PER_CALL, (unsigned long)s_overflow_drops,
                   (int)priority_draw, n_visible, n_chunks,
                   MAX_CHUNKS_PER_CALL, n_unique);
            s_overflow_drops = 0;
        }
    }

    // Close final chunk + reset its slot-map entries.
    if (n_unique > 0 && n_chunks < MAX_CHUNKS_PER_CALL) {
        chunk_vis_end[n_chunks] = n_visible;
        chunk_uniq   [n_chunks] = n_unique;
        chunk_ring_ix[n_chunks] = s_ring_ix;
        n_chunks++;
        for (int k = 0; k < n_unique; k++)
            s_code_to_slot[s_used_codes[k]] = 0xffff;
        s_ring_ix = (uint8_t)((s_ring_ix + 1) % K_ATLAS_RING);
    }

    // ---- Pass 2: upload + draw each chunk ---------------------------------
    const uint64_t pass2_t0 = get_ticks_us();
    n64_profile::tile_call_pass1_us = (uint32_t)(pass2_t0 - pass1_t0);
    if (n_chunks == 0) {
        n64_profile::tile_call_pass2_us    = 0;
        n64_profile::tile_call_vis         = (uint32_t)n_visible;
        n64_profile::tile_call_uniq_total  = 0;
        n64_profile::tile_call_chunks      = 0;
        n64_profile::tile_call_tlut_evicts = 0;
        n64_profile::tile_call_prims       = 0;
        return;
    }

    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);
    rdpq_set_tile(TILE0, FMT_CI4,    0, ATLAS_PITCH, NULL);
    rdpq_set_tile(TILE1, FMT_RGBA16, 0, 0,           NULL);
    // TILE2 mirrors TILE0's TMEM region but with s.mask=3 so the 8-pixel
    // tile texture wraps every 8 texels. Lets a horizontal run of K identical
    // (slot, colour, y) tiles draw as one rect of width 8*K instead of K
    // separate texture_rectangles. Only valid for in_pair=0 tiles — in_pair=1
    // would need s to start at 8 and wrap to 8 (not 0), which mask=3 can't
    // express; those runs fall back to the single-emit path.
    {
        rdpq_tileparms_t parms2 = {};
        parms2.s.mask = 3;
        rdpq_set_tile(TILE2, FMT_CI4, 0, ATLAS_PITCH, &parms2);
    }

    // 16-slot CI4 palette LRU cache in TMEM. TILE.palette is a 4-bit index
    // so we can keep up to 16 distinct 16-entry tile_tlut[] palettes resident
    // and switch via rdpq_set_tile (1 RDP cmd) instead of re-uploading via
    // rdpq_tex_upload_tlut (3 RDP cmds) per draw. tile colours are 7-bit
    // (0..127) but only a handful appear per frame — 16 slots covers OutRun's
    // tile-layer working set with near-100% hit rate.
    constexpr int N_TLUT_SLOTS = 16;
    int      tlut_colour[N_TLUT_SLOTS];
    uint32_t tlut_seq   [N_TLUT_SLOTS] = {};
    for (int i = 0; i < N_TLUT_SLOTS; i++) tlut_colour[i] = -1;
    uint32_t next_seq = 1;
    int      cur_tile_palette  = 0;  // matches the parms=NULL set_tile above
    int      cur_tile2_palette = 0;  // matches the mask-3 init above
    // Single-element colour→slot cache. With 18 palette switches over 289
    // visibles the average run length is ~16 cells; this skips the 16-slot
    // LRU scan on the cache hit path.
    int      prev_colour = -1;
    int      prev_slot   = 0;

    int vis_start = 0;
    for (int c = 0; c < n_chunks; c++)
    {
        const int n_uniq_c       = chunk_uniq[c];
        const int vis_end        = chunk_vis_end[c];
        const int atlas_h_c      = n_uniq_c * ATLAS_TILE_H;   // one tile per 8 lines
        const int atlas_bytes_c  = n_uniq_c * ATLAS_TILE_BYTES;
        const int rgba16_texels  = atlas_bytes_c >> 1;
        uint8_t* atlas_c         = s_atlas_ring[chunk_ring_ix[c]];

        data_cache_hit_writeback(atlas_c, atlas_bytes_c);
        rdpq_set_tile_size(TILE0, 0, 0, 8, atlas_h_c);
        rdpq_set_tile_size(TILE2, 0, 0, 8, atlas_h_c);
        rdpq_set_texture_image_raw(0, PhysicalAddr(atlas_c),
                                   FMT_RGBA16, ATLAS_PITCH / 2, atlas_h_c);
        rdpq_load_block(TILE1, 0, 0, rgba16_texels, ATLAS_PITCH);

        int i = vis_start;
        while (i < vis_end)
        {
            const Visible& v = s_visible[i];
            const int colour = v.colour;

            int slot;
            if (colour == prev_colour) {
                slot = prev_slot;
            } else {
                // Locate (or evict an LRU entry for) this colour's palette slot.
                slot = -1;
                uint32_t best_seq = ~0u;
                int best_i = 0;
                for (int s = 0; s < N_TLUT_SLOTS; s++) {
                    if (tlut_colour[s] == colour) { slot = s; break; }
                    if (tlut_seq[s] < best_seq) { best_seq = tlut_seq[s]; best_i = s; }
                }
                if (slot < 0) {
                    slot = best_i;
                    rdpq_tex_upload_tlut((uint16_t*)&tile_tlut[colour * 16], slot * 16, 16);
                    tlut_colour[slot] = colour;
                    n64_profile::tile_tlut_uploads++;
                }
                prev_colour = colour;
                prev_slot   = slot;
            }
            tlut_seq[slot] = ++next_seq;

            const int t_base = v.slot * ATLAS_TILE_H;

            // Look for a horizontal run of identical (atlas slot, colour, y)
            // tiles at x+8, x+16, ... — with the per-tile pack every slot
            // starts at s=0 so any run can be coalesced via TILE2 (s.mask=3).
            int run_end = i + 1;
            int next_x  = v.x + 8;
            while (run_end < vis_end) {
                const Visible& n = s_visible[run_end];
                if (n.slot != v.slot || n.colour != colour
                    || n.y != v.y || n.x != next_x)
                    break;
                run_end++;
                next_x += 8;
            }
            const int run_len = run_end - i;

            if (run_len > 1) {
                if (slot != cur_tile2_palette) {
                    rdpq_tileparms_t parms = {};
                    parms.palette = (uint8_t)slot;
                    parms.s.mask  = 3;
                    rdpq_set_tile(TILE2, FMT_CI4, 0, ATLAS_PITCH, &parms);
                    cur_tile2_palette = slot;
                }
                rdpq_texture_rectangle(TILE2,
                    v.x, v.y, v.x + 8 * run_len, v.y + 8,
                    0, t_base);
                n64_profile::prim_count++;
            } else {
                if (slot != cur_tile_palette) {
                    rdpq_tileparms_t parms = {};
                    parms.palette = (uint8_t)slot;
                    rdpq_set_tile(TILE0, FMT_CI4, 0, ATLAS_PITCH, &parms);
                    cur_tile_palette = slot;
                }
                rdpq_texture_rectangle(TILE0,
                    v.x, v.y, v.x + 8, v.y + 8,
                    0, t_base);
                n64_profile::prim_count++;
            }

            i = run_end;
        }
        vis_start = vis_end;
    }

    // Per-call telemetry — consumed by the outlier logger in n64main to
    // diagnose tbg cost variance. Sum chunk uniques (atlas LOAD work) and
    // derive eviction count from the tlut_uploads delta.
    uint32_t uniq_total = 0;
    for (int c = 0; c < n_chunks; c++) uniq_total += (uint32_t)chunk_uniq[c];
    n64_profile::tile_call_vis         = (uint32_t)n_visible;
    n64_profile::tile_call_uniq_total  = uniq_total;
    n64_profile::tile_call_chunks      = (uint32_t)n_chunks;
    n64_profile::tile_call_tlut_evicts =
        n64_profile::tile_tlut_uploads - pre_tlut_uploads;
    n64_profile::tile_call_prims = n64_profile::prim_count - pre_prim_count;
    n64_profile::tile_call_pass2_us = (uint32_t)(get_ticks_us() - pass2_t0);
}

// RDP path for the text layer. Same chunked-atlas + LOAD_BLOCK strategy as
// render_rdp_tile_layers (see that function for the TMEM packing rationale);
// the text-layer differences are confined to Pass 1 decode:
//   * walks the 32x64 text_ram grid instead of tile_ram
//   * no scrolling; tile origin is shifted by -192 (matches the CPU path)
//   * Colour is 3-bit (0..7) — only the first 8 TLUT slots are touched
//   * tile bank is always tile_banks[0] (text codes are 9-bit)
void hwtiles::render_rdp_text_layer(const uint16_t* tile_tlut,
                                    uint8_t priority_draw,
                                    int x_offset, int y_offset)
{
    // Per-tile pack + TILE2 horizontal coalesce, mirroring
    // render_rdp_tile_layers. Score digit runs (e.g. "0000000") collapse to
    // one wider rect via TILE2's s.mask=3 wrap. HUD has well under 32 unique
    // chars; steady-state is a single chunk.
    constexpr int ATLAS_MAX        = 32;
    constexpr int ATLAS_PITCH      = 8;
    constexpr int ATLAS_TILE_H     = 8;
    constexpr int ATLAS_TILE_BYTES = ATLAS_TILE_H * ATLAS_PITCH; // 64
    constexpr int ATLAS_BYTES      = ATLAS_MAX * ATLAS_TILE_BYTES; // 2048
    constexpr int K_ATLAS_RING     = 8;
    constexpr int MAX_CHUNKS_PER_CALL = 8;

    struct Visible { int16_t x, y; uint16_t slot; uint8_t colour; };

    // Text grid is 32x64 = 2048 cells; HUD usually populates a small fraction.
    static Visible  s_visible[2048];
    static uint16_t s_used_codes[ATLAS_MAX];
    static uint16_t s_code_to_slot[NUM_TILES];
    static bool     s_slot_map_initted = false;
    if (!s_slot_map_initted) {
        for (int i = 0; i < NUM_TILES; i++) s_code_to_slot[i] = 0xffff;
        s_slot_map_initted = true;
    }
    alignas(8) static uint8_t s_atlas_ring[K_ATLAS_RING][ATLAS_BYTES];
    static uint8_t s_ring_ix = 0;

    int chunk_vis_end[MAX_CHUNKS_PER_CALL];
    int chunk_uniq   [MAX_CHUNKS_PER_CALL];
    int chunk_ring_ix[MAX_CHUNKS_PER_CALL];
    int n_chunks = 0;
    uint8_t* atlas_cur = s_atlas_ring[s_ring_ix];

    // ---- Pass 1: collect visible tiles + build atlas chunks ---------------
    int n_visible = 0;
    int n_unique  = 0;

    // Walk only the on-screen rows. Text grid is 32 rows of 8px but
    // S16_HEIGHT (224) caps usable text to my=0..27. Past that the cells
    // would be culled anyway; early-break saves the per-row text_ram scan.
    const int last_my = (S16_HEIGHT - 1) >> 3;
    uint32_t TileIndex = 0;
    for (int my = 0; my <= last_my; my++)
    {
        const int y = 8 * my;

        for (int mx = 0; mx < 64; mx++, TileIndex += 2)
        {
            const uint16_t Code_raw =
                (text_ram[TileIndex + 0] << 8) | text_ram[TileIndex + 1];

            if (((Code_raw >> 15) & 1) != priority_draw)
                continue;

            uint32_t Code = Code_raw & 0x1ff;
            Code += tile_banks[0] * 0x1000;
            Code &= (NUM_TILES - 1);
            if (Code == 0) continue;

            const int x = 8 * mx - 192;
            if (x <= -8 || x >= s16_width_noscale) continue;

            const int Colour = (Code_raw >> 9) & 0x07;

            uint16_t slot = s_code_to_slot[Code];
            if (slot == 0xffff) {
                if (n_unique >= ATLAS_MAX) {
                    if (n_chunks >= MAX_CHUNKS_PER_CALL) break;
                    chunk_vis_end[n_chunks] = n_visible;
                    chunk_uniq   [n_chunks] = n_unique;
                    chunk_ring_ix[n_chunks] = s_ring_ix;
                    n_chunks++;
                    for (int k = 0; k < n_unique; k++)
                        s_code_to_slot[s_used_codes[k]] = 0xffff;
                    n_unique = 0;
                    s_ring_ix = (uint8_t)((s_ring_ix + 1) % K_ATLAS_RING);
                    atlas_cur = s_atlas_ring[s_ring_ix];
                }
                slot = (uint16_t)n_unique;
                s_code_to_slot[Code] = slot;
                s_used_codes[n_unique] = (uint16_t)Code;
                // Per-tile pack: tile data in left half of 8 TMEM lines;
                // right half is padding (set_tile_size caps s at 8).
                uint32_t* dst =
                    (uint32_t*)&atlas_cur[n_unique * ATLAS_TILE_BYTES];
                const uint32_t* src =
                    hwtiles_fetch_tile(tiles_pi_addr, (uint32_t)Code);
                for (int r = 0; r < 8; r++)
                    dst[r * 2] = src[r];
                n_unique++;
            }

            s_visible[n_visible].x      =
                (int16_t)(x + x_offset + config.s16_x_off);
            s_visible[n_visible].y      = (int16_t)(y + y_offset);
            s_visible[n_visible].slot   = slot;
            s_visible[n_visible].colour = (uint8_t)Colour;
            n_visible++;
        }
    }

    if (n_unique > 0 && n_chunks < MAX_CHUNKS_PER_CALL) {
        chunk_vis_end[n_chunks] = n_visible;
        chunk_uniq   [n_chunks] = n_unique;
        chunk_ring_ix[n_chunks] = s_ring_ix;
        n_chunks++;
        for (int k = 0; k < n_unique; k++)
            s_code_to_slot[s_used_codes[k]] = 0xffff;
        s_ring_ix = (uint8_t)((s_ring_ix + 1) % K_ATLAS_RING);
    }

    // ---- Pass 2: upload + draw each chunk ---------------------------------
    if (n_chunks == 0) return;

    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);
    rdpq_set_tile(TILE0, FMT_CI4,    0, ATLAS_PITCH, NULL);
    rdpq_set_tile(TILE1, FMT_RGBA16, 0, 0,           NULL);
    {
        rdpq_tileparms_t parms2 = {};
        parms2.s.mask = 3;
        rdpq_set_tile(TILE2, FMT_CI4, 0, ATLAS_PITCH, &parms2);
    }

    // 16-slot CI4 palette LRU cache (same pattern as render_rdp_tile_layers).
    // Text uses only Colour 0..7, so the cache reaches a steady state where
    // each colour occupies a fixed slot and only first-use uploads happen.
    constexpr int N_TLUT_SLOTS = 16;
    int      tlut_colour[N_TLUT_SLOTS];
    uint32_t tlut_seq   [N_TLUT_SLOTS] = {};
    for (int i = 0; i < N_TLUT_SLOTS; i++) tlut_colour[i] = -1;
    uint32_t next_seq = 1;
    int      cur_tile_palette  = 0;
    int      cur_tile2_palette = 0;
    int      prev_colour = -1;
    int      prev_slot   = 0;

    int vis_start = 0;
    for (int c = 0; c < n_chunks; c++)
    {
        const int n_uniq_c       = chunk_uniq[c];
        const int vis_end        = chunk_vis_end[c];
        const int atlas_h_c      = n_uniq_c * ATLAS_TILE_H;
        const int atlas_bytes_c  = n_uniq_c * ATLAS_TILE_BYTES;
        const int rgba16_texels  = atlas_bytes_c >> 1;
        uint8_t* atlas_c         = s_atlas_ring[chunk_ring_ix[c]];

        data_cache_hit_writeback(atlas_c, atlas_bytes_c);
        rdpq_set_tile_size(TILE0, 0, 0, 8, atlas_h_c);
        rdpq_set_tile_size(TILE2, 0, 0, 8, atlas_h_c);
        rdpq_set_texture_image_raw(0, PhysicalAddr(atlas_c),
                                   FMT_RGBA16, ATLAS_PITCH / 2, atlas_h_c);
        rdpq_load_block(TILE1, 0, 0, rgba16_texels, ATLAS_PITCH);

        int i = vis_start;
        while (i < vis_end)
        {
            const Visible& v = s_visible[i];
            const int colour = v.colour;

            int slot;
            if (colour == prev_colour) {
                slot = prev_slot;
            } else {
                slot = -1;
                uint32_t best_seq = ~0u;
                int best_i = 0;
                for (int s = 0; s < N_TLUT_SLOTS; s++) {
                    if (tlut_colour[s] == colour) { slot = s; break; }
                    if (tlut_seq[s] < best_seq) { best_seq = tlut_seq[s]; best_i = s; }
                }
                if (slot < 0) {
                    slot = best_i;
                    rdpq_tex_upload_tlut((uint16_t*)&tile_tlut[colour * 16], slot * 16, 16);
                    tlut_colour[slot] = colour;
                    n64_profile::text_tlut_uploads++;
                }
                prev_colour = colour;
                prev_slot   = slot;
            }
            tlut_seq[slot] = ++next_seq;

            const int t_base = v.slot * ATLAS_TILE_H;

            // Horizontal coalesce: collapse runs of identical (slot, colour,
            // y) chars at x+8, x+16, ... into one wider rect via TILE2's
            // s.mask=3 wrap. Common in score digit runs.
            int run_end = i + 1;
            int next_x  = v.x + 8;
            while (run_end < vis_end) {
                const Visible& n = s_visible[run_end];
                if (n.slot != v.slot || n.colour != colour
                    || n.y != v.y || n.x != next_x)
                    break;
                run_end++;
                next_x += 8;
            }
            const int run_len = run_end - i;

            if (run_len > 1) {
                if (slot != cur_tile2_palette) {
                    rdpq_tileparms_t parms = {};
                    parms.palette = (uint8_t)slot;
                    parms.s.mask  = 3;
                    rdpq_set_tile(TILE2, FMT_CI4, 0, ATLAS_PITCH, &parms);
                    cur_tile2_palette = slot;
                }
                rdpq_texture_rectangle(TILE2,
                    v.x, v.y, v.x + 8 * run_len, v.y + 8,
                    0, t_base);
                n64_profile::prim_count++;
            } else {
                if (slot != cur_tile_palette) {
                    rdpq_tileparms_t parms = {};
                    parms.palette = (uint8_t)slot;
                    rdpq_set_tile(TILE0, FMT_CI4, 0, ATLAS_PITCH, &parms);
                    cur_tile_palette = slot;
                }
                rdpq_texture_rectangle(TILE0,
                    v.x, v.y, v.x + 8, v.y + 8,
                    0, t_base);
                n64_profile::prim_count++;
            }

            i = run_end;
        }
        vis_start = vis_end;
    }
}

