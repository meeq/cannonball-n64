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
#include "splash.hpp"
#include "boot_menu.hpp"
#include "rendersurface.hpp"
#include "hwroad_rsp.hpp"
#include "hwroad_rdp.hpp"
#include "hwroad_rdp_rsp.hpp"
#include "../hwvideo/hwroad.hpp"
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
#include "../engine/oroad.hpp"
#include "../engine/ohud.hpp"
#include "../engine/ostats.hpp"
#include "../engine/otiles.hpp"
#include "../engine/audio/osoundint.hpp"
#include "../engine/audio/commands.hpp"

#include "../frontend/config.hpp"
#include "../frontend/ttrial.hpp"

using namespace cannonball;

int    cannonball::state       = STATE_BOOT;
double cannonball::frame_ms    = 0;
int    cannonball::frame       = 0;
bool   cannonball::tick_frame  = true;
int    cannonball::fps_counter = 0;

Audio  cannonball::audio;
Input  input;
bool   pause_engine = false;
static TTrial g_ttrial(config.ttrial.best_times);

// Set to 1 to log heap stats at key boot milestones (post-ROM load,
// post-audio init). Useful for diagnosing OOM in the 4 MiB build; off in
// shipping builds so the ISViewer log isn't cluttered.
#define CANNONBALL_LOG_HEAP 0

