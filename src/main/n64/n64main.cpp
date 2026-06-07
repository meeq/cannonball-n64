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

    config.load();
    platform_overrides();

    if (!roms.load_revb_roms(config.sound.fix_samples))
    {
        debugf("ROM load failed — DFS payload likely missing.\n");
        while (1) { /* halt */ }
    }

    if (!omusic.load_widescreen_map(config.data.res_path))
        debugf("Widescreen tilemaps not loaded\n");

    // Bring the DAC up before set_fps() — config.set_fps() calls
    // osoundint.init() (which sets the chip emulators' output rate) and then
    // cycles audio.stop_audio()/start_audio() around it. With the DAC
    // already initialised, those bounces are just flag toggles.
    audio.init();

    config.set_fps(config.video.fps);

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
#define CANNONBALL_LOG_PROFILE 1     // TEMP — dump profile to debugf
    for (int i = 0; i < CANNONBALL_WARMUP_TICKS; i++)
        tick_engine();

    // EMA smoothing for on-screen profile counters so they don't strobe.
    auto smooth = [](uint32_t& acc, uint64_t sample)
    {
        acc = (uint32_t)((acc * 7 + sample) >> 3);
    };

    while (state != STATE_QUIT)
    {
        uint64_t t0 = get_ticks_us();
        tick_engine();
        uint64_t t1 = get_ticks_us();

        // The rasterizers (hwroad/hwtiles/hwsprites) are by far the most
        // expensive CPU work each frame. With config.video.fps = 1 tick_frame
        // alternates 1/0, and engine state only advances on tick frames — so
        // the scratch surface is bit-identical on the off-frames. Skip the
        // CPU prepare pass in that case; render_frame() still runs every
        // loop so display_get() continues to pace us.
        if (tick_frame)
            video.prepare_frame();
        uint64_t t2 = get_ticks_us();

        video.render_frame();
        uint64_t t3 = get_ticks_us();

        audio.tick();
        uint64_t t4 = get_ticks_us();

        smooth(n64_profile::tick_us,    t1 - t0);
        if (tick_frame) smooth(n64_profile::prepare_us, t2 - t1);
        smooth(n64_profile::render_us,  t3 - t2);
        smooth(n64_profile::audio_us,   t4 - t3);

#ifdef CANNONBALL_LOG_PROFILE
        {
            static int log_n = 0;
            if ((++log_n % 60) == 0)
                debugf("prof fps=%4.1f ras=%5lu wait=%5lu "
                       "rbg=%4lu tbg=%5lu tfg=%5lu rfg=%5lu spr=%5lu txt=%5lu\n",
                       display_get_fps(),
                       (unsigned long)n64_profile::prepare_us,
                       (unsigned long)n64_profile::wait_us,
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_ROAD_BG],
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_TILE_BG],
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_TILE_FG],
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_ROAD_FG],
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_SPRITE],
                       (unsigned long)n64_profile::sub_us[n64_profile::SUB_TEXT]);
        }
#endif
    }

    // N64 can't actually quit — loop forever.
    while (1) { /* halt */ }
    return 0;
}
