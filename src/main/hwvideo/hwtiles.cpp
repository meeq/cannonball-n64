#include <cstring> // memcpy
#include <malloc.h> // memalign
#include <libdragon.h>
#include "globals.hpp"
#include "romloader.hpp"
#include "hwvideo/hwtiles.hpp"
#include "frontend/config.hpp"
#include "n64/tile_cache_rsp.hpp"
#include <cstring>

namespace n64_profile {
    extern uint32_t prim_count;
    extern uint32_t tile_tlut_uploads;
    extern uint32_t text_tlut_uploads;
    extern uint32_t tile_call_vis;
    extern uint32_t tile_call_vis_bg;
    extern uint32_t tile_call_vis_fg;
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
// 1024 slots × 32 B = 32 KiB pixels + 2 KiB tags. Originally pinned to
// 256 when post-prime heap was ~4 KiB on 4 MiB; subsequent memory
// optimisation work loosened that budget to ~210 KiB free post-prime,
// so the cache can hold its original-measured working set again.
//
// Hit rate scales with slots / working_set. At 1024 slots vs ~150
// unique codes per frame, conflict misses are negligible — original
// measurement was 91% hits, pass 1 from ~4640 us → ~2549 us in OUT.
//
// Heap-allocated in hwtiles::init() — has to run after roms.load on
// 4 MiB (ROM load needs ~1 MiB contiguous; pre-roms BSS growth bumps
// up against that ceiling). Slots double as DMA targets — invalidate
// before issuing the PI-DMA so the CPU sees fresh bytes through the
// cached alias. On a hit the inner pack loop reads through the same
// cached alias; lines stay hot in L1 across the chunk's inner loop.
namespace {
    constexpr uint32_t TILE_CACHE_SLOTS = 1024;
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

// ============================================================================
// Per-layer pre-render cache (Expansion Pak only). One instance per BG/FG
// layer; both run the same code with different EffPage/scroll inputs.
//
// Background per [[project-tbg-chunks-bound]]: tbg pass-2 costs ~14 ms in
// heavy scenes from emitting ~150 per-cell texture_rectangles for BG+FG
// tile layers, each carrying ~20 us of RDP+RSP setup. Tile_ram content is
// effectively static in steady-state gameplay (0 dirty cells per frame
// across the attract loop) — the parallax effect is done by scrolling the
// camera over a fixed panorama, not by rewriting tile_ram.
//
// Pre-rendering each layer's 1024×512 logical tilemap into a CI4 surface
// lets each frame emit ONE wrap-aware tex_blit per layer (~1-3 ms typical)
// instead of ~75 individual texture_rectangles per layer. CI4 storage
// means TLUT changes don't invalidate the cache; only EffPage/tile_banks
// changes do.
//
// Gated behind is_memory_expanded() per [[feedback-n64-expansion-pak]]
// (4 MiB baseline can't afford 512 KB; 8 MiB unlocks the consistent 30 fps
// path). 4 MiB users keep the per-cell emit fallback unchanged.
namespace tile_cache {
    constexpr int CACHE_W_PX        = 1024;
    constexpr int CACHE_H_PX        = 512;
    constexpr int CACHE_STRIDE      = CACHE_W_PX / 2;  // CI4
    constexpr int CACHE_BYTES       = CACHE_STRIDE * CACHE_H_PX;  // 262144
    constexpr int CACHE_CELLS_X     = 128;
    constexpr int CACHE_CELLS_Y     = 64;

    struct Layer {
        uint8_t*  buf;
        surface_t surface;

        // Per-layer TLUT scratch — must be PER-LAYER because rdpq_tex_upload
        // _tlut is async: CPU writes here, issues a queued upload command,
        // RDP DMAs from this address later. If BG and FG shared one scratch
        // buffer, FG's overwrite would race with BG's pending DMA → both
        // layers end up reading the SAME (FG) palette. Manifests as
        // alternating-frame flicker (BG cache appears to use FG's palette).
        alignas(8) uint16_t scratch_tlut[16];

