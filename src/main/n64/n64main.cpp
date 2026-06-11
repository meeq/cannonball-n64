/***************************************************************************
    Cannonball N64 entry point.

    Replaces src/main/main.cpp on libdragon builds. Drives the engine via
    direct polling: there is no event queue, joypad is sampled once per
    frame, and the renderer always blocks on vsync via display_get().
***************************************************************************/

#include <libdragon.h>
#include <cstring>

#include "platform.hpp"
#include "save.hpp"
#include "rendersurface.hpp"
#include "hwroad_rsp.hpp"
#include "hwroad_rdp.hpp"
#include "hwroad_rdp_rsp.hpp"
#include "../hwvideo/hwsprites.hpp"

#include "../main.hpp"
#include "../video.hpp"
#include "../romloader.hpp"
#include "../trackloader.hpp"
#include "../stdint.hpp"

#include "../engine/outrun.hpp"
#include "../engine/oinputs.hpp"
#include "../engine/ooutputs.hpp"
#include "../engine/omusic.hpp"

#include "../frontend/config.hpp"

using namespace cannonball;

int    cannonball::state       = STATE_BOOT;
double cannonball::frame_ms    = 0;
int    cannonball::frame       = 0;
bool   cannonball::tick_frame  = true;
int    cannonball::fps_counter = 0;

Audio  cannonball::audio;
Input  input;
bool   pause_engine = false;

// Set to 1 to log heap stats at key boot milestones (post-ROM load,
// post-audio init). Useful for diagnosing OOM in the 4 MiB build; off in
// shipping builds so the ISViewer log isn't cluttered.
#define CANNONBALL_LOG_HEAP 0

namespace
{
    void boot_subsystems()
    {
        debug_init_usblog();
        debug_init_emulog();

        // Direct emux probe — bypass the debug_writer indirection. If ares
        // (or any emux-aware emulator) is running, this will appear in the
        // console even if debug_init_emulog() failed to register a writer.
        emux_log("cannonball: boot via emux_log\n");
        debugf("cannonball: boot via debugf\n");
        joypad_init();
        timer_init();

        // Bake the DFS payload as soon as possible — config.load() resolves
        // its paths through "rom:/" before any ROM I/O runs.
        int dfs_result = dfs_init(DFS_DEFAULT_LOCATION);
        if (dfs_result != DFS_ESUCCESS)
            debugf("dfs_init failed: %d\n", dfs_result);

        n64save::init();
    }

    void platform_overrides()
    {
        // libdragon DFS routes "rom:/..." through newlib. The generic no-XML
        // load() left rom_path = "./roms/" — point it at DFS instead.
        config.data.rom_path  = "rom:/roms/";
        config.data.res_path  = "rom:/res/";
        config.data.save_path = "/";   // EEPROM-backed, no filesystem write
        config.data.crc32     = 0;     // filename mode (no DFS dirent)

        // Cartridge / console port: skip the coin-up cycle. Outrun's
        // check_freeplay_start() credits up on START press when freeplay is on,
        // so the player goes straight from attract → game without a coin button.
        config.engine.freeplay = true;

        // Engine timing: the main loop is vsync-capped to ~60 Hz by
        // display_get(). tick_engine() only frame-skips when config.fps == 60
        // or 120 — leaving video.fps == 0 (30 Hz path) makes the engine tick
        // every iteration and run at 2x speed. video.fps = 1 selects
        // "60 Hz display, 30 Hz engine tick" which matches the arcade cadence.
        config.video.fps = 1;

        // Phase 4a audio: CPU-mixed YM2151 + SegaPCM → libdragon audio_push.
        // 22050 Hz keeps the chip emulators' per-frame cost manageable at the
        // 30 fps engine target; raise once budget permits.
        config.sound.enabled = 1;
        config.sound.rate    = 22050;
    }

