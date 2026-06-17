/***************************************************************************
    N64 / libdragon Render.

    Produces a scratch RGBA5551 surface from the engine's palette-indexed
    pixel buffer, then rdpq_tex_blit's it into the display framebuffer with
    a vertical letterbox.
***************************************************************************/

#pragma once

#include "../stdint.hpp"
#include "../globals.hpp"
#include <libdragon.h>

// Per-phase timings (microseconds, EMA-smoothed). Updated by n64main and
// finalize_frame; consumed by the CANNONBALL_LOG_PROFILE dip log in n64main.
namespace n64_profile
{
    extern uint32_t prepare_us;   // total prepare_frame
    extern uint32_t render_us;    // RDP blit + present
    extern uint32_t tick_us;      // engine tick (excludes rasterize)
    extern uint32_t wait_us;      // display_get() vsync block
    extern uint32_t audio_us;     // audio.tick (Z80 catchup + mixer_poll)
    extern uint32_t aud_z80_us;   // advance_z80_audio
    extern uint32_t aud_pcm_us;   // reconcile_pcm
    extern uint32_t aud_mix_us;   // mixer_poll loop

    // Health window — accumulated over the gap between two dip-log emissions
    // (the dip log resets them after printing). Captures regressions like the
    // off-frame-skip rdpq backpressure case, which collapsed fps without any
    // single per-phase EMA crossing a visible threshold.
    extern uint32_t dropped_frames; // frames where total wall > FRAME_BUDGET_US
    extern uint32_t min_wait_us;    // floor of wait_us in window — low = RDP-bound
    extern uint32_t max_total_us;   // peak un-smoothed frame total in window
    extern uint32_t window_frames;  // frames the window spans (for ratios)

    // Cache-pressure snapshots — cumulative counters captured at dip-log emit
    // so the next line can show per-window deltas (working-set thrash signal).
    extern uint32_t snap_spr_extracts;
    extern uint32_t snap_spr_hits;
    extern uint32_t snap_spr_overflows;
    extern uint32_t snap_tile_tlut_uploads;
    extern uint32_t snap_text_tlut_uploads;
    // Cumulative cache counters incremented at the call sites (hwtiles).
    // hwsprites exposes equivalents through public methods.
    extern uint32_t tile_tlut_uploads;
    extern uint32_t text_tlut_uploads;

    // Per-call telemetry for render_rdp_tile_layers — set at the end of the
    // call and read by the outlier logger. Used to diagnose why tbg cost
    // varies 3.4× (~6.5ms → ~22ms) in tile-heavy, low-sprite scenes. Single
    // call per frame, so the value at end-of-call == frame value.
    extern uint32_t tile_call_vis;          // n_visible (tiles drawn)
    extern uint32_t tile_call_vis_bg;       // n_visible from BG (page=1) sub-layer
    extern uint32_t tile_call_vis_fg;       // n_visible from FG (page=0) sub-layer
    extern uint32_t tile_call_uniq_total;   // sum of chunk_uniq[] (atlas LOAD work)
    extern uint32_t tile_call_chunks;       // n_chunks (atlas rebuilds)
    extern uint32_t tile_call_tlut_evicts;  // TLUT LRU evictions this call
    extern uint32_t tile_call_prims;        // rdpq_texture_rectangle calls (post-coalesce)
    extern uint32_t tile_call_pass1_us;     // pass 1 (walk tilemap + build atlas) us
    extern uint32_t tile_call_pass2_us;     // pass 2 (chunk setup + emit) us
    extern uint32_t tile_call_dma_fetches;  // hwtiles_fetch_tile invocations (cache lookups)
    extern uint32_t tile_call_dma_us;       // us spent in hwtiles_fetch_tile (hits + misses)
    extern uint32_t tile_call_dma_misses;   // PI-DMA actually issued (cache misses)

    // Per-call telemetry for hwsprites::render_rdp. Set at end of call,
    // consumed by the outlier logger to diagnose why spr peaks at ~6.5 ms
    // (down from ~5-6 ms baseline after the 14-slot TLUT cache landed —
    // [[project-hwsprites-tlut-cache]]). vis is the post-filter sprite count
    // that actually emitted; prims includes shadow's mask+body second rect
    // so prims > vis on shadow-heavy frames. loads is LOAD_BLOCK count
    // (atlas surface change); tlut_uploads is rdpq_tex_upload_tlut count
    // (palette cache miss + shadow scratch refresh).
    extern uint32_t spr_call_vis;
    extern uint32_t spr_call_prims;
    extern uint32_t spr_call_loads;
    extern uint32_t spr_call_tlut_uploads;
    extern uint32_t spr_call_us;
    extern uint32_t spr_call_ovf;       // atlas overflows this call (drives bimodality)

    // Scratch composite blit (320×224 RGBA5551 with alpha-compare). Skipped
    // when CPU road_fg path didn't run; instrumented to confirm that's a real
    // RDP fillrate saving and not just a wash. raw_composite_us is the per-
    // frame value; composite_us is the EMA.
    extern uint32_t composite_us;
    extern uint32_t raw_composite_us;
    extern uint32_t composite_skipped_frames;   // count of skipped blits in window

    enum {
        SUB_ROAD_BG,
        SUB_TILE_BG,
        SUB_ROAD_FG,
        SUB_SPRITE,
        SUB_TEXT,
        SUB_COUNT
    };
    extern uint32_t sub_us[SUB_COUNT];

