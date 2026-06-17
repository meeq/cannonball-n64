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

    uint32_t dropped_frames = 0;
    uint32_t min_wait_us    = 0xFFFFFFFF;
    uint32_t max_total_us   = 0;
    uint32_t window_frames  = 0;

    uint32_t snap_spr_extracts      = 0;
    uint32_t snap_spr_hits          = 0;
    uint32_t snap_spr_overflows     = 0;
    uint32_t snap_tile_tlut_uploads = 0;
    uint32_t snap_text_tlut_uploads = 0;
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
    : rgb{}, tile_tlut{}, sprite_tlut{}, src_width(0), src_height(0),
      video_mode(0), scanlines(0), scale(1), shadow_multi(0),
      y_offset(0), initialized(false)
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

bool Render::init(int src_w, int src_h,
                  int /*scale_in*/, int video_mode_in, int scanlines_in)
{
    src_width  = src_w;
    src_height = src_h;
    scale      = 1;
    video_mode = video_mode_in;
    scanlines  = scanlines_in;

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

    // TEMP-PROBE: reset RDP perf counters at frame start. Read at end of
    // frame (post-rspq_wait) to compute pipe-busy / clock ratio. Answers
    // whether the RDP rasterizer is saturated (fps-floor is RDP-bound) or
    // idle (fps-floor is CPU/RSP feed-bound). ares doesn't emulate these
    // counters; valid only on real hardware.
    *DP_STATUS = DP_WSTATUS_RESET_CLOCK_COUNTER
               | DP_WSTATUS_RESET_PIPE_COUNTER
               | DP_WSTATUS_RESET_CMD_COUNTER
               | DP_WSTATUS_RESET_TMEM_COUNTER;

    const int x = (disp->width - src_width) / 2;

    // Clip all subsequent RDP work to the 320x224 game viewport so partial-
    // edge tiles/sprites don't bleed into the top/bottom letterbox. The CPU
    // tile renderer clips per-pixel against the 320x224 pixels[] buffer; the
    // RDP path emits whole 8x8 textured rectangles, so a tile at engine
    // y=223 would otherwise extend 7 px into the bottom letterbox (visible
    // as a cyan strip going up the first hill — that's arcade "bezel-hidden"
    // data leaking past the visible area).
    rdpq_set_scissor(x, y_offset, x + src_width, y_offset + src_height);


    // Road background: emit one rdpq_fill_rectangle per same-color band.
    // Configures fill mode internally; safe to call before the composite blit.
    uint64_t rbg_t0 = get_ticks_us();
    hwroad.render_rdp_background(rgb, x, y_offset, src_width);
    uint64_t rbg_t1 = get_ticks_us();
    n64_profile::raw_sub_us[n64_profile::SUB_ROAD_BG] = (uint32_t)(rbg_t1 - rbg_t0);
    n64_profile::sub_us[n64_profile::SUB_ROAD_BG] =
        (n64_profile::sub_us[n64_profile::SUB_ROAD_BG] * 7
         + (uint32_t)(rbg_t1 - rbg_t0)) >> 3;

    // Tile background + foreground: writeback the TLUT cache (the engine
    // updates it from cached convert_palette writes) so the RDP TLUT load
    // DMAs see fresh bytes, then walk both tilemap pages in one shared
    // atlas/draw pass. BG (page 1) draws under FG (page 0), and both sit
    // under the engine composite, which still owns road_fg/sprites/text via
    // pixels[]. priority=1 tiles stay on the CPU (drawn in front of sprites
    // at engine layer-5).
    data_cache_hit_writeback(tile_tlut, sizeof(tile_tlut));

    uint64_t tbg_t0 = get_ticks_us();
    video.tile_layer->render_rdp_tile_layers(tile_tlut, 0, x, y_offset);
    uint64_t tbg_t1 = get_ticks_us();
    n64_profile::raw_sub_us[n64_profile::SUB_TILE_BG] = (uint32_t)(tbg_t1 - tbg_t0);
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
        hwroad.emit_foreground_lores_rdp(x, y_offset);
    }

    // Sprite layer via RDP: opaque sprites are one blit each, shadow-flagged
    // sprites get a darken pass + body pass. Lives above the composite (so it
    // occludes road_fg) and below text.
    data_cache_hit_writeback(sprite_tlut, sizeof(sprite_tlut));
    uint64_t spr_t0 = get_ticks_us();
    video.sprite_layer->render_rdp(8, sprite_tlut, x, y_offset);
    uint64_t spr_t1 = get_ticks_us();
    n64_profile::raw_sub_us[n64_profile::SUB_SPRITE] = (uint32_t)(spr_t1 - spr_t0);
    n64_profile::sub_us[n64_profile::SUB_SPRITE] =
        (n64_profile::sub_us[n64_profile::SUB_SPRITE] * 7
         + (uint32_t)(spr_t1 - spr_t0)) >> 3;

    // Text layer sits on top of everything. Uses the same TLUT cache as the
    // tile layers (Colour is 3-bit here, only slots 0..7 are touched).
    uint64_t txt_t0 = get_ticks_us();
    video.tile_layer->render_rdp_text_layer(tile_tlut, 1, x, y_offset);
    uint64_t txt_t1 = get_ticks_us();
    n64_profile::raw_sub_us[n64_profile::SUB_TEXT] = (uint32_t)(txt_t1 - txt_t0);
    n64_profile::sub_us[n64_profile::SUB_TEXT] =
        (n64_profile::sub_us[n64_profile::SUB_TEXT] * 7
         + (uint32_t)(txt_t1 - txt_t0)) >> 3;


    // TEMP-PROBE: drain queue + read RDP busy counters. One line per 60
    // frames. Valid on real hardware only.
    {
        rspq_wait();
        const uint32_t dp_clock = *DP_CLOCK;
        const uint32_t dp_pipe  = *DP_PIPE_BUSY;
        const uint32_t dp_cmd   = *DP_BUSY;
        const uint32_t dp_tmem  = *DP_TMEM_BUSY;
        static uint32_t s_probe_frame = 0;
        if ((s_probe_frame++ % 60) == 0) {
            const uint32_t pipe_pct = dp_clock ? (uint32_t)((uint64_t)dp_pipe * 100u / dp_clock) : 0;
            const uint32_t cmd_pct  = dp_clock ? (uint32_t)((uint64_t)dp_cmd  * 100u / dp_clock) : 0;
            const uint32_t tmem_pct = dp_clock ? (uint32_t)((uint64_t)dp_tmem * 100u / dp_clock) : 0;
            debugf("RDPbusy[%5lu] clock=%lu pipe=%lu (%lu%%) cmd=%lu (%lu%%) tmem=%lu (%lu%%)\n",
                   (unsigned long)s_probe_frame,
                   (unsigned long)dp_clock,
                   (unsigned long)dp_pipe, (unsigned long)pipe_pct,
                   (unsigned long)dp_cmd,  (unsigned long)cmd_pct,
                   (unsigned long)dp_tmem, (unsigned long)tmem_pct);
        }
    }

    rdpq_detach_show();
    return true;
}