    void tick_engine()
    {
        frame++;

        if (config.fps == 60)      tick_frame = frame & 1;
        else if (config.fps == 120) tick_frame = (frame & 3) == 1;

        input.poll();

        if (tick_frame)
        {
            oinputs.tick();
            oinputs.do_gear();
        }

        switch (state)
        {
            case STATE_GAME:
                if (tick_frame)
                {
                    if (input.has_pressed(Input::TIMER)) outrun.freeze_timer = !outrun.freeze_timer;
                    if (input.has_pressed(Input::PAUSE)) pause_engine = !pause_engine;
                    if (input.has_pressed(Input::MENU))  state = STATE_INIT_MENU;
                }
                if (!pause_engine || input.has_pressed(Input::STEP))
                {
                    outrun.tick(tick_frame);
                    if (tick_frame) input.frame_done();
                    // Z80 audio code is advanced from audio.tick() on a wall-
                    // clock schedule; ticking it here would over-clock it when
                    // the main loop is faster than the audio cadence and
                    // under-clock it when the renderer falls behind, both of
                    // which warp music tempo.
                }
                else if (tick_frame) input.frame_done();
                break;

            case STATE_INIT_GAME:
                tick_frame   = true;
                pause_engine = false;
                outrun.init();
                state = STATE_GAME;
                break;

            case STATE_MENU:
            case STATE_INIT_MENU:
                // Menu disabled in Phase 1 — boot straight into attract.
                state = STATE_INIT_GAME;
                break;
        }

        outrun.outputs->writeDigitalToConsole();
        if (tick_frame)
            input.set_rumble(outrun.outputs->is_set(OOutputs::D_MOTOR),
                             config.controls.rumble);
    }
}