        // Invalidation shadow.
        uint16_t  shadow_effpage;
        uint8_t   shadow_tile_banks[2];
        bool      shadow_valid;

        // Dominant palette + coverage from the last render.
        uint8_t   palette_idx;       // 0xff = nothing rendered yet
        uint16_t  cells_in;          // cells matching dominant
        uint16_t  cells_out;         // cells with other palettes

        // Telemetry.
        uint32_t  renders;
        uint32_t  blits;
        uint32_t  subrects;
    };

    Layer s_bg = {};     // page=1
    Layer s_fg = {};     // page=0
    bool  s_enabled = false;
    // Set whenever Video::write_tile* / clear_tile_ram modifies tile_ram.
    // update_and_blit AND-checks this against shadow_matches before
    // skipping the rebuild — if dirty, the cache is stale even though
    // EffPage / tile_banks match. Cleared after each render() call.
    bool  s_dirty = false;

    // Scratch cell list shared by BG and FG renders (their renders run
    // sequentially per frame). Pass 1 walks every cell once, building both
    // the palette histogram AND this list of every valid (priority-0,
    // Code != 0) cell. Pass 2 then walks just this list — no second pass
    // over tile_ram. Saves the ~3.3 ms tile_ram walk per layer.
    //
    // Worst-case fill: 128 * 64 = 8192 cells. We've measured up to 4080
    // valid cells in heavy scenes (music select, both layers same data).
    // Sizing for the full theoretical max keeps the bound assertion-clean.
    struct CellEntry {
        uint16_t pal_ix;     // 0..127, valid cell-colour palette index
        uint16_t Code;       // tile_banks-remapped, masked to NUM_TILES-1
        uint32_t dst_offset; // byte offset into L.buf
    };
    constexpr int CELL_LIST_MAX = CACHE_CELLS_X * CACHE_CELLS_Y;  // 8192
    CellEntry s_cell_list[CELL_LIST_MAX];

    bool init_layer(Layer& L, const char* name)
    {
        // Uncached: CPU pass-2 writes go straight to RAM, bypassing the
        // write-allocate fill that costs ~30 cycles per cache line on the
        // R4300. For our stride-512 paste, every 4-byte write hits a
        // different cache line, so write-allocate would otherwise fill
        // 16 bytes per cell-row from RAM (~8 misses per cell) just to
        // immediately overwrite 4 of them. RSP also writes via SP DMA
        // (zero_async), which already bypasses CPU dcache — making the
        // surface uncached keeps the two writers consistent and lets us
        // drop the post-paste data_cache_hit_writeback entirely.
        L.buf = (uint8_t*)malloc_uncached_aligned(64, CACHE_BYTES);
        if (!L.buf) {
            debugf("tile_cache(%s): malloc_uncached_aligned(64, %d) failed\n",
                   name, CACHE_BYTES);
            return false;
        }
        std::memset(L.buf, 0, CACHE_BYTES);
        L.surface = surface_make_linear(L.buf, FMT_CI4, CACHE_W_PX, CACHE_H_PX);
        L.shadow_valid = false;
        L.palette_idx = 0xff;
        debugf("tile_cache(%s): enabled — %d KiB at %p (uncached)\n",
               name, CACHE_BYTES >> 10, L.buf);
        return true;
    }

    bool init()
    {
        if (s_bg.buf && s_fg.buf) return s_enabled;
        if (!is_memory_expanded()) {
            s_enabled = false;
            return false;
        }
        if (!init_layer(s_bg, "bg")) { s_enabled = false; return false; }
        if (!init_layer(s_fg, "fg")) { s_enabled = false; return false; }
        s_enabled = true;
        return true;
    }

    bool is_enabled() { return s_enabled; }

    void mark_dirty() { s_dirty = true; }

    inline bool shadow_matches(const Layer& L, uint16_t effpage,
                               uint8_t bank0, uint8_t bank1)
    {
        return L.shadow_valid
            && L.shadow_effpage == effpage
            && L.shadow_tile_banks[0] == bank0
            && L.shadow_tile_banks[1] == bank1;
    }

