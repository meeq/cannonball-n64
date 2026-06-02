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
        debug_init_isviewer();
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
                    osoundint.tick();
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

    config.set_fps(config.video.fps);

    if (!video.init(&roms, &config.video))
    {
        debugf("video.init failed\n");
        while (1) { /* halt */ }
    }

    audio.init();

    input.init(config.controls.pad_id,
               config.controls.keyconfig, config.controls.padconfig,
               config.controls.analog,    config.controls.axis,
               config.controls.invert,    config.controls.asettings);

    state = STATE_INIT_GAME;

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
        // pixels[] is bit-identical on the off-frames. Skip re-rasterizing in
        // that case; render_frame() still runs every loop so display_get()
        // continues to pace us.
        if (tick_frame)
            video.prepare_frame();
        uint64_t t2 = get_ticks_us();

        video.render_frame();
        uint64_t t3 = get_ticks_us();

        audio.tick();

        smooth(n64_profile::tick_us,    t1 - t0);
        if (tick_frame) smooth(n64_profile::prepare_us, t2 - t1);
        smooth(n64_profile::render_us,  t3 - t2);
    }

    // N64 can't actually quit — loop forever.
    while (1) { /* halt */ }
    return 0;
}
