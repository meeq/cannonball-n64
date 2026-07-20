/***************************************************************************
    N64 Render — all-RDP composite.

    finalize_frame() composites every layer straight onto the framebuffer
    in z-order: road_bg fill → tile_bg → tile_fg → road_fg (RDP mask + CI4
    TLUTs built per-line in prepare_frame) → sprites → text → FPS overlay.
***************************************************************************/

#include "rendersurface.hpp"
#include "platform.hpp"
#include "video.hpp"
#include "hwvideo/hwroad.hpp"
#include "hwvideo/hwtiles.hpp"
#include "hwvideo/hwsprites.hpp"
#include "frontend/config.hpp"
#include "n64/hwroad_rdp.hpp"
#include "n64/hwroad_rdp_rsp.hpp"
#include <rdp.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <malloc.h>

// Diagnostic: bracket each RDP pass with rspq_wait() so per-pass time splits
// into CPU-emit (command queue construction) vs RDP-drain (rasterizer/TMEM
// work the RDP still had to do after CPU returned), and read the DP busy
// counters at end of frame for pipe/cmd/TMEM utilisation. Serializing CPU
// and RDP kills frame-rate while enabled — attribution builds only, never
// shipping. The DP counters are hardware-only (ares reads them as 0).
// Override via -DN64_PROFILE_RDP_DRAIN=1; per-pass history and how to read
// the numbers live in the rdp-perf-model memory note.
#ifndef N64_PROFILE_RDP_DRAIN
#define N64_PROFILE_RDP_DRAIN 0
#endif

namespace n64_profile
{
    uint32_t prepare_us = 0;
    uint32_t render_us  = 0;
    uint32_t tick_us    = 0;
    uint32_t wait_us    = 0;
    uint32_t audio_us   = 0;
    uint32_t aud_z80_us = 0;
    uint32_t aud_pcm_us = 0;
    uint32_t aud_mix_us = 0;
    uint32_t sub_us[SUB_COUNT] = {0};
    uint32_t prim_count = 0;

    uint32_t raw_sub_us[SUB_COUNT] = {0};
    uint32_t raw_wait_us    = 0;
    uint32_t raw_aud_z80_us = 0;
    uint32_t raw_aud_pcm_us = 0;
    uint32_t raw_aud_mix_us = 0;
    uint32_t aud_starve     = 0;

    uint32_t dropped_frames = 0;
    uint32_t min_wait_us    = 0xFFFFFFFF;
    uint32_t max_total_us   = 0;
    uint32_t window_frames  = 0;

    uint32_t snap_spr_extracts      = 0;
    uint32_t snap_spr_hits          = 0;
    uint32_t snap_spr_overflows     = 0;
    uint32_t snap_tile_tlut_uploads = 0;
    uint32_t snap_text_tlut_uploads = 0;
    uint32_t snap_aud_starve        = 0;
    uint32_t tile_tlut_uploads      = 0;
    uint32_t text_tlut_uploads      = 0;

    uint32_t tile_call_vis          = 0;
    uint32_t tile_call_vis_bg       = 0;
    uint32_t tile_call_vis_fg       = 0;
    uint32_t tile_call_uniq_total   = 0;
    uint32_t tile_call_chunks       = 0;
    uint32_t tile_call_tlut_evicts  = 0;
    uint32_t tile_call_prims        = 0;
    uint32_t tile_call_pass1_us     = 0;
    uint32_t tile_call_pass2_us     = 0;
    uint32_t tile_call_dma_fetches  = 0;
    uint32_t tile_call_dma_us       = 0;
    uint32_t tile_call_dma_misses   = 0;

    uint32_t spr_call_vis           = 0;
    uint32_t spr_call_prims         = 0;
    uint32_t spr_call_loads         = 0;
    uint32_t spr_call_tlut_uploads  = 0;
    uint32_t spr_call_us            = 0;
    uint32_t spr_call_ovf           = 0;
}