    // ----- Cold-render path (called on EffPage / tile_banks invalidation).
    //
    // Pass 1: walk all 8192 cells, count palette usage among priority-0
    // cells with non-zero Code. Pick the most common palette ("dominant")
    // — that's the one the cache surface will be baked against.
    //
    // Pass 2: re-walk the cells, memcpy pixels into the cache surface only
    // for cells whose Colour matches dominant. Cells with other palettes
    // stay as the memset-0 background (alpha-transparent at blit time) and
    // are emitted per-cell by the regular tbg loop.
    //
    // The same code handles BG and FG — caller supplies the appropriate
    // EffPage and the Layer& whose surface receives the baked cells.
    void render(Layer& L, uint8_t* tile_ram, uint16_t EffPage,
                const uint8_t* tile_banks, uint32_t tiles_pi_addr,
                const char* dbg_name)
    {
        const uint64_t t_kick = get_ticks_us();
        // Kick the RSP-side zero of L.buf BEFORE Pass 1 so its SP DMA fill
        // (~0.7 ms for 256 KiB at the measured 368 MB/s) hides behind the
        // CPU's tile-ram walk + histogram (~0.5 ms). We sync below right
        // before Pass 2 first writes to L.buf. SP DMA writes bypass the
        // CPU dcache, so no zeroing-side writeback is needed here — only
        // the post-Pass-2 writeback of the CPU-side cell pastes remains.
        n64::tile_cache_rsp::zero_async(L.buf, CACHE_BYTES);
        const uint64_t t_p1 = get_ticks_us();

        // Pass 1 — single walk: build the palette histogram AND the
        // cell list of every valid (priority-0, Code != 0) cell. Pass 2
        // below replays that list instead of re-walking tile_ram, saving
        // ~3.3 ms per layer.
        uint16_t palette_count[128] = {};
        int cell_count = 0;
        for (int my = 0; my < CACHE_CELLS_Y; my++)
        {
            const bool my_top = my < 32;
            const uint32_t my_offset = (2 * 64 * my) & 0xfff;
            const uint32_t base_L =
                64 * 32 * 2 * ((EffPage >> (my_top ? 0 : 8)) & 0x0f) + my_offset;
            const uint32_t base_R =
                64 * 32 * 2 * ((EffPage >> (my_top ? 4 : 12)) & 0x0f) + my_offset;

            const uint32_t cell_row_byte_base =
                (uint32_t)my * 8 * CACHE_STRIDE;

            for (int mx = 0; mx < CACHE_CELLS_X; mx++)
            {
                const uint32_t base = (mx < 64) ? base_L : base_R;
                const uint32_t TileIndex = base + ((2 * mx) & 0x7f);
                const uint16_t Data =
                    (tile_ram[TileIndex + 0] << 8) | tile_ram[TileIndex + 1];
                if (((Data >> 15) & 1) != 0) continue;  // priority-1
                uint32_t Code = Data & 0x1fff;
                Code = tile_banks[Code / 0x1000] * 0x1000 + Code % 0x1000;
                Code &= 0x1fff;  // NUM_TILES - 1, mirrored from hwtiles.hpp
                if (Code == 0) continue;
                const uint16_t pal_ix = (Data >> 6) & 0x7f;
                palette_count[pal_ix]++;
                CellEntry& e = s_cell_list[cell_count++];
                e.pal_ix     = pal_ix;
                e.Code       = (uint16_t)Code;
                e.dst_offset = cell_row_byte_base + (uint32_t)mx * 4;
            }
        }
        assertf(cell_count <= CELL_LIST_MAX,
                "tile_cache: cell_list overflow %d > %d",
                cell_count, CELL_LIST_MAX);
        int dom_palette = 0;
        int dom_count   = 0;
        int total_count = 0;
        for (int i = 0; i < 128; i++) {
            total_count += palette_count[i];
            if (palette_count[i] > dom_count) {
                dom_count   = palette_count[i];
                dom_palette = i;
            }
        }
        L.palette_idx = (uint8_t)dom_palette;
        L.cells_in    = (uint16_t)dom_count;
        L.cells_out   = (uint16_t)(total_count - dom_count);

        const uint64_t t_p1_done = get_ticks_us();
        // Sync the kicked-off RSP zero before Pass 2 starts writing cells.
        // If the SP DMA finished during Pass 1 (typical), this is ~free.
        n64::tile_cache_rsp::zero_sync();
        const uint64_t t_sync = get_ticks_us();

        // Pass 2 — replay the cell list, paste cells matching dom_palette.
        // No tile_ram walk: the cell_list already contains every valid
        // cell's (Code, dst_offset), so this is purely a list scan + fetch
        // + write per matching cell.
        for (int i = 0; i < cell_count; i++)
        {
            const CellEntry& e = s_cell_list[i];
            if (e.pal_ix != dom_palette) continue;
            const uint32_t* src = hwtiles_fetch_tile(tiles_pi_addr, e.Code);
            uint8_t* dst = L.buf + e.dst_offset;
            for (int r = 0; r < 8; r++) {
                *(uint32_t*)(dst + r * CACHE_STRIDE) = src[r];
            }
        }

        const uint64_t t_p2 = get_ticks_us();
        // No writeback: L.buf is uncached, so CPU writes have already
        // landed in RAM. RDP reads via SP DMA (which bypasses CPU cache)
        // see the correct bytes immediately.
        L.renders++;
        debugf("tile_cache(%s): rebuilt — dom=0x%02x in=%u out=%u  "
               "p1=%lu sync=%lu p2=%lu (us)\n",
               dbg_name, (unsigned)dom_palette, L.cells_in, L.cells_out,
               (unsigned long)(t_p1_done - t_p1),
               (unsigned long)(t_sync   - t_p1_done),
               (unsigned long)(t_p2     - t_sync));
        (void)t_kick;
    }