    // Raw (un-smoothed) per-frame copies of the same buckets, written every
    // render iter alongside the EMAs. The outlier logger in n64main needs to
    // print what *this* frame cost — EMAs would dilute a 50 ms spike with
    // surrounding normal frames and the breakdown wouldn't add up to total.
    extern uint32_t raw_sub_us[SUB_COUNT];
    extern uint32_t raw_wait_us;
    extern uint32_t raw_aud_z80_us;
    extern uint32_t raw_aud_pcm_us;
    extern uint32_t raw_aud_mix_us;

    // Per-frame RDP primitive counter. Each rasterizer increments this at
    // every rdpq_*_rectangle / rdpq_tex_blit site (~1500/frame at peak,
    // ~30us of L1 store traffic). Read by the per-call counters
    // (spr_call_prims, tile_call_prims) for the outlier logger.
    extern uint32_t prim_count;
}

class Render
{
public:
    Render();
    ~Render();

    // Bring up libdragon's display + rdpq early so their internal allocations
    // (2 framebuffers + RSPQ command buffer + rdpq state) land in a fresh,
    // contiguous heap region. Called from main() right after the sprite
    // atlas pool so all big-contiguous allocs front-load before ROM load /
    // audio init fragment the heap. Idempotent — Render::init() detects this
    // has already happened via the initialized flag and skips re-entry.
    void boot_display();

    bool init(int src_width, int src_height,
              int scale, int video_mode, int scanlines);
    void disable();
    bool start_frame();
    bool finalize_frame();

    // Palette plumbing — Video::refresh_palette() calls convert_palette()
    // when S16 palette RAM changes; set_shadow_intensity() scales the shadow
    // half of the LUT at init.
    void convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1);
    void set_shadow_intensity(float f);

    // Engine scratch surface — RGBA5551, sized src_width × src_height. Held
    // through KSEG1 so CPU writes go straight to RDRAM via the store buffer
    // (no cache-line allocate, no writeback), then DMA-blit'd onto the
    // framebuffer in finalize_frame. hwroad::render_foreground writes here.
    uint16_t* scratch_uc() const { return scratch_uc_ptr; }

    // RGBA5551 engine palette LUT (S16_PALETTE_ENTRIES * 2 entries). Lower
    // half is normal colors, upper half is shadow-darkened. Indexed by engine
    // palette entry; entry 0 is encoded as zero so the alpha-compare composite
    // treats it as transparent.
    const uint16_t* rgb_lut() const { return rgb; }

private:
    // Palette Lookup — held as RGBA5551 (libdragon's DEPTH_16_BPP framebuffer
    // format). Extended slots hold shadow colors at +S16_PALETTE_ENTRIES.
    uint16_t rgb[S16_PALETTE_ENTRIES * 2];

    // Tile-palette TLUT cache: 128 contiguous 16-entry RGBA5551 TLUTs that
    // mirror the engine tile palette layout. Engine palette entry E is read
    // by the tile renderer as (nTilePalette<<3) + c where c in [0..15], so
    // each slot i covers entries [i*8 .. i*8+15] (overlapping slots share 8
    // entries). Slot i is a contiguous 16-entry RGBA5551 block at offset
    // i*16, with entry 0 forced to 0 (alpha LSB clear) — the RDP alpha-
    // compare composite then drops CI4 pixval=0 to transparent, matching
    // the CPU mask path's `if (c0) buf[0] = nPalette + c0;`.
    // 8-byte aligned so rdpq_tex_upload_tlut can DMA from any slot directly.
    static constexpr int TILE_TLUT_SLOTS = 128;
    static constexpr int TILE_TLUT_SLOT_SIZE = 16;
    alignas(8) uint16_t tile_tlut[TILE_TLUT_SLOTS * TILE_TLUT_SLOT_SIZE];

    // Sprite-palette TLUT cache. Sprite renderer reads engine palette as
    // 0x800 + (color<<4) + pix, color in [0..127], pix in [0..15] — so slots
    // are non-overlapping (stride 16, slot size 16) covering engine indices
    // [0x800 .. 0x1800). Entry 0 of each slot is forced to 0 so the RDP
    // alpha-compare composite drops CI4 pixval=0 to transparent (the atlas
    // extractor pads EOR-terminated short rows with 0 as well).
    static constexpr int SPRITE_TLUT_SLOTS = 128;
    static constexpr int SPRITE_TLUT_SLOT_SIZE = 16;
    static constexpr uint32_t SPRITE_PAL_BASE = 0x800;
    alignas(8) uint16_t sprite_tlut[SPRITE_TLUT_SLOTS * SPRITE_TLUT_SLOT_SIZE];

    // Source S16 buffer dimensions (eg. 320x224).
    int src_width, src_height;
    int video_mode;
    int scanlines;
    int scale;

    // Shadow intensity multiplier (0..255).
    int shadow_multi;

    // Scratch RGBA5551 surface populated by hwroad foreground (and possibly
    // future CPU layers) and blit'd onto the framebuffer in finalize_frame.
    // scratch_pixels is the cached allocation; scratch_uc_ptr is its KSEG1
    // alias used by all CPU writers — see scratch_uc() accessor above.
    uint16_t*  scratch_pixels;
    uint16_t*  scratch_uc_ptr;
    surface_t  scratch_surface;
    int        y_offset;          // letterbox top (pixels)

    bool       initialized;
};