Render::Render()
    : rgb{}, tile_tlut{}, sprite_tlut{}, tile_tlut_stage{},
      sprite_tlut_stage{}, src_width(0), src_height(0),
      shadow_multi(0), y_offset(0), initialized(false)
{
}

Render::~Render()
{
    disable();
}

// Packs the S16 5-bit RGB channels into libdragon's FMT_RGBA16 framebuffer
// format: RRRRR GGGGG BBBBB A (5/5/5/1). Alpha LSB is 1 for opaque texels —
// finalize_frame composites with alpha compare so the LUT's zero-alpha
// entries (palette index 0, see convert_palette) reveal the road background.
static inline uint16_t pack_rgba5551(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0x1F) << 11) |
                      ((g & 0x1F) << 6)  |
                      ((b & 0x1F) << 1)  |
                      0x1);
}

void Render::convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1)
{
    adr >>= 1;

    // Palette index 0 is the engine's transparency sentinel: tiles, sprites
    // and text skip writing it, and start_frame() zeroes the scratch surface
    // each frame. Encode it as zero (alpha LSB = 0) so finalize_frame's
    // alpha-compare composite drops these pixels and the underlying RDP
    // road-background fill shows through.
    if (adr == 0)
    {
        rgb[0] = 0;
        rgb[S16_PALETTE_ENTRIES] = 0;
        return;
    }

    uint16_t new_color = pack_rgba5551(r1, g1, b1);
    rgb[adr] = new_color;

    // Shadow color: scale by shadow_multi/255. Stored in the upper half of
    // the table to match the original SDL backend's layout (osprites looks
    // up shadow indices at +S16_PALETTE_ENTRIES).
    uint32_t sr = (r1 * shadow_multi) / 255;
    uint32_t sg = (g1 * shadow_multi) / 255;
    uint32_t sb = (b1 * shadow_multi) / 255;
    rgb[adr + S16_PALETTE_ENTRIES] = pack_rgba5551(sr, sg, sb);

    // Mirror into tile_tlut. Each tile slot i covers engine entries
    // [i*8 .. i*8+15] (stride 8, slot size 16), so adr lies in slot
    // i_hi = adr>>3 (at entry adr&7) and also in i_lo = i_hi-1 (at
    // entry 8+(adr&7)), when those slots are in range. Slot entry 0 is
    // always 0 so the alpha-compare composite drops CI4 pixval=0.
    constexpr uint32_t MAX_TILE_PAL = TILE_TLUT_SLOTS * 8;  // = 1024
    if (adr < MAX_TILE_PAL + 8)
    {
        int slot_hi = (int)(adr >> 3);
        int entry_hi = (int)(adr & 7);
        if (slot_hi < TILE_TLUT_SLOTS)
        {
            tile_tlut[slot_hi * TILE_TLUT_SLOT_SIZE + entry_hi] =
                (entry_hi == 0) ? 0 : new_color;
        }
        int slot_lo = slot_hi - 1;
        if (slot_lo >= 0)
        {
            // entry_in_slot = adr - slot_lo*8 = adr - (slot_hi-1)*8 = 8 + entry_hi
            tile_tlut[slot_lo * TILE_TLUT_SLOT_SIZE + 8 + entry_hi] = new_color;
        }
    }

    // Mirror into sprite_tlut. Sprite renderer reads (0x800 + color*16 + pix),
    // so slots are non-overlapping (stride 16, slot size 16) covering engine
    // indices [0x800 .. 0x1800). Slot entry 0 is forced to 0 so the alpha-
    // compare composite drops CI4 pixval=0 to transparent.
    constexpr uint32_t SPRITE_PAL_END =
        SPRITE_PAL_BASE + SPRITE_TLUT_SLOTS * SPRITE_TLUT_SLOT_SIZE;
    if (adr >= SPRITE_PAL_BASE && adr < SPRITE_PAL_END)
    {
        const uint32_t rel = adr - SPRITE_PAL_BASE;
        const int slot  = (int)(rel >> 4);
        const int entry = (int)(rel & 15);
        sprite_tlut[slot * SPRITE_TLUT_SLOT_SIZE + entry] =
            (entry == 0) ? 0 : new_color;
    }
}