    // Emit a wrap-aware CI4 blit of the visible 320×224 window. tex_blit
    // strip-walks CI4 automatically; up to 4 sub-blits handle X+Y wrap.
    // TLUT slot is bound by the caller (palette-cycle-safe).
    void blit_window(Layer& L, int dst_x, int dst_y,
                     int src_x, int src_y)
    {
        // Wrap math against cache dims (1024 × 512).
        int x_left  = src_x;
        int y_top   = src_y;
        int w_a     = CACHE_W_PX - x_left; if (w_a > 320) w_a = 320;
        int h_a     = CACHE_H_PX - y_top;  if (h_a > 224) h_a = 224;
        int w_b     = 320 - w_a;
        int h_b     = 224 - h_a;

        auto emit_one = [&](int sx, int sy, int sw, int sh,
                            int dx, int dy)
        {
            if (sw <= 0 || sh <= 0) return;
            rdpq_blitparms_t parms = {};
            parms.s0     = sx;
            parms.t0     = sy;
            parms.width  = sw;
            parms.height = sh;
            rdpq_tex_blit(&L.surface, (float)dx, (float)dy, &parms);
            L.subrects++;
        };

        emit_one(x_left, y_top, w_a, h_a, dst_x,         dst_y);
        if (w_b > 0)
            emit_one(0,      y_top, w_b, h_a, dst_x + w_a, dst_y);
        if (h_b > 0)
            emit_one(x_left, 0,     w_a, h_b, dst_x,         dst_y + h_a);
        if (w_b > 0 && h_b > 0)
            emit_one(0,      0,     w_b, h_b, dst_x + w_a, dst_y + h_a);

        L.blits++;
    }