int main(int /*argc*/, char* /*argv*/[])
{
    boot_subsystems();

    // Memory-map order: every big-contiguous heap allocation happens here,
    // *before* ROM load (which is the 2.3 MiB elephant that fragments the
    // remaining free space). Once all the must-be-contiguous chunks are in
    // place, ROMs fill the rest. Audio init's wav64 mixer + scratch buffers
    // come before ROMs for the same reason.
    //
    //   1. sprite atlas pool  (2 MiB EP / 1 MiB base)
    //   2. display_init        (2× 320×240 framebuffers, RSPQ buffer)
    //   3. config + audio init (wav64 mixer + 4-channel scratch)
    //   4. roms.load           (~2.3 MiB)
    //   5. video.init          (small allocs only — display + rdpq already up)
    config.load();
    platform_overrides();

    video.sprite_layer->atlas_init();
    video.boot_display();
#if CANNONBALL_LOG_HEAP
    {
        heap_stats_t hs; sys_get_heap_stats(&hs);
        debugf("heap post-display: used=%d free=%d\n", hs.used, hs.total - hs.used);
    }
#endif

    audio.init();
#if CANNONBALL_LOG_HEAP
    {
        heap_stats_t hs; sys_get_heap_stats(&hs);
        debugf("heap post-audio: used=%d free=%d\n", hs.used, hs.total - hs.used);
    }
#endif

    config.set_fps(config.video.fps);

    if (!roms.load_revb_roms(config.sound.fix_samples))
    {
        debugf("ROM load failed — DFS payload likely missing.\n");
        while (1) { /* halt */ }
    }

#if CANNONBALL_LOG_HEAP
    {
        heap_stats_t hs; sys_get_heap_stats(&hs);
        debugf("heap post-roms: used=%d free=%d\n", hs.used, hs.total - hs.used);
    }
#endif

    if (!omusic.load_widescreen_map(config.data.res_path))
        debugf("Widescreen tilemaps not loaded\n");

    if (!video.init(&roms, &config.video))
    {
        debugf("video.init failed\n");
        while (1) { /* halt */ }
    }

    // Bring up the hwroad RSP overlay. Cheap (one rspq_overlay_register +
    // a 14 KB malloc) — leave the runtime switch off so the CPU path stays
    // the default. Toggle n64::hwroad_rsp::enabled to A/B test.
    n64::hwroad_rsp::init();
    n64::hwroad_rdp::init();
    n64::hwroad_rdp_rsp::init();

    input.init(config.controls.pad_id,
               config.controls.keyconfig, config.controls.padconfig,
               config.controls.analog,    config.controls.axis,
               config.controls.invert,    config.controls.asettings);

    state = STATE_INIT_GAME;

    // Warm up the engine before the first render. The OutRun attract sequence
    // fades the sky palette in via opalette.cycle_sky_palette over several
    // vints and populates the tilemap incrementally. SDL hides this behind
    // ~60 fps frames so it's imperceptible; on N64 the first second runs at
    // ~5 fps (atlas extraction + heavy startup work), which stretches the
    // warm-up into a visible brown-sky / partial-tilemap flash.
    //
    // Override via -DCANNONBALL_WARMUP_TICKS=N to skip ahead in attract — e.g.
    // ~1800 lands the AI near the stage-1 road split (case 0) for testing
    // the hwroad_rdp prototype without watching a minute of demo.
#ifndef CANNONBALL_WARMUP_TICKS
#define CANNONBALL_WARMUP_TICKS 8
#endif
// CANNONBALL_LOG_PROFILE: dump per-frame timing breakdown to debugf on every
// FPS dip below 30 (post-warmup). Set to 0 to silence the USB log during
// extended play sessions. Cost is one display_get_fps() + a branch per frame.
#define CANNONBALL_LOG_PROFILE 1
    for (int i = 0; i < CANNONBALL_WARMUP_TICKS; i++)
        tick_engine();

    // EMA smoothing for the n64_profile counters so the dip log doesn't
    // strobe on single-frame spikes.
    auto smooth = [](uint32_t& acc, uint64_t sample)
    {
        acc = (uint32_t)((acc * 7 + sample) >> 3);
    };

    // 60Hz NTSC vsync interval is ~16683 us. Anything past one interval +
    // jitter slack means we missed a vblank — i.e. dropped a frame. Keeping
    // the threshold a bit above the nominal interval avoids false positives
    // from get_ticks_us granularity and the loop's own measurement overhead.
    constexpr uint32_t FRAME_BUDGET_US = 17000;

    while (state != STATE_QUIT)
    {
        uint64_t t0 = get_ticks_us();
        tick_engine();
        uint64_t t1 = get_ticks_us();
        video.prepare_frame();
        uint64_t t2 = get_ticks_us();
        video.render_frame();
        uint64_t t3 = get_ticks_us();
        audio.tick();
        uint64_t t4 = get_ticks_us();

        smooth(n64_profile::tick_us,    t1 - t0);
        smooth(n64_profile::prepare_us, t2 - t1);
        smooth(n64_profile::render_us,  t3 - t2);
        smooth(n64_profile::audio_us,   t4 - t3);

        // Un-smoothed per-frame total — drop detection works on raw values so
        // a single missed vblank isn't averaged away. EMAs are good for level
        // tracking but blind to discrete events like vsync misses.
        const uint32_t frame_total_us = (uint32_t)(t4 - t0);
        if (frame_total_us > FRAME_BUDGET_US) n64_profile::dropped_frames++;
        if (frame_total_us > n64_profile::max_total_us)
            n64_profile::max_total_us = frame_total_us;
        n64_profile::window_frames++;

        // Outlier capture: log the raw per-pass breakdown of any frame whose
        // total wall exceeds the OUTLIER_THRESHOLD_US. The dip log shows EMAs
        // which dilute single-frame spikes — the actual cause of a 50 ms frame
        // gets smoothed away. This logs *that* frame as it happened.
        //
        // Rate-limited to OUTLIER_LOG_MIN_GAP_US between emissions so a
        // sustained 22 fps dip can't flood the USB log; the rare bad frame
        // still gets through but a stuck-at-22 stretch only emits once a
        // second. Floor on snapshot delta uses the raw per-pass counters
        // (raw_sub_us, raw_aud_*_us, raw_wait_us) updated in finalize_frame /
        // audio.tick — *not* the EMAs.
        constexpr uint32_t OUTLIER_THRESHOLD_US  = 40000;
        constexpr uint64_t OUTLIER_LOG_MIN_GAP_US = 1000000;
        if (frame_total_us > OUTLIER_THRESHOLD_US)
        {
            static uint64_t last_outlier_us = 0;
            if (t4 - last_outlier_us >= OUTLIER_LOG_MIN_GAP_US)
            {
                last_outlier_us = t4;
                const uint32_t tick_us    = (uint32_t)(t1 - t0);
                const uint32_t prepare_us = (uint32_t)(t2 - t1);
                const uint32_t render_us  = (uint32_t)(t3 - t2);
                const uint32_t audio_us   = (uint32_t)(t4 - t3);
                debugf("OUT total=%5lu  tick=%4lu prep=%5lu rend=%5lu "
                       "aud=%5lu wait=%5lu  "
                       "rbg=%4lu tbg=%5lu rfg=%5lu spr=%5lu txt=%5lu  "
                       "z80=%4lu pcm=%4lu mix=%4lu\n",
                       (unsigned long)frame_total_us,
                       (unsigned long)tick_us,
                       (unsigned long)prepare_us,
                       (unsigned long)render_us,
                       (unsigned long)audio_us,
                       (unsigned long)n64_profile::raw_wait_us,
                       (unsigned long)n64_profile::raw_sub_us[n64_profile::SUB_ROAD_BG],
                       (unsigned long)n64_profile::raw_sub_us[n64_profile::SUB_TILE_BG],
                       (unsigned long)n64_profile::raw_sub_us[n64_profile::SUB_ROAD_FG],
                       (unsigned long)n64_profile::raw_sub_us[n64_profile::SUB_SPRITE],
                       (unsigned long)n64_profile::raw_sub_us[n64_profile::SUB_TEXT],
                       (unsigned long)n64_profile::raw_aud_z80_us,
                       (unsigned long)n64_profile::raw_aud_pcm_us,
                       (unsigned long)n64_profile::raw_aud_mix_us);
                // Sprite-cache state at the outlier — flagging an atlas reset
                // (ovf bumped vs the prior outlier) immediately tells us the
                // spike came from cache cold-start instead of normal load.
                debugf("    spr.cache: ext=%4lu bake=%4lu hit=%4lu ovf=%lu used=%lu\n",
                       (unsigned long)video.sprite_layer->atlas_extract_count(),
                       (unsigned long)video.sprite_layer->baked_extract_count(),
                       (unsigned long)video.sprite_layer->atlas_hit_count(),
                       (unsigned long)video.sprite_layer->atlas_overflow_count(),
                       (unsigned long)video.sprite_layer->atlas_used_bytes());
                // Per-call tbg state — if tbg is 3.4× normal but vis/uniq/
                // chunks/evicts look typical, the cost isn't in the obvious
                // counters (then suspect RDP backpressure or DMA stalls).
                // If chunks > 1 or evicts spike, the atlas/TLUT working set
                // overflowed for this scene's tile diversity.
                debugf("    tile.call: vis=%4lu uniq=%3lu chunks=%lu evicts=%lu prims=%lu p1=%lu p2=%lu  comp=%lu dma=%lu/%lu@%luus\n",
                       (unsigned long)n64_profile::tile_call_vis,
                       (unsigned long)n64_profile::tile_call_uniq_total,
                       (unsigned long)n64_profile::tile_call_chunks,
                       (unsigned long)n64_profile::tile_call_tlut_evicts,
                       (unsigned long)n64_profile::tile_call_prims,
                       (unsigned long)n64_profile::tile_call_pass1_us,
                       (unsigned long)n64_profile::tile_call_pass2_us,
                       (unsigned long)n64_profile::raw_composite_us,
                       (unsigned long)n64_profile::tile_call_dma_misses,
                       (unsigned long)n64_profile::tile_call_dma_fetches,
                       (unsigned long)n64_profile::tile_call_dma_us);
                debugf("    spr.call:  vis=%4lu prims=%lu loads=%lu tlut=%lu us=%lu\n",
                       (unsigned long)n64_profile::spr_call_vis,
                       (unsigned long)n64_profile::spr_call_prims,
                       (unsigned long)n64_profile::spr_call_loads,
                       (unsigned long)n64_profile::spr_call_tlut_uploads,
                       (unsigned long)n64_profile::spr_call_us);
            }
        }

#ifdef CANNONBALL_LOG_PROFILE
        // Emit a profile line on two triggers:
        //   * DIP   — fps under 30, sampled every 8 frames so the log captures
        //             prolonged peak-scene dips at high resolution.
        //   * PULSE — once every ~5 seconds regardless of fps. Catches
        //             regressions that hold fps near-target while collapsing
        //             headroom (the off-frame-skip case dropped wait_us to ~0
        //             without crossing the 30 fps threshold for a while).
        // display_get_fps() ramps from 0 during startup; gate >10 to ignore
        // the initial spin-up. `wait` is the time display_get() blocked on
        // vsync; subtracting it from `total` yields the active work per
        // iteration. `min_wait` and `max_total` are window aggregates of the
        // un-smoothed per-frame values — the EMA `wait_us` hides single-frame
        // backpressure events that `min_wait` catches.
        {
            static int log_n = 0;
            static int pulse_n = 0;
            float fps = display_get_fps();
            const bool dip   = (fps > 10.0f && fps < 30.0f
                                && ((++log_n & 7) == 0));
            const bool pulse = (++pulse_n >= 300);  // ~5s at 60fps
            if (dip || pulse)
            {
                pulse_n = 0;
                uint32_t total = n64_profile::tick_us + n64_profile::prepare_us
                               + n64_profile::render_us + n64_profile::audio_us;
                uint32_t active = (total > n64_profile::wait_us)
                                ? total - n64_profile::wait_us : 0;
                const char* tag = dip ? "DIP" : "PLS";
                const uint32_t mw = (n64_profile::min_wait_us == 0xFFFFFFFFu)
                                    ? 0 : n64_profile::min_wait_us;
                const uint32_t wf = n64_profile::window_frames ?
                                    n64_profile::window_frames : 1;
                debugf("%s fps=%4.1f active=%5lu total=%5lu  "
                       "tick=%4lu prep=%5lu rend=%5lu aud=%5lu wait=%5lu\n",
                       tag, fps, (unsigned long)active, (unsigned long)total,
                       (unsigned long)n64_profile::tick_us,
                       (unsigned long)n64_profile::prepare_us,
                       (unsigned long)n64_profile::render_us,
                       (unsigned long)n64_profile::audio_us,
                       (unsigned long)n64_profile::wait_us);
                debugf("    rbg=%4lu tbg=%5lu rfg=%5lu spr=%5lu txt=%5lu  "
                       "z80=%4lu pcm=%4lu mix=%4lu\n",
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_ROAD_BG],
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_TILE_BG],
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_ROAD_FG],
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_SPRITE],
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_TEXT],
                       (unsigned long)n64_profile::aud_z80_us,
                       (unsigned long)n64_profile::aud_pcm_us,
                       (unsigned long)n64_profile::aud_mix_us);
                // Health window: dropped/window frame ratio, min_wait floor
                // (low while fps holds = rdpq backpressure), max raw total.
                // Cache deltas — high spr.ext or tile.upl rate = working-set
                // thrash; spr.ovf going non-zero means the atlas bump-pool hit
                // capacity and reset (cold restart of the entire cache).
                const uint32_t cur_spr_ext = video.sprite_layer
                                             ->atlas_extract_count();
                const uint32_t cur_spr_hit = video.sprite_layer
                                             ->atlas_hit_count();
                const uint32_t cur_spr_ovf = video.sprite_layer
                                             ->atlas_overflow_count();
                const uint32_t cur_tile_upl = n64_profile::tile_tlut_uploads;
                const uint32_t cur_text_upl = n64_profile::text_tlut_uploads;
                debugf("    drop=%3lu/%3lu min_wait=%5lu max_total=%5lu  "
                       "spr.ext=%4lu hit=%5lu ovf=%2lu  tile.upl=%4lu txt.upl=%3lu\n",
                       (unsigned long)n64_profile::dropped_frames,
                       (unsigned long)wf,
                       (unsigned long)mw,
                       (unsigned long)n64_profile::max_total_us,
                       (unsigned long)(cur_spr_ext - n64_profile::snap_spr_extracts),
                       (unsigned long)(cur_spr_hit - n64_profile::snap_spr_hits),
                       (unsigned long)(cur_spr_ovf - n64_profile::snap_spr_overflows),
                       (unsigned long)(cur_tile_upl - n64_profile::snap_tile_tlut_uploads),
                       (unsigned long)(cur_text_upl - n64_profile::snap_text_tlut_uploads));
                // Last-call tile.call values — baseline reference when fps is
                // healthy. Compare against the OUT log to see how vis/uniq/
                // chunks/evicts move on slow frames.
                static uint32_t snap_comp_skipped = 0;
                const uint32_t cur_comp_skipped = n64_profile::composite_skipped_frames;
                debugf("    tile.call: vis=%4lu uniq=%3lu chunks=%lu evicts=%lu prims=%lu p1=%lu p2=%lu  comp=%lu skip=%lu/%lu dma=%lu/%lu@%luus\n",
                       (unsigned long)n64_profile::tile_call_vis,
                       (unsigned long)n64_profile::tile_call_uniq_total,
                       (unsigned long)n64_profile::tile_call_chunks,
                       (unsigned long)n64_profile::tile_call_tlut_evicts,
                       (unsigned long)n64_profile::tile_call_prims,
                       (unsigned long)n64_profile::tile_call_pass1_us,
                       (unsigned long)n64_profile::tile_call_pass2_us,
                       (unsigned long)n64_profile::composite_us,
                       (unsigned long)(cur_comp_skipped - snap_comp_skipped),
                       (unsigned long)wf,
                       (unsigned long)n64_profile::tile_call_dma_misses,
                       (unsigned long)n64_profile::tile_call_dma_fetches,
                       (unsigned long)n64_profile::tile_call_dma_us);
                debugf("    spr.call:  vis=%4lu prims=%lu loads=%lu tlut=%lu us=%lu\n",
                       (unsigned long)n64_profile::spr_call_vis,
                       (unsigned long)n64_profile::spr_call_prims,
                       (unsigned long)n64_profile::spr_call_loads,
                       (unsigned long)n64_profile::spr_call_tlut_uploads,
                       (unsigned long)n64_profile::spr_call_us);
                // Reset window aggregates after each emit so the next line
                // describes the next window, not the run-to-date.
                n64_profile::dropped_frames = 0;
                n64_profile::min_wait_us    = 0xFFFFFFFF;
                n64_profile::max_total_us   = 0;
                n64_profile::window_frames  = 0;
                n64_profile::snap_spr_extracts      = cur_spr_ext;
                n64_profile::snap_spr_hits          = cur_spr_hit;
                n64_profile::snap_spr_overflows     = cur_spr_ovf;
                n64_profile::snap_tile_tlut_uploads = cur_tile_upl;
                n64_profile::snap_text_tlut_uploads = cur_text_upl;
                snap_comp_skipped                   = cur_comp_skipped;
            }
        }
#endif
    }

    // N64 can't actually quit — loop forever.
    while (1) { /* halt */ }
    return 0;
}