void Render::set_shadow_intensity(float f)
{
    shadow_multi = (int)std::round(255.0f * f);
}

void Render::boot_display()
{
    if (initialized) return;

    // FILTERS_DISABLED skips VI's AA + dedither + resample chain. VI shares
    // RDRAM bandwidth with CPU/RDP, and we're CPU-bandwidth bound, so handing
    // that back is a free win — the engine output is already correctly sized
    // for the framebuffer so there's nothing to filter.
    display_init(n64::FB_RES, n64::FB_DEPTH, n64::FB_COUNT,
                 GAMMA_NONE, FILTERS_DISABLED);
    rdpq_init();

    initialized = true;
}

bool Render::init(int src_w, int src_h)
{
    src_width  = src_w;
    src_height = src_h;

    // boot_display() is expected to have run in main() before any heavy
    // heap consumer; this guard is the safety net for unit tests or future
    // call sites that skip the early boot.
    boot_display();

    // Engine resolution is fixed at S16_WIDTH x S16_HEIGHT on N64 (widescreen
    // disabled, src_w / src_h always match). Assert the contract — anything
    // else is a config bug.
    assert(src_width == S16_WIDTH && src_height == S16_HEIGHT);

    // Vertical letterbox: engine is 224 tall, framebuffer 240.
    y_offset = (n64::FB_HEIGHT - src_height) / 2;
    if (y_offset < 0) y_offset = 0;

    return true;
}

void Render::disable()
{
    if (initialized)
    {
        rdpq_close();
        display_close();
        initialized = false;
    }
}

bool Render::start_frame()
{
    return true;
}