// CANNONBALL_LOG_PROFILE: dump per-frame timing breakdown to debugf on every
// FPS dip below 30 (post-warmup). Set to 0 to silence the USB log during
// extended play sessions. Cost is one display_get_fps() + a branch per frame.
#define CANNONBALL_LOG_PROFILE 1

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
#define CANNONBALL_WARMUP_TICKS 0
#endif

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

        // Engine timing: render runs as fast as the work allows (30-60 fps
        // depending on scene); engine logic is decoupled via wall-clock
        // accumulator in tick_engine() and ticks at exactly 30 Hz. video.fps=1
        // keeps cannonball's tick_fps at 30 while letting the background
        // scroll interpolate on off-tick render frames (outrun.tick(false)).
        config.video.fps = 1;

        // Phase 4a audio: CPU-mixed YM2151 + SegaPCM → libdragon audio_push.
        // 22050 Hz keeps the chip emulators' per-frame cost manageable at the
        // 30 fps engine target; raise once budget permits.
        config.sound.enabled = 1;
        config.sound.rate    = 22050;

        // Add a silent "RADIO OFF" slot to the music-select carousel so
        // players who'd rather drive without a soundtrack can pick it like
        // any other track. The default cursor sits at index 1 (Passing
        // Breeze) and RIGHT cycles 1 → 2 → 3 → 0; the arcade-faithful
        // sequence from that start is Breeze → Splash → Magical, so we
        // lay out the array as [Radio Off, Breeze, Splash, Magical] —
        // moving Magical to the end keeps that cycle and parks Radio Off
        // as the last track visited before wrapping back to Breeze.
        music_t magical = config.sound.music[0];
        config.sound.music.erase(config.sound.music.begin());
        music_t radio_off;
        radio_off.title = "RADIO OFF";
        radio_off.type  = music_t::IS_NONE;
        config.sound.music.insert(config.sound.music.begin(), radio_off);
        config.sound.music.push_back(magical);
    }

    // -----------------------------------------------------------------------
    // In-game pause overlay
    //
    // Driven by Input::START while STATE_GAME is active and outrun.game_state
    // is in the driving range (GS_START1..GS_BONUS). Three options:
    //   Continue → resume engine + re-queue the active music track
    //   Retry    → STATE_INIT_GAME (outrun.init restarts the chosen mode)
    //   Quit     → STATE_REENTER_BOOT_MENU (existing audio-shutdown path)
    //
    // Engine is held frozen by skipping outrun.tick — every render frame
    // repaints the same scene. We blit the menu onto the engine's text RAM
    // and clear it on transition so the engine's next tick rewrites the HUD
    // cleanly.
    enum { PAUSE_CONTINUE = 0, PAUSE_RETRY = 1, PAUSE_QUIT = 2, PAUSE_COUNT = 3 };
    int pause_cursor = 0;

    // Visible screen is 40 cols × 28 rows (320×224). The text-RAM grid is
    // 64 cols wide for hardware-tilemap reasons, but anything past col 39 is
    // off-screen. Centering math runs against the visible 40.
    // Title uses the big 8x16 font (blit_text_big auto-centers on a 40-col
    // row). Options use the small font on rows 12/14/16, leaving a clean gap
    // below the title (which occupies rows 9-10).
    constexpr uint8_t  PAUSE_TITLE_ROW = 9;
    constexpr uint16_t PAUSE_OPT_COL   = 15;  // "; CONTINUE" fits 15..24
    constexpr uint16_t PAUSE_OPT_W     = 12;  // wipe width on option rows

    void blit_pause_overlay(int cursor)
    {
        ohud.blit_text_big(PAUSE_TITLE_ROW, "PAUSED");

        // OutRun's small HUD font has direction-marker glyphs at the standard
        // ASCII punctuation slots — the shaft extends one way, the tip points
        // the other. 0x3B (';') is right-pointing (tail on left); 0x3C ('<')
        // is its left-pointing mirror; 0x3E ('>') is up-pointing.
        static const char* labels[PAUSE_COUNT] = { "CONTINUE", "RETRY", "QUIT" };
        for (int i = 0; i < PAUSE_COUNT; ++i)
        {
            const uint16_t row = PAUSE_TITLE_ROW + 3 + i * 2;
            const uint16_t pal = (i == cursor) ? OHud::GREEN : OHud::GREY;
            ohud.blit_text_new(PAUSE_OPT_COL,     row,
                               (i == cursor) ? "; " : "  ");
            ohud.blit_text_new(PAUSE_OPT_COL + 2, row, labels[i], pal);
        }
    }

    // Wipe only the cells the overlay touched. video.clear_text_ram() would
    // also remove HUD labels (TIME/SCORE/LAP), which the engine only blits on
    // game-state init — they'd never come back on resume.
    void clear_pause_overlay()
    {
        // blit_text_big with an empty string still runs its full-row clear,
        // wiping both halves of the big text (rows 9 and 10).
        ohud.blit_text_big(PAUSE_TITLE_ROW, "");
        static const char blank[PAUSE_OPT_W + 1] = "            ";
        for (int i = 0; i < PAUSE_COUNT; ++i)
        {
            const uint16_t row = PAUSE_TITLE_ROW + 3 + i * 2;
            ohud.blit_text_new(PAUSE_OPT_COL, row, blank);
        }
    }

    void tick_engine()
    {
        frame++;

        // Wall-clock decouple: engine ticks at exactly 30 Hz of real time
        // regardless of render fps. The original frame-skip path (frame & 1
        // when config.fps == 60) assumed the loop was vsync-locked at 60 Hz;
        // when render dips to 30 the engine would drop to 15 Hz (half-speed
        // gameplay), when it floats back to 60 the engine pops to 30 (feels
        // like fast-forward). Accumulator-driven scheduling makes engine
        // speed independent of render rate. State handlers below still
        // override tick_frame=true for warmup transitions; that's fine, the
        // accumulator phase carries through and resyncs on next steady-state
        // iteration. Long stalls (boot menu, blocking loads) clamp the
        // accumulator to avoid catch-up bursts on resume.
        {
            static uint64_t last_us = 0;
            static uint64_t accum_us = 0;
            constexpr uint64_t TICK_US = 1000000 / 30;  // 33333
            uint64_t now_us = get_ticks_us();
            if (last_us == 0) { last_us = now_us; accum_us = TICK_US; }
            accum_us += (now_us - last_us);
            last_us = now_us;
            if (accum_us > TICK_US * 4) accum_us = TICK_US;
            tick_frame = (accum_us >= TICK_US);
            if (tick_frame) accum_us -= TICK_US;
        }

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

                    // START brings up the in-game pause menu, but only during
                    // actual driving (GS_START1 = countdown through GS_BONUS).
                    // GS_MAP (course map between stages) and GS_GAMEOVER have
                    // their own START semantics in the engine; don't poach.
                    {
                        const int gs = outrun.game_state;
                        const bool driving = (gs >= GS_START1 && gs <= GS_BONUS);
                        if (driving && input.has_pressed(Input::START))
                        {
                            // Save mixer state BEFORE FM_RESET — that command
                            // stops the music channel and resets the FM regs
                            // as a side effect via the wav64 intercept and
                            // OSound, but we want to keep the SegaPCM voice
                            // register state so reconcile_pcm can re-trigger
                            // engine rev / traffic noise on resume.
                            cannonball::audio.pause_audio();
                            osoundint.queue_sound(sound::FM_RESET);
                            cannonball::audio.clear_wav();
                            pause_cursor = PAUSE_CONTINUE;
                            blit_pause_overlay(pause_cursor);
                            input.frame_done();
                            state = STATE_PAUSED;
                            break;
                        }
                    }

                    // C-Left / C-Right cycle the current music track during
                    // gameplay. Gated on game_state being on the in-game side
                    // (countdown through course map) — engine handles attract /
                    // music select / best-outrunners with their own input maps.
                    // Setting auto_cycle_disabled latches the player out of the
                    // every-5-stages auto-DJ for the rest of the run, so a
                    // mid-stage-4 pick isn't clobbered seconds later by the
                    // stage-5 auto-cycle. omusic.enable() (next music-select
                    // entry) re-arms it.
                    {
                        const int gs = outrun.game_state;
                        const bool in_game =
                            (gs >= GS_INIT_GAME && gs <= GS_MAP);
                        if (in_game)
                        {
                            if (input.has_pressed(Input::MUSIC_NEXT))
                            {
                                omusic.cycle_music();
                                omusic.auto_cycle_disabled = true;
                            }
                            else if (input.has_pressed(Input::MUSIC_PREV))
                            {
                                omusic.cycle_music_prev();
                                omusic.auto_cycle_disabled = true;
                            }
                            // Drives the title-overlay countdown regardless of
                            // whether C was pressed this tick — the engine's
                            // stage-5/10 auto-cycle also arms it via
                            // cycle_music, so the title gets shown either way.
                            omusic.tick_track_overlay();
                        }
                    }

                    // B = "back" while the engine is in attract or music
                    // select. Once gameplay starts (GS_INIT_GAME onward) B
                    // reverts to its arcade role (BRAKE), so the predicate
                    // intentionally excludes everything past GS_MUSIC.
                    if (input.has_pressed(Input::BRAKE))
                    {
                        const int gs = outrun.game_state;
                        const bool in_attract =
                            (gs == GS_INIT      || gs == GS_ATTRACT
                          || gs == GS_INIT_BEST1 || gs == GS_BEST1
                          || gs == GS_INIT_LOGO  || gs == GS_LOGO);
                        const bool in_music   =
                            (gs == GS_INIT_MUSIC || gs == GS_MUSIC);
                        if (in_attract)
                        {
                            // Attract loop → boot menu.
                            osoundint.queue_sound(sound::FM_RESET);
                            cannonball::audio.clear_wav();
                            state = STATE_REENTER_BOOT_MENU;
                        }
                        else if (in_music)
                        {
                            osoundint.queue_sound(sound::BEEP2);
                            osoundint.queue_sound(sound::FM_RESET);
                            cannonball::audio.clear_wav();
                            if (outrun.cannonball_mode == Outrun::MODE_TTRIAL)
                            {
                                // TT music select → stage selector.
                                state = STATE_INIT_TTRIAL_SELECT;
                            }
                            else
                            {
                                // Arcade/continuous music select → attract.
                                // Re-run outrun.init(): same path the desktop
                                // frontend takes between game sessions. It
                                // calls select_course, clear_text_ram, then
                                // boot() → oinitengine.init() → otiles.
                                // reset_tiles_pal which queues TILEMAP_CLEAR
                                // so the next render tick wipes tile RAM and
                                // repopulates it from rom0. Drops credits +
                                // resets jump table along the way so attract
                                // comes back as a fresh boot, not patched up
                                // around music-select leftovers.
                                ostats.credits = 0;
                                outrun.init();
                                // HACK: same 4-tick warmup STATE_INIT_GAME
                                // uses. outrun.init queues a tilemap clear
                                // that doesn't land until vint, and the sky
                                // palette is mid-cycle/fade — without burning
                                // a few engine ticks here, the first visible
                                // frame shows the music-select sky over the
                                // gameplay-attract scene. Magic 4 isn't
                                // derived, it's copied from STATE_INIT_GAME.
                                // Outstanding: see memory note
                                // engine-init-warmup-hack for what's been
                                // ruled out (tilemap, sky palette) and the
                                // open leads (rgb[] desync, stale roadram).
                                for (int i = 0; i < 4; ++i)
                                    outrun.tick(true);
                            }
                        }
                    }
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

            case STATE_PAUSED:
                // Engine is frozen — outrun.tick is not called, so the same
                // scene paints each render frame. We just handle menu input
                // and re-blit the overlay (cheap, ~50 cells).
                if (tick_frame)
                {
                    bool nav   = false;
                    bool back  = input.has_pressed(Input::BRAKE);
                    bool sel   = input.has_pressed(Input::START)
                              || input.has_pressed(Input::ACCEL);

                    if (input.has_pressed(Input::UP))
                    {
                        pause_cursor = (pause_cursor + PAUSE_COUNT - 1) % PAUSE_COUNT;
                        nav = true;
                    }
                    else if (input.has_pressed(Input::DOWN))
                    {
                        pause_cursor = (pause_cursor + 1) % PAUSE_COUNT;
                        nav = true;
                    }

                    if (nav)  osoundint.queue_sound(sound::BEEP1);

                    // B = quick resume — matches arcade muscle memory and
                    // mirrors what BRAKE does on the title screens.
                    const int choice = back ? PAUSE_CONTINUE
                                     : (sel ? pause_cursor : -1);

                    if (choice >= 0)
                    {
                        osoundint.queue_sound(choice == PAUSE_CONTINUE
                                                ? sound::BEEP2
                                                : sound::COIN_IN);
                        // Targeted clear: only the cells the overlay wrote
                        // need wiping. A full clear_text_ram would also drop
                        // HUD labels (TIME/SCORE/LAP), which the engine only
                        // blits at game-state init — they wouldn't come back
                        // on resume. Retry/Quit transition through states that
                        // re-init the HUD so the targeted clear is harmless
                        // there too.
                        clear_pause_overlay();

                        switch (choice)
                        {
                            case PAUSE_CONTINUE:
                                // Replay the same wav64 and seek to the
                                // sample position we captured at pause entry.
                                // SegaPCM voices come back through
                                // reconcile_pcm's edge detection on the next
                                // engine tick.
                                cannonball::audio.resume_audio();
                                state = STATE_GAME;
                                break;
                            case PAUSE_RETRY:
                                // STATE_INIT_GAME would put us through attract
                                // because outrun.init()→boot() sets game_state =
                                // GS_INIT. Skip that and jump straight to the
                                // engine's countdown setup: GS_INIT_GAME inits
                                // jump table + engine, queues GET_READY voice,
                                // plays music, draws HUD, then falls through to
                                // GS_START1. has_booted+credits are normally
                                // primed by init_attract; we mirror those here
                                // since we're bypassing it. Music selection,
                                // auto_cycle_disabled, region etc. carry over
                                // — matches "back to starting line" semantics.
                                cannonball::audio.clear_wav();
                                outrun.init();
                                osoundint.has_booted = true;
                                ostats.credits       = 1;
                                outrun.game_state    = GS_INIT_GAME;
                                pause_engine         = false;
                                for (int i = 0; i < 4; ++i)
                                    outrun.tick(true);
                                state = STATE_GAME;
                                break;
                            case PAUSE_QUIT:
                                cannonball::audio.clear_wav();
                                state = STATE_REENTER_BOOT_MENU;
                                break;
                        }
                    }
                    else
                    {
                        blit_pause_overlay(pause_cursor);
                    }

                    input.frame_done();
                }
                break;

            case STATE_INIT_GAME:
                tick_frame   = true;
                pause_engine = false;
                // Continuous mode reads otraffic.cpp's per-stage density from
                // outrun.custom_traffic (set_max_traffic falls through to it
                // for any non-ORIGINAL mode). The desktop sets this in
                // frontend/menu.cpp before launching CONT, but menu.cpp isn't
                // in the N64 build — so without this line, dip_traffic gets
                // ignored and CONT plays with whatever last touched it
                // (0 = silent road on cold boot).
                if (outrun.cannonball_mode == Outrun::MODE_CONT)
                    outrun.custom_traffic = config.cont_traffic;
                outrun.init();
                // Advance the engine through GS_INIT → GS_INIT_MUSIC →
                // GS_MUSIC (and a few GS_MUSIC ticks) without rendering, so
                // the first visible frame is the music-select scene with
                // correct tilemap/palette. Without this, the previous screen's
                // leftover tile RAM (sand-brown course-map background from
                // TTrial::init, or boot menu pixels on a fresh start) renders
                // through the music-select tilemap for ~5 ticks while
                // omusic.enable() + opalette fade resolve. Same fix benefits
                // both the boot → game and TT-select → game transitions.
                for (int i = 0; i < 4; ++i)
                    outrun.tick(true);
                state = STATE_GAME;
                break;

            case STATE_INIT_TTRIAL_SELECT:
                // Force the same iteration's STATE_TTRIAL_SELECT fall-through
                // to call g_ttrial.tick (which runs INIT_COURSEMAP →
                // omap.init → write_tilemap_hw). At 60Hz display / 30Hz
                // engine we flip tick_frame every other frame; if we
                // entered here on an off-tick, the first render would paint
                // with text_ram still zeroed (no tile coverage), exposing
                // whatever the previous mode left in the cycled framebuffer.
                tick_frame = true;
                // Engine subsystems the TT screen depends on but doesn't set
                // up itself. The desktop Menu::init prelude does these for
                // free; on N64 we skip Menu entirely so we cover them here.
                // select_course populates outrun.adr.* from the active region's
                // constant family — must run before setup_palette_hud, which
                // reads pal_hud_src out of adr. On a first entry adr is BSS-
                // zero, so without this the palette is sourced from rom0[0]
                // and the lap icon / "STEER TO SELECT TRACK" come out
                // miscolored until the next round.
                outrun.select_course(config.engine.jap != 0,
                                     config.engine.prototype != 0);
                otiles.setup_palette_hud();
                osoundint.has_booted = true;
                osoundint.init();
                cannonball::audio.clear_wav();
                // Wipe leftover engine + hardware state from a prior
                // arcade/continuous session before painting the course-map
                // screen. The desktop's Menu::init covers this in one place;
                // on N64 we mirror its individual resets here. Without these,
                // hwroad/hwsprites/hwtiles all keep replaying the attract
                // mode's last frame underneath the course map (sky bands,
                // cloud sprites, stage-1 tiles). On a fresh boot these are
                // all no-ops because the buffers are already BSS-zero.
                video.sprite_layer->reset();
                hwroad.reset();
                video.clear_tile_ram();
                video.clear_text_ram();
                oroad.init();
                // OTiles::scroll_tilemaps runs (and writes a non-zero V-
                // scroll from oroad.horizon_y_bak) for several game_state
                // values including GS_ATTRACT. After arcade attract that's
                // the engine's last-seen state; GS_INIT short-circuits the
                // function so the tilemap stays at v=0 and fully covers
                // the road background sand. Fresh-boot is safe because the
                // BSS-zero default (GS_INIT) already trips the early-out.
                outrun.game_state = GS_INIT;
                g_ttrial.init();
                state = STATE_TTRIAL_SELECT;
                // Suppress carried-over presses so a button held since the
                // boot menu (typically A confirming "TIME TRIALS") doesn't
                // immediately confirm the default stage in TTrial::tick.
                //   * Input::frame_done syncs keys_old←keys so has_pressed
                //     reports false until the player releases + re-presses.
                //   * OInputs::reset_press_state rearms the analog-accel
                //     debounce so is_analog_select doesn't fire on its
                //     first call (it counts down from delay3, which init()
                //     leaves at 0). Player has DELAY_RESET ticks to release
                //     A before the held-hold path also triggers.
                input.frame_done();
                oinputs.reset_press_state();
                // fall through
            case STATE_TTRIAL_SELECT:
            {
                // Drive TTrial::tick at the engine's logic rate (30Hz),
                // matching what the desktop frontend does. Calling it on
                // every render frame at 60Hz doubles the steering / input
                // response speed.
                if (tick_frame)
                {
                    // Menu SFX: queue before TTrial::tick consumes the press.
                    // Engine audio is already up so these route through the
                    // wav64 SFX channel without needing the boot menu's mixer.
                    const bool nav  = input.has_pressed(Input::LEFT)
                                   || input.has_pressed(Input::RIGHT);
                    const bool go   = input.has_pressed(Input::START)
                                   || input.has_pressed(Input::ACCEL);
                    const bool back = input.has_pressed(Input::BRAKE);
                    if (nav)  osoundint.queue_sound(sound::BEEP1);
                    if (go)   osoundint.queue_sound(sound::COIN_IN);
                    if (back) osoundint.queue_sound(sound::BEEP2);

                    if (back)
                    {
                        // Hand control back to the N64 boot menu so the
                        // player can pick a different mode. Audio gets torn
                        // down + brought back up around the re-entry — the
                        // boot menu owns audio init while it runs.
                        state = STATE_REENTER_BOOT_MENU;
                    }
                    else
                    {
                        int r = g_ttrial.tick();
                        if (r == TTrial::INIT_GAME)
                        {
                            osoundint.queue_clear();
                            state = STATE_INIT_GAME;
                        }
                    }
                    input.frame_done();
                }
                break;
            }

            case STATE_REENTER_BOOT_MENU:
                cannonball::audio.shutdown();
                n64::boot_menu::run();
                cannonball::audio.init();
                cannonball::audio.prime_mixer_buffers();
                // The boot menu can flip REGION between visits; rom0/rom1 hold
                // one region's chip data in place, so resync the resident set
                // to match the new config before the engine reads it. Fall
                // back to World if the Japanese chips aren't in DFS.
                if (!roms.ensure_region(config.engine.jap != 0))
                {
                    debugf("Region reload failed — falling back to World.\n");
                    config.engine.jap = 0;
                    roms.ensure_region(false);
                }
                // boot_menu::run clears the framebuffers but leaves engine
                // state (hwroad/hwsprites/hwtiles/text_ram) holding arcade
                // attract leftovers. STATE_INIT_TTRIAL_SELECT /
                // STATE_INIT_GAME run on the NEXT iteration, so the
                // prepare/render pair that runs between them and this case
                // would otherwise paint those leftovers into the freshly
                // black framebuffer for one frame. Wipe the hardware
                // buffers here so that intermediate paint is a no-op.
                video.sprite_layer->reset();
                hwroad.reset();
                video.clear_tile_ram();
                video.clear_text_ram();
                state = (outrun.cannonball_mode == Outrun::MODE_TTRIAL)
                          ? STATE_INIT_TTRIAL_SELECT
                          : STATE_INIT_GAME;
                break;

            case STATE_MENU:
            case STATE_INIT_MENU:
                // No in-engine menu on N64 — STATE_INIT_MENU is the engine's
                // "we're done with this game, what next?" hand-off. Persist
                // a new TT best to EEPROM (via TTrial::update_best_time so
                // the right level index is used), then route MODE_TTRIAL
                // back to the course-map select screen instead of restarting
                // the same stage through attract.
                if (outrun.ttrial.new_high_score)
                {
                    outrun.ttrial.new_high_score = false;
                    g_ttrial.update_best_time();
                }
                // Silence the in-game music before re-entering the stage
                // selector. Without FM_RESET the selected track keeps
                // looping under the course-map screen until the next stage
                // launches.
                osoundint.queue_sound(sound::FM_RESET);
                cannonball::audio.clear_wav();
                state = (outrun.cannonball_mode == Outrun::MODE_TTRIAL)
                          ? STATE_INIT_TTRIAL_SELECT
                          : STATE_INIT_GAME;
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
    // Overlay persisted user choices over the platform defaults *before* any
    // subsystem reads them (audio.init keys off sound.rate, roms.load keys
    // off engine.jap). No-op on first boot — the menu below will write a
    // record if the user confirms one.
    n64::boot_menu::apply_saved_settings();

    // Bring up the framebuffer + rdpq first because both the SEGA splash
    // and the boot menu render through it. Atlas + every other heap
    // consumer waits until after the menu so the menu's sprite allocations
    // sit at the wilderness top and dlmalloc reabsorbs them cleanly when
    // the next big single-chunk alloc (atlas_init's 256 KiB pool) extends
    // the boundary. Headroom on a 4 MiB cart is ~69 KiB after ROM load —
    // any small free-list scatter from the menu would block one of the
    // larger ROM allocations (pcm.init = 0x60000 = 384 KiB single chunk).
    video.boot_display();
#if CANNONBALL_LOG_HEAP
    {
        heap_stats_t hs; sys_get_heap_stats(&hs);
        debugf("heap post-display: used=%d free=%d\n", hs.used, hs.total - hs.used);
    }
#endif

    // Pre-boot SEGA splash — ~3.5 s palette-cycled intro animation.
    // Allocates a sprite_t for the duration and frees it before returning,
    // so the boot menu's font load lands on a clean heap.
    n64::splash::run();

    // Pre-engine title + options menu. The player picks ARCADE /
    // CONTINUOUS / TIME TRIALS (with an inline laps editor) or steps into
    // OPTIONS to adjust persisted settings. On confirm the choice sets
    // outrun.cannonball_mode and writes the live config back to EEPROM,
    // then the engine init pipeline runs and the chosen mode kicks off.
    n64::boot_menu::run();
#if CANNONBALL_LOG_HEAP
    {
        heap_stats_t hs; sys_get_heap_stats(&hs);
        debugf("heap post-bootmenu: used=%d free=%d\n", hs.used, hs.total - hs.used);
    }
#endif

    video.sprite_layer->atlas_init();
#if CANNONBALL_LOG_HEAP
    {
        heap_stats_t hs; sys_get_heap_stats(&hs);
        debugf("heap post-atlas: used=%d free=%d\n", hs.used, hs.total - hs.used);
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

    // Japanese ROMs overwrite rom0 / rom1 in place — only one master/slave
    // CPU ROM set is resident at a time, so this fits on baseline 4 MiB. The
    // engine code paths that depend on region-shifted offsets route through
    // outrun.adr.*, which select_course populates from the _J family of
    // constants when jap=1. If the DFS payload is missing the J chips, fall
    // back to World rom0 / rom1 (which load_revb_roms left in place) and
    // clear the flag so the engine never reads Japan offsets from a buffer
    // that still holds World bytes.
    if (config.engine.jap && !roms.load_japanese_roms())
    {
        debugf("Japanese ROMs not found — falling back to World.\n");
        config.engine.jap = 0;
        roms.load_revb_roms(config.sound.fix_samples); // restore World rom0/1
    }

#if CANNONBALL_LOG_HEAP
    {
        heap_stats_t hs; sys_get_heap_stats(&hs);
        debugf("heap post-roms: used=%d free=%d\n", hs.used, hs.total - hs.used);
    }
#endif

    if (!omusic.load_widescreen_map(config.data.res_path))
        debugf("Widescreen tilemaps not loaded\n");

    // video.init runs before audio.prime_mixer_buffers so the hwtiles tile
    // pixel cache claims a contiguous block first. mixer_ch_play allocates
    // via malloc_uncached, which fragments the free heap as 16 separate
    // ~4 KiB blocks — afterward there's enough free *bytes* for the cache
    // but no single contiguous run that fits it.
    if (!video.init(&roms, &config.video))
    {
        debugf("video.init failed\n");
        while (1) { /* halt */ }
    }
#if CANNONBALL_LOG_HEAP
    {
        heap_stats_t hs; sys_get_heap_stats(&hs);
        debugf("heap post-video: used=%d free=%d\n", hs.used, hs.total - hs.used);
    }
#endif

    // Bring up the hwroad RSP overlay. Cheap (one rspq_overlay_register +
    // a 14 KB malloc) — leave the runtime switch off so the CPU path stays
    // the default. Toggle n64::hwroad_rsp::enabled to A/B test.
    n64::hwroad_rsp::init();
    n64::hwroad_rdp::init();
    n64::hwroad_rdp_rsp::init();

    // Pre-flush the libdragon mixer's lazy per-channel sample buffers as
    // the LAST init step, so every other large alloc (video atlas/cache,
    // hwroad descriptors) has claimed its contiguous region before the
    // mixer fragments the remainder into 16 ~4 KiB blocks. Without this,
    // each SegaPCM channel malloc_uncached's its buffer on first play —
    // a 4 MiB OOM could surface mid-game the first time an unused voice
    // fires (e.g. crash SFX after a long drive). Surface any shortfall
    // here predictably instead.
    audio.prime_mixer_buffers();
#if CANNONBALL_LOG_HEAP
    {
        heap_stats_t hs; sys_get_heap_stats(&hs);
        debugf("heap post-audio-prime: used=%d free=%d\n", hs.used, hs.total - hs.used);
    }
#endif

    input.init(config.controls.pad_id,
               config.controls.keyconfig, config.controls.padconfig,
               config.controls.analog,    config.controls.axis,
               config.controls.invert,    config.controls.asettings);

    state = (outrun.cannonball_mode == Outrun::MODE_TTRIAL)
              ? STATE_INIT_TTRIAL_SELECT
              : STATE_INIT_GAME;

    // Warm up the engine by ticking before the first frame
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
                debugf("    spr.cache: ext=%4lu hit=%4lu ovf=%lu used=%lu\n",
                       (unsigned long)video.sprite_layer->atlas_extract_count(),
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
                debugf("    spr.call:  vis=%4lu prims=%lu loads=%lu tlut=%lu ovf=%lu us=%lu\n",
                       (unsigned long)n64_profile::spr_call_vis,
                       (unsigned long)n64_profile::spr_call_prims,
                       (unsigned long)n64_profile::spr_call_loads,
                       (unsigned long)n64_profile::spr_call_tlut_uploads,
                       (unsigned long)n64_profile::spr_call_ovf,
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
                debugf("    tile.call: vis=%4lu (bg=%lu fg=%lu) uniq=%3lu chunks=%lu evicts=%lu prims=%lu p1=%lu p2=%lu  comp=%lu skip=%lu/%lu dma=%lu/%lu@%luus\n",
                       (unsigned long)n64_profile::tile_call_vis,
                       (unsigned long)n64_profile::tile_call_vis_bg,
                       (unsigned long)n64_profile::tile_call_vis_fg,
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
                debugf("    spr.call:  vis=%4lu prims=%lu loads=%lu tlut=%lu ovf=%lu us=%lu\n",
                       (unsigned long)n64_profile::spr_call_vis,
                       (unsigned long)n64_profile::spr_call_prims,
                       (unsigned long)n64_profile::spr_call_loads,
                       (unsigned long)n64_profile::spr_call_tlut_uploads,
                       (unsigned long)n64_profile::spr_call_ovf,
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