    // Public entry: render the cache if state changed, then emit the blit.
    // Called twice per render_rdp_tile_layers — once for BG before any other
    // RDP work, once for FG after BG per-cell emits complete (so layering
    // BG-cache → BG-per-cell-non-dom → FG-cache → FG-per-cell-non-dom is
    // preserved). x_clamp mirrors the value the per-cell loop uses to map
    // (xScroll → tilemap-pixel origin); both must agree.
    void update_and_blit(Layer& L, const char* dbg_name,
                         uint8_t* tile_ram, uint16_t effpage,
                         const uint8_t* tile_banks, uint32_t tiles_pi_addr,
                         const uint16_t* tile_tlut,
                         int16_t x_clamp,
                         uint16_t xscroll, uint16_t yscroll,
                         int dst_x, int dst_y)
    {
        // Rebuild when keys differ OR when tile_ram was written since
        // the last render. The dirty flag catches the case where the
        // engine animates content within a static EffPage (Time Trials
        // music select — see [[ttrial-music-select-cache-dirty]]).
        if (s_dirty || !shadow_matches(L, effpage, tile_banks[0], tile_banks[1])) {
            render(L, tile_ram, effpage, tile_banks, tiles_pi_addr, dbg_name);
            L.shadow_effpage       = effpage;
            L.shadow_tile_banks[0] = tile_banks[0];
            L.shadow_tile_banks[1] = tile_banks[1];
            L.shadow_valid         = true;
        }

        // If the cache rendered zero cells (no content this layer for this
        // scene), skip the blit — nothing to draw.
        if (L.cells_in == 0) return;

        // RDP state for the cache blit. Mode + alpha-compare must match the
        // tile emit path so the cache pixels alpha-test the same way per-cell
        // emits do (CI4 index 0 = transparent).
        rdpq_set_mode_standard();
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_alphacompare(1);

        // Upload the dominant palette to TMEM TLUT slot 0, with entry 0's
        // alpha forced to 0 (transparent). Cache cells we didn't paste
        // (priority-1, Code==0, or non-dominant palette) are memset to CI4
        // index 0 and MUST alpha-test out so layers below show through.
        // Use the Layer's *own* scratch_tlut buffer — rdpq_tex_upload_tlut
        // is async and a shared static buffer would race when BG and FG
        // emit in the same frame.
        const uint16_t* src_tlut = &tile_tlut[(uint32_t)L.palette_idx * 16];
        for (int i = 0; i < 16; i++) L.scratch_tlut[i] = src_tlut[i];
        L.scratch_tlut[0] &= 0xfffe;
        data_cache_hit_writeback(L.scratch_tlut, sizeof(L.scratch_tlut));
        rdpq_tex_upload_tlut(L.scratch_tlut, 0, 16);

        // Bind TILE0 to CI4 + palette slot 0.
        rdpq_tileparms_t parms = {};
        parms.palette = 0;
        rdpq_set_tile(TILE0, FMT_CI4, 0, CACHE_STRIDE, &parms);

        // Source X must mirror the per-cell loop's `ox = (x_clamp - xScroll)
        // & 0x3ff` — same tilemap-pixel offset for the same screen position.
        // Y has no clamp transform; just wrap mod 512.
        const int src_x = ((int)x_clamp - (int)xscroll) & 0x3ff;
        const int src_y = yscroll & 0x1ff;
        blit_window(L, dst_x, dst_y, src_x, src_y);
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

    // Per-layer BG/FG caches (Expansion Pak only). Runs after the tile pixel
    // cache alloc above because cache fills read through hwtiles_fetch_tile,
    // which requires s_tile_cache_pix.
    tile_cache::init();

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

void hwtiles::mark_tile_cache_dirty()
{
    tile_cache::mark_dirty();
}
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

    // ---- BG + FG caches (Expansion Pak) -----------------------------------
    // When enabled, the dominant-palette BG and FG cells are pre-rendered
    // to CI4 cache surfaces and emitted as wrap-aware blits — BG at the
    // top of the function (under everything), FG between BG-per-cell and
    // FG-per-cell emits in pass 2 (so the layer order BG-cache → BG-rest →
    // FG-cache → FG-rest matches the original BG-then-FG painter order).
    // priority_draw=0 only — text/HUD layer takes the per-cell path.
    const bool use_cache = (priority_draw == 0) && tile_cache::is_enabled();

    // Pre-resolve scroll for both layers (used by both cache blits and to
    // avoid duplicating the text_ram override logic).
    uint16_t bg_xs = scroll_x[1], bg_ys = scroll_y[1];
    if ((bg_xs & 0x8000) != 0)
        bg_xs = (text_ram[0xf80 + 0x40] << 8) | text_ram[0xf80 + 0x41];
    if ((bg_ys & 0x8000) != 0)
        bg_ys = (text_ram[0xf16 + 0x40] << 8) | text_ram[0xf16 + 0x41];
    uint16_t fg_xs = scroll_x[0], fg_ys = scroll_y[0];
    if ((fg_xs & 0x8000) != 0)
        fg_xs = (text_ram[0xf80 + 0] << 8) | text_ram[0xf80 + 1];
    if ((fg_ys & 0x8000) != 0)
        fg_ys = (text_ram[0xf16 + 0] << 8) | text_ram[0xf16 + 1];

    if (use_cache) {
        // BG cache blit goes first — drawn under everything else.
        tile_cache::update_and_blit(
            tile_cache::s_bg, "bg",
            tile_ram, page[1], tile_banks, tiles_pi_addr, tile_tlut,
            x_clamp, bg_xs, bg_ys, x_offset, y_offset);
    }
    const uint8_t bg_cached_palette =
        use_cache ? tile_cache::s_bg.palette_idx : (uint8_t)0xff;
    const uint8_t fg_cached_palette =
        use_cache ? tile_cache::s_fg.palette_idx : (uint8_t)0xff;

    // ---- Pass 1: collect visible tiles + build atlas chunks ---------------
    int n_visible = 0;
    int n_unique  = 0;
    // Per-sub-layer visible counts so we can scope a BG-cache strategy
    // ([[project-tbg-chunks-bound]]). Captures the BG/FG ratio inside this
    // call's 246-vis / 154-prim total; the prim breakdown isn't strictly
    // additive (coalescing crosses pages) but the vis ratio is a strong
    // signal for where the bigger win lives.
    int n_vis_bg = 0;
    int n_vis_fg = 0;
    // BG-extent audit (lightweight, runs every frame). Tracks the smallest
    // bounding box of (mx, my) BG-page cells that have non-zero Code over
    // the lifetime of the session, plus the range of EffPage values seen.
    // Drives the pre-rendered BG cache surface sizing decision — without
    // this we'd over-allocate the cache to the theoretical 1024×512.
    static uint16_t s_bg_mx_min   = 0xffff, s_bg_mx_max   = 0;
    static uint16_t s_bg_my_min   = 0xffff, s_bg_my_max   = 0;
    static uint32_t s_bg_cells_used = 0;
    static uint8_t  s_bg_cell_seen[128 * 64 / 8] = {};
    static uint16_t s_bg_effpage_seen[16] = {};
    static int      s_bg_effpage_n  = 0;
    static uint16_t s_bg_xscroll_min = 0xffff, s_bg_xscroll_max = 0;
    static uint16_t s_bg_yscroll_min = 0xffff, s_bg_yscroll_max = 0;
    static uint32_t s_audit_frames = 0;

    // Walk BG (page=1) first, then FG (page=0). BG must draw under FG.
    for (int pass = 0; pass < 2; pass++)
    {
        const int pass_vis_start = n_visible;
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
        // BG-extent audit: track scroll range + unique EffPage values.
        if (page_index == 1) {
            if (xScroll < s_bg_xscroll_min) s_bg_xscroll_min = xScroll;
            if (xScroll > s_bg_xscroll_max) s_bg_xscroll_max = xScroll;
            if (yScroll < s_bg_yscroll_min) s_bg_yscroll_min = yScroll;
            if (yScroll > s_bg_yscroll_max) s_bg_yscroll_max = yScroll;
            bool seen = false;
            for (int k = 0; k < s_bg_effpage_n; k++)
                if (s_bg_effpage_seen[k] == EffPage) { seen = true; break; }
            if (!seen && s_bg_effpage_n < 16) {
                s_bg_effpage_seen[s_bg_effpage_n++] = EffPage;
            }
        }

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
                Code &= 0x1fff;  // NUM_TILES - 1, mirrored from hwtiles.hpp
                if (Code == 0) continue;

                // BG/FG cache: skip cells already drawn by their layer's
                // cache blit. BG cache emitted before this function starts;
                // FG cache emitted in pass 2 between BG and FG per-cell
                // emits. Cells whose Colour doesn't match dominant fall
                // through to the per-cell path.
                if (use_cache) {
                    const uint8_t cell_col = (Data >> 6) & 0x7f;
                    if (page_index == 1 && cell_col == bg_cached_palette) continue;
                    if (page_index == 0 && cell_col == fg_cached_palette) continue;
                }

                // BG-extent audit. Only count BG (page_index==1) cells —
                // FG cache decision is separate. Cheap O(1) bitmap ops.
                if (page_index == 1) {
                    const int bit_idx = my * 128 + mx;
                    const int byte_ix = bit_idx >> 3;
                    const uint8_t bit = (uint8_t)(1u << (bit_idx & 7));
                    if (!(s_bg_cell_seen[byte_ix] & bit)) {
                        s_bg_cell_seen[byte_ix] |= bit;
                        s_bg_cells_used++;
                    }
                    if (mx < s_bg_mx_min) s_bg_mx_min = (uint16_t)mx;
                    if (mx > s_bg_mx_max) s_bg_mx_max = (uint16_t)mx;
                    if (my < s_bg_my_min) s_bg_my_min = (uint16_t)my;
                    if (my > s_bg_my_max) s_bg_my_max = (uint16_t)my;
                }

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
        // Capture per-sub-layer vis delta before we leave this pass.
        const int pass_vis = n_visible - pass_vis_start;
        if (pass == 0) n_vis_bg = pass_vis; else n_vis_fg = pass_vis;

        // When the per-layer cache is on, force chunk closure between BG
        // (pass=0) and FG (pass=1) so chunks don't span layers. Pass 2 can
        // then cleanly emit the FG cache blit between BG and FG chunks
        // without re-binding mid-chunk state.
        if (use_cache && pass == 0 && n_unique > 0
            && n_chunks < MAX_CHUNKS_PER_CALL)
        {
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
        // No per-cell chunks, but the FG cache may still need to be emitted
        // (case: all FG content fell into the dominant palette → no FG
        // visibles in the per-cell list, but the cache surface holds them).
        if (use_cache) {
            tile_cache::update_and_blit(
                tile_cache::s_fg, "fg",
                tile_ram, page[0], tile_banks, tiles_pi_addr, tile_tlut,
                x_clamp, fg_xs, fg_ys, x_offset, y_offset);
        }
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
    bool fg_cache_emitted = false;
    for (int c = 0; c < n_chunks; c++)
    {
        const int n_uniq_c       = chunk_uniq[c];
        const int vis_end        = chunk_vis_end[c];

        // Before processing the first chunk that contains FG visibles,
        // emit the FG cache blit. Chunks are forced to align with the
        // BG/FG boundary up in pass 1 when use_cache is on, so the check
        // here is just `chunk's vis_start >= n_vis_bg`. Also re-establish
        // the FG-chunk's TILE0/TILE1 state below because the cache blit
        // overwrites them.
        if (use_cache && !fg_cache_emitted && vis_start >= n_vis_bg) {
            tile_cache::update_and_blit(
                tile_cache::s_fg, "fg",
                tile_ram, page[0], tile_banks, tiles_pi_addr, tile_tlut,
                x_clamp, fg_xs, fg_ys, x_offset, y_offset);
            fg_cache_emitted = true;
            // Re-bind TILE0/TILE1/TILE2 base state — the FG cache blit
            // clobbered them. Match the original setup at the top of pass 2.
            rdpq_set_tile(TILE0, FMT_CI4,    0, ATLAS_PITCH, NULL);
            rdpq_set_tile(TILE1, FMT_RGBA16, 0, 0,           NULL);
            rdpq_tileparms_t parms2 = {};
            parms2.s.mask = 3;
            rdpq_set_tile(TILE2, FMT_CI4, 0, ATLAS_PITCH, &parms2);
            // Bypass the LRU's cached state — slot 0 now holds the FG
            // cache palette, and cur_tile_palette is no longer aligned
            // with what's on TILE0. Force the next bind_tile0 (whatever
            // palette) to re-issue set_tile.
            cur_tile_palette  = -1;
            cur_tile2_palette = -1;
            for (int s = 0; s < N_TLUT_SLOTS; s++) tlut_colour[s] = -1;
            prev_colour = -1;
        }

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

    // Final FG cache emit. Covers the case where all chunks were BG and
    // no chunk-start ever crossed n_vis_bg (i.e. zero FG visibles after
    // the cache's dominant-palette skip).
    if (use_cache && !fg_cache_emitted) {
        tile_cache::update_and_blit(
            tile_cache::s_fg, "fg",
            tile_ram, page[0], tile_banks, tiles_pi_addr, tile_tlut,
            x_clamp, fg_xs, fg_ys, x_offset, y_offset);
    }
    // Both BG and FG have had their chance to rebuild from the current
    // tile_ram contents — clear the dirty flag so subsequent frames
    // with no tile_ram writes can skip the rebuild.
    if (use_cache) tile_cache::s_dirty = false;

    // Per-call telemetry — consumed by the outlier logger in n64main to
    // diagnose tbg cost variance. Sum chunk uniques (atlas LOAD work) and
    // derive eviction count from the tlut_uploads delta.
    uint32_t uniq_total = 0;
    for (int c = 0; c < n_chunks; c++) uniq_total += (uint32_t)chunk_uniq[c];
    n64_profile::tile_call_vis         = (uint32_t)n_visible;
    n64_profile::tile_call_vis_bg      = (uint32_t)n_vis_bg;
    n64_profile::tile_call_vis_fg      = (uint32_t)n_vis_fg;
    n64_profile::tile_call_uniq_total  = uniq_total;
    n64_profile::tile_call_chunks      = (uint32_t)n_chunks;
    n64_profile::tile_call_tlut_evicts =
        n64_profile::tile_tlut_uploads - pre_tlut_uploads;
    n64_profile::tile_call_prims = n64_profile::prim_count - pre_prim_count;
    n64_profile::tile_call_pass2_us = (uint32_t)(get_ticks_us() - pass2_t0);

    // BG-extent audit: dump the running high-water marks every 256 frames so
    // we can size the pre-rendered BG cache surface against actual usage
    // instead of the theoretical 1024×512. See [[project-tbg-chunks-bound]].
    if (++s_audit_frames == 256 && priority_draw == 0) {
        if (s_bg_mx_max >= s_bg_mx_min && s_bg_my_max >= s_bg_my_min) {
            const int bbox_w = (s_bg_mx_max - s_bg_mx_min + 1) * 8;
            const int bbox_h = (s_bg_my_max - s_bg_my_min + 1) * 8;
            debugf("[bg-audit] cells=%lu bbox_xy=[%u..%u]×[%u..%u] "
                   "(%dpx × %dpx) xscroll=[%u..%u] yscroll=[%u..%u] "
                   "effpages=%d (",
                   (unsigned long)s_bg_cells_used,
                   (unsigned)s_bg_mx_min, (unsigned)s_bg_mx_max,
                   (unsigned)s_bg_my_min, (unsigned)s_bg_my_max,
                   bbox_w, bbox_h,
                   (unsigned)s_bg_xscroll_min, (unsigned)s_bg_xscroll_max,
                   (unsigned)s_bg_yscroll_min, (unsigned)s_bg_yscroll_max,
                   s_bg_effpage_n);
            for (int k = 0; k < s_bg_effpage_n; k++)
                debugf("0x%04x%s", s_bg_effpage_seen[k],
                       k + 1 < s_bg_effpage_n ? "," : "");
            debugf(")\n");
        }
        s_audit_frames = 0;
    }
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