bool Render::finalize_frame()
{
    // display_get() blocks until a backbuffer frees — i.e. vsync wait. Time
    // it separately so we can tell vsync-bound from CPU-bound.
    uint64_t t0 = get_ticks_us();
    surface_t* disp = display_get();
    uint64_t t1 = get_ticks_us();
    const uint32_t raw_wait = (uint32_t)(t1 - t0);
    n64_profile::wait_us = (n64_profile::wait_us * 7 + raw_wait) >> 3;
    n64_profile::raw_wait_us = raw_wait;
    // Track the *floor* of wait_us across the window — a near-zero floor while
    // fps drops indicates rdpq backpressure: display_get returns instantly
    // because the previous frame is still draining the RDP queue. EMA on
    // wait_us hides this; we need the raw minimum.
    if (raw_wait < n64_profile::min_wait_us) n64_profile::min_wait_us = raw_wait;

    // Clear to black. Some scenes (music select, tunnels w/ skip-mode rows)
    // leave pixels above the road bands unpainted — no rbg fill (data&0x800
    // not set), no tile_bg paint (engine forces every tile palette slot 0
    // to RGBA=0, see convert_palette), and sprites/text don't cover the
    // sky region. Without an explicit clear those pixels keep whatever
    // was in the framebuffer from the previous frame — typically the
    // splash-screen white, hence "white sky on music select". The clear
    // costs ~150 µs (one full-screen fill rect) but eliminates a whole
    // class of cross-scene staleness bugs.
    
    rdpq_attach_clear(disp, NULL);

    // Snapshot the TLUT caches for this frame's RDP consumption. The engine
    // tick rewrites tile_tlut / sprite_tlut (convert_palette) while the
    // *previous* frame's queued load_tlut commands may still be pending —
    // the RDP DMAs them from RDRAM at command-execution time, so handing it
    // the live arrays lets a palette write from frame N leak into frame
    // N-1's draw (nondeterministic one-frame-early palettes during fades).
    // The staging copies are only ever written here, after display_get():
    // with FB_COUNT == 2 that call blocks until the previous frame is on
    // screen, which requires its RSP+RDP work to have fully drained — so
    // nothing in flight can still be reading them. The same guarantee is
    // what makes the sprite-atlas pool and TMEM scratch recycling in the
    // render calls below safe without an explicit rspq syncpoint. (The
    // road-RSP input buffers are written earlier, in prepare_frame, and
    // carry their own rspq_wait at build entry.)
    static_assert(n64::FB_COUNT == 2,
                  "cross-frame RDP-read safety relies on display_get() "
                  "draining the previous frame; a deeper swapchain needs an "
                  "explicit rspq syncpoint instead");
    memcpy(tile_tlut_stage, tile_tlut, sizeof(tile_tlut));
    memcpy(sprite_tlut_stage, sprite_tlut, sizeof(sprite_tlut));
    data_cache_hit_writeback(tile_tlut_stage, sizeof(tile_tlut_stage));
    data_cache_hit_writeback(sprite_tlut_stage, sizeof(sprite_tlut_stage));

    const int x = (disp->width - src_width) / 2;

    // Clip all subsequent RDP work to the 320x224 game viewport so partial-
    // edge tiles/sprites don't bleed into the top/bottom letterbox. The CPU
    // tile renderer clips per-pixel against the 320x224 pixels[] buffer; the
    // RDP path emits whole 8x8 textured rectangles, so a tile at engine
    // y=223 would otherwise extend 7 px into the bottom letterbox (visible
    // as a cyan strip going up the first hill — that's arcade "bezel-hidden"
    // data leaking past the visible area).
    rdpq_set_scissor(x, y_offset, x + src_width, y_offset + src_height);

#if N64_PROFILE_RDP_DRAIN
    uint32_t rbg_emit = 0, rbg_drain = 0, rbg_prims = 0;
    uint32_t tbg_emit = 0, tbg_drain = 0, tbg_prims = 0;
    uint32_t rfg_emit = 0, rfg_drain = 0, rfg_prims = 0;
    uint32_t spr_emit = 0, spr_drain = 0, spr_prims = 0;
    uint32_t txt_emit = 0, txt_drain = 0, txt_prims = 0;
    uint32_t prim_mark = 0;
    // Reset the DP busy counters at frame start; read after the final
    // per-pass drain below (queue empty by then, so the counts cover
    // exactly this frame's RDP work). Hardware-only — ares reads 0.
    *DP_STATUS = DP_WSTATUS_RESET_CLOCK_COUNTER
               | DP_WSTATUS_RESET_PIPE_COUNTER
               | DP_WSTATUS_RESET_CMD_COUNTER
               | DP_WSTATUS_RESET_TMEM_COUNTER;
#endif

    // Road background: emit one rdpq_fill_rectangle per same-color band.
    // Configures fill mode internally; safe to call before the composite blit.
#if N64_PROFILE_RDP_DRAIN
    prim_mark = n64_profile::prim_count;
#endif
    uint64_t rbg_t0 = get_ticks_us();
    hwroad.render_rdp_background(rgb, x, y_offset, src_width);
    uint64_t rbg_t1 = get_ticks_us();
    n64_profile::raw_sub_us[n64_profile::SUB_ROAD_BG] = (uint32_t)(rbg_t1 - rbg_t0);
#if N64_PROFILE_RDP_DRAIN
    rspq_wait();
    uint64_t rbg_t2 = get_ticks_us();
    rbg_emit  = (uint32_t)(rbg_t1 - rbg_t0);
    rbg_drain = (uint32_t)(rbg_t2 - rbg_t1);
    rbg_prims = n64_profile::prim_count - prim_mark;
#endif
    n64_profile::sub_us[n64_profile::SUB_ROAD_BG] =
        (n64_profile::sub_us[n64_profile::SUB_ROAD_BG] * 7
         + (uint32_t)(rbg_t1 - rbg_t0)) >> 3;

    // Tile background + foreground: walk both tilemap pages in one shared
    // atlas/draw pass, reading palettes from the frame-stable TLUT snapshot
    // (copied + written back above). BG (page 1) draws under FG (page 0),
    // and both sit under the engine composite, which still owns
    // road_fg/sprites/text via pixels[]. priority=1 tiles stay on the CPU
    // (drawn in front of sprites at engine layer-5).
#if N64_PROFILE_RDP_DRAIN
    prim_mark = n64_profile::prim_count;
#endif
    uint64_t tbg_t0 = get_ticks_us();
    video.tile_layer->render_rdp_tile_layers(tile_tlut_stage, 0, x, y_offset);
    uint64_t tbg_t1 = get_ticks_us();
    n64_profile::raw_sub_us[n64_profile::SUB_TILE_BG] = (uint32_t)(tbg_t1 - tbg_t0);
#if N64_PROFILE_RDP_DRAIN
    rspq_wait();
    uint64_t tbg_t2 = get_ticks_us();
    tbg_emit  = (uint32_t)(tbg_t1 - tbg_t0);
    tbg_drain = (uint32_t)(tbg_t2 - tbg_t1);
    tbg_prims = n64_profile::prim_count - prim_mark;
#endif
    n64_profile::sub_us[n64_profile::SUB_TILE_BG] =
        (n64_profile::sub_us[n64_profile::SUB_TILE_BG] * 7
         + (uint32_t)(tbg_t1 - tbg_t0)) >> 3;

    // RDP road_fg overlay. prepare_frame ran build_foreground_lores_rdp_rsp;
    // here we sync the RSP-written n_runs back and emit the prebuilt mask +
    // per-line TLUTs into rdpq, painting straight into the framebuffer.
    //
    // should_render_road_fg() mirrors the build-side predicate at
    // video.cpp's prepare_frame: when the engine suppresses road_fg (e.g.
    // music-select sets horizon_base = HORIZON_OFF), build was skipped and
    // line[]/runs_buf hold stale geometry from the prior frame. Emitting
    // that would paint last frame's road over an unrelated scene.
    if (n64::hwroad_rdp::should_render_road_fg()) {
        // Sync per-row n_runs back from the RSP build before emit walks
        // runs[][]. Cheap no-op once RSP has already drained.
        n64::hwroad_rdp_rsp::sync_runs();
#if N64_PROFILE_RDP_DRAIN
        prim_mark = n64_profile::prim_count;
        uint64_t rfg_t0 = get_ticks_us();
#endif
        hwroad.emit_foreground_lores_rdp(x, y_offset);
#if N64_PROFILE_RDP_DRAIN
        uint64_t rfg_t1 = get_ticks_us();
        rspq_wait();
        uint64_t rfg_t2 = get_ticks_us();
        rfg_emit  = (uint32_t)(rfg_t1 - rfg_t0);
        rfg_drain = (uint32_t)(rfg_t2 - rfg_t1);
        rfg_prims = n64_profile::prim_count - prim_mark;
#endif
    }

    // Sprite layer via RDP: opaque sprites are one blit each, shadow-flagged
    // sprites get a darken pass + body pass. Lives above the composite (so it
    // occludes road_fg) and below text.
#if N64_PROFILE_RDP_DRAIN
    prim_mark = n64_profile::prim_count;
#endif
    uint64_t spr_t0 = get_ticks_us();
    video.sprite_layer->render_rdp(8, sprite_tlut_stage, x, y_offset);
    uint64_t spr_t1 = get_ticks_us();
    n64_profile::raw_sub_us[n64_profile::SUB_SPRITE] = (uint32_t)(spr_t1 - spr_t0);
#if N64_PROFILE_RDP_DRAIN
    rspq_wait();
    uint64_t spr_t2 = get_ticks_us();
    spr_emit  = (uint32_t)(spr_t1 - spr_t0);
    spr_drain = (uint32_t)(spr_t2 - spr_t1);
    spr_prims = n64_profile::prim_count - prim_mark;
#endif
    n64_profile::sub_us[n64_profile::SUB_SPRITE] =
        (n64_profile::sub_us[n64_profile::SUB_SPRITE] * 7
         + (uint32_t)(spr_t1 - spr_t0)) >> 3;

    // Text layer sits on top of everything. Uses the same TLUT snapshot as
    // the tile layers (Colour is 3-bit here, only slots 0..7 are touched).
#if N64_PROFILE_RDP_DRAIN
    prim_mark = n64_profile::prim_count;
#endif
    uint64_t txt_t0 = get_ticks_us();
    video.tile_layer->render_rdp_text_layer(tile_tlut_stage, 1, x, y_offset);
    uint64_t txt_t1 = get_ticks_us();
    n64_profile::raw_sub_us[n64_profile::SUB_TEXT] = (uint32_t)(txt_t1 - txt_t0);
#if N64_PROFILE_RDP_DRAIN
    rspq_wait();
    uint64_t txt_t2 = get_ticks_us();
    txt_emit  = (uint32_t)(txt_t1 - txt_t0);
    txt_drain = (uint32_t)(txt_t2 - txt_t1);
    txt_prims = n64_profile::prim_count - prim_mark;
#endif
    n64_profile::sub_us[n64_profile::SUB_TEXT] =
        (n64_profile::sub_us[n64_profile::SUB_TEXT] * 7
         + (uint32_t)(txt_t1 - txt_t0)) >> 3;

#if N64_PROFILE_RDP_DRAIN
    // Print one line per second so the USB log stays scannable. The queue
    // is empty here (every pass drained above), so the DP counters cover
    // exactly this frame's RDP work.
    static uint32_t drain_log_frame = 0;
    if ((drain_log_frame++ % 60) == 0)
    {
        debugf("rdp[%5lu] rbg %3lup e=%4lu d=%5lu  tbg %4lup e=%4lu d=%4lu  "
               "rfg %4lup e=%4lu d=%5lu  spr %3lup e=%4lu d=%4lu  txt %3lup e=%4lu d=%4lu\n",
               (unsigned long)drain_log_frame,
               (unsigned long)rbg_prims,
               (unsigned long)rbg_emit, (unsigned long)rbg_drain,
               (unsigned long)tbg_prims,
               (unsigned long)tbg_emit, (unsigned long)tbg_drain,
               (unsigned long)rfg_prims,
               (unsigned long)rfg_emit, (unsigned long)rfg_drain,
               (unsigned long)spr_prims,
               (unsigned long)spr_emit, (unsigned long)spr_drain,
               (unsigned long)txt_prims,
               (unsigned long)txt_emit, (unsigned long)txt_drain);
        const uint32_t dp_clock = *DP_CLOCK;
        const uint32_t dp_pipe  = *DP_PIPE_BUSY;
        const uint32_t dp_cmd   = *DP_BUSY;
        const uint32_t dp_tmem  = *DP_TMEM_BUSY;
        const uint32_t pipe_pct = dp_clock ? (uint32_t)((uint64_t)dp_pipe * 100u / dp_clock) : 0;
        const uint32_t cmd_pct  = dp_clock ? (uint32_t)((uint64_t)dp_cmd  * 100u / dp_clock) : 0;
        const uint32_t tmem_pct = dp_clock ? (uint32_t)((uint64_t)dp_tmem * 100u / dp_clock) : 0;
        debugf("    dp: clock=%lu pipe=%lu%% cmd=%lu%% tmem=%lu%% (hw counters; 0 under ares)\n",
               (unsigned long)dp_clock, (unsigned long)pipe_pct,
               (unsigned long)cmd_pct,  (unsigned long)tmem_pct);
    }
#endif

    // No drain before present: the RDP keeps grinding this frame's queue
    // while the CPU moves on to audio + the next engine tick. Cross-frame
    // buffer-recycle safety is covered by the display_get() double-buffer
    // guarantee (see the TLUT-snapshot comment above) and the road build's
    // entry rspq_wait in prepare_frame.
    rdpq_detach_show();
    return true;
}
