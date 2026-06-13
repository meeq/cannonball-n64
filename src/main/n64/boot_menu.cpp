/***************************************************************************
    N64 pre-engine title + options menu — sprite-backed implementation.

    Render path is intentionally narrow:
      * rdpq_sprite_blit + a single per-frame combiner that multiplies the
        CI4 texel RGB by a runtime prim colour — so the labels, pills and
        cursor can be re-tinted (highlight #F9E231, dim grey for OFF pills)
        without per-state palette swaps.
      * Backgrounds (title_bg, options_bg) blit through the same combiner
        with prim = white, which leaves the RGBA16 chrome untouched.
      * One caret sprite (▲) re-used for ▶ ◀ ▲ ▼ via rdpq_blitparms_t.theta.

    Heap discipline matters: the rdpq_text engine got us OOM at pcm.init the
    last time round (~25 KiB static cost), and we run on a 4 MiB cart with
    only ~69 KiB of headroom after ROM load. Sprite blitting + sprite_load
    is already linked for the engine renderer, so this path adds essentially
    no new static budget. Sprites are loaded for one screen at a time and
    freed before the next screen swap, so transient cost peaks at the larger
    of the two sets (~options, ≈210 KiB).
***************************************************************************/

#include "boot_menu.hpp"
#include "save.hpp"
#include "input.hpp"
#include "../frontend/config.hpp"
#include "../engine/outrun.hpp"

#include <libdragon.h>
#include <cstdio>
#include <cstring>
#include <cmath>

using n64save::saved_settings_v1;

namespace
{
    // ===== Layout constants =================================================

    constexpr int FB_W       = 320;
    constexpr int FB_H       = 240;

    // Title menu: 4 rows stacked flush at (36, 66). Inter-row spacing is
    // baked into the label sprites (164×22 each), so stride == label height.
    constexpr int TITLE_X        = 36;
    constexpr int TITLE_Y0       = 66;
    constexpr int TITLE_LABEL_W  = 164;
    constexpr int TITLE_LABEL_H  = 22;
    constexpr int TITLE_STRIDE   = TITLE_LABEL_H;

    // Options menu: 5 top rows. Content area x=36..284 (20 outer + 16 inner
    // margin); label 156 wide left-aligned at x=36; value 92 wide right-
    // aligned ending at x=284 (so value_x = 192). Row stride == label
    // height — vertical breathing room is part of the sprite.
    constexpr int OPT_X          = 36;
    constexpr int OPT_Y0         = 54;
    constexpr int OPT_LABEL_W    = 156;
    constexpr int OPT_VALUE_W    = 92;
    constexpr int OPT_VALUE_X    = OPT_X + OPT_LABEL_W;   // 192
    constexpr int OPT_LABEL_H    = 20;
    constexpr int OPT_STRIDE     = OPT_LABEL_H;

    // Cheats pill row: starts at (36, 193). Pills fill the 248-wide content
    // area flush (44 + 68 + 60 + 76 = 248), no gaps.
    constexpr int PILL_Y         = 193;
    constexpr int PILL_X0        = 36;
    constexpr int PILL_H         = 20;

    constexpr int CARET_DIM      = 16;
    constexpr int CARET_GAP      = 4;     // gap between row left edge and caret

    // Caret rotation (counter-clockwise radians; sprite authored as ▲).
    const float CARET_UP      = 0.0f;
    const float CARET_LEFT    = (float)(M_PI * 0.5);
    const float CARET_DOWN    = (float)M_PI;
    const float CARET_RIGHT   = (float)(M_PI * 1.5);

    // Tint colours.
    const color_t COL_WHITE     = RGBA32(0xFF, 0xFF, 0xFF, 0xFF);
    const color_t COL_HIGHLIGHT = RGBA32(0xF9, 0xE2, 0x31, 0xFF);   // user spec
    const color_t COL_PILL_OFF  = RGBA32(0x40, 0x40, 0x40, 0xFF);   // ~25 % brightness

    // Audio bringup follows the splash pattern: Audio::init() owns the
    // mixer once the engine boots, so the menu opens audio + mixer fresh
    // and tears them back down before returning. Stereo music uses ch 0+1;
    // ch 2 is the SFX bus (YM beep / coin samples, retriggered on each
    // input event — replaces any in-flight sample, which is exactly the
    // right snappiness for menu navigation).
    //
    // We allocate 4 mixer channels even though only 3 are addressed: the
    // mixer asserts on configuring the LAST channel as stereo (it would
    // need ch+1 for the right side), and wav64_play unconditionally
    // prepares for a stereo source.
    constexpr int MENU_AUDIO_RATE    = 22050;
    constexpr int MENU_AUDIO_BUFFERS = 4;
    constexpr int MENU_AUDIO_CH      = 4;
    constexpr int SFX_CH             = 2;

    wav64_t g_music;
    wav64_t g_sfx_beep1;   // navigation / value cycle
    wav64_t g_sfx_beep2;   // confirm / pill toggle / exit options
    wav64_t g_sfx_coin;    // game-mode pick (launches arcade / cont / TT)
    bool    g_music_active = false;

    // ===== Row / pill enums =================================================

    enum title_row_t
    {
        T_ARCADE = 0,
        T_CONTINUOUS,
        T_TIME_TRIALS,
        T_OPTIONS,
        T_COUNT
    };

    // REGION (jap/world toggle) is always available — Roms::load_japanese_roms
    // overwrites rom0 / rom1 in place rather than allocating a second set, so
    // the 512 KiB cost the old guard worried about is now zero.
    enum opt_row_t
    {
        O_REGION = 0,
        O_TIME,
        O_TRAFFIC,
        O_COLOR,
        O_TRANSMISSION,
        O_CHEATS,
        O_COUNT
    };

    enum pill_t
    {
        P_TIRES = 0,
        P_BUMPER,
        P_TURBO,
        P_OFFROAD,
        P_COUNT
    };

    // ===== Sprite tables ====================================================

    struct title_sprites_t
    {
        sprite_t* bg;
        sprite_t* caret;
        sprite_t* arcade;
        sprite_t* continuous;
        sprite_t* tt[5];     // tt1..tt5
        sprite_t* options;
    };

    struct options_sprites_t
    {
        sprite_t* bg;
        sprite_t* caret;
        sprite_t* labels[5];     // region, time, traffic, color, transmission
        sprite_t* world;
        sprite_t* japan;
        sprite_t* off;           // shared by TIME + TRAFFIC for freeze / disable
        sprite_t* diff[4];       // easy, normal, hard, hardest
        sprite_t* colors[5];     // red, blue, yellow, green, cyan
        sprite_t* manual;
        sprite_t* automatic;
        sprite_t* pills[P_COUNT];
    };

    title_sprites_t   g_title{};
    options_sprites_t g_options{};

    // ===== Audio ============================================================

    // Bring up audio + mixer, open music_lastwave.wav64 looping + the three
    // OutRun YM SFX. Splash has already torn down its own audio_init /
    // mixer_init before we got here.
    void start_music()
    {
        if (g_music_active) return;
        audio_init(MENU_AUDIO_RATE, MENU_AUDIO_BUFFERS);
        mixer_init(MENU_AUDIO_CH);
        wav64_open(&g_music, "rom:/audio/music_lastwave.wav64");
        wav64_set_loop(&g_music, true);
        wav64_play(&g_music, 0);
        wav64_open(&g_sfx_beep1, "rom:/audio/ym_beep1.wav64");
        wav64_open(&g_sfx_beep2, "rom:/audio/ym_beep2.wav64");
        wav64_open(&g_sfx_coin,  "rom:/audio/ym_coin_in.wav64");
        g_music_active = true;
    }

    // Drain in-flight mixer state then hand audio + mixer back to the
    // system so Audio::init() can claim them fresh.
    void stop_music()
    {
        if (!g_music_active) return;
        wav64_close(&g_music);
        wav64_close(&g_sfx_beep1);
        wav64_close(&g_sfx_beep2);
        wav64_close(&g_sfx_coin);
        mixer_close();
        audio_close();
        g_music_active = false;
    }

    // Retriggers cleanly on each call — repeated nav presses produce a
    // tight staccato instead of overlapping echoes because the SFX channel
    // always restarts from the top.
    //
    // mixer_ch_stop on both ch and ch+1 before each play handles two cases:
    //   * a stereo wav64 left ch+1 in a 'playing' state from the previous
    //     trigger — wav64_play would otherwise assert when it tries to
    //     reserve ch+1 for the right side of the new stereo source
    //     (mixer.c:486 "cannot play stereo waveform on channel N because
    //     channel N+1 is active");
    //   * a mono wav64 needs only ch, but stopping ch+1 is free.
    void play_sfx(wav64_t* s)
    {
        if (!g_music_active || !s) return;
        mixer_ch_stop(SFX_CH);
        mixer_ch_stop(SFX_CH + 1);
        wav64_play(s, SFX_CH);
    }

    // Per-frame: feed the AI ring buffers from the mixer. Without this the
    // mixer's output silently backs up and the stream stalls.
    void pump_audio()
    {
        if (!g_music_active) return;
        while (audio_can_write())
        {
            short* buf = audio_write_begin();
            mixer_poll(buf, audio_get_buffer_length());
            audio_write_end();
        }
    }

    sprite_t* load_named(const char* name)
    {
        char path[64];
        std::snprintf(path, sizeof(path), "rom:/menu/%s.sprite", name);
        return sprite_load(path);
    }

    template <typename T>
    void free_field(T*& s)
    {
        if (s) { sprite_free(s); s = nullptr; }
    }

    void load_title_sprites()
    {
        g_title.bg          = load_named("title_bg");
        g_title.caret       = load_named("caret");
        g_title.arcade      = load_named("arcade");
        g_title.continuous  = load_named("continuous");
        g_title.tt[0]       = load_named("tt1");
        g_title.tt[1]       = load_named("tt2");
        g_title.tt[2]       = load_named("tt3");
        g_title.tt[3]       = load_named("tt4");
        g_title.tt[4]       = load_named("tt5");
        g_title.options     = load_named("options");
    }

    // rdpq_detach_show is async (RDP may still be reading the last frame's
    // textures); drain before sprite_free or we pull glyph data out from
    // under in-flight commands. Same gotcha as the rdpq_font path that bit
    // us earlier — see feedback_rdpq_free_needs_drain.
    void free_title_sprites()
    {
        rspq_wait();
        free_field(g_title.bg);
        free_field(g_title.caret);
        free_field(g_title.arcade);
        free_field(g_title.continuous);
        for (auto& s : g_title.tt) free_field(s);
        free_field(g_title.options);
    }

    void load_options_sprites()
    {
        g_options.bg                       = load_named("options_bg");
        g_options.caret                    = load_named("caret");
        g_options.labels[O_REGION]         = load_named("region");
        g_options.world                    = load_named("world");
        g_options.japan                    = load_named("japan");
        g_options.labels[O_TIME]           = load_named("time");
        g_options.labels[O_TRAFFIC]        = load_named("traffic");
        g_options.labels[O_COLOR]          = load_named("color");
        g_options.labels[O_TRANSMISSION]   = load_named("transmission");
        g_options.off                      = load_named("off");
        g_options.diff[0]                  = load_named("easy");
        g_options.diff[1]                  = load_named("normal");
        g_options.diff[2]                  = load_named("hard");
        g_options.diff[3]                  = load_named("hardest");
        g_options.colors[0]                = load_named("red");
        g_options.colors[1]                = load_named("blue");
        g_options.colors[2]                = load_named("yellow");
        g_options.colors[3]                = load_named("green");
        g_options.colors[4]                = load_named("cyan");
        g_options.manual                   = load_named("manual");
        g_options.automatic                = load_named("automatic");
        g_options.pills[P_TIRES]           = load_named("tires");
        g_options.pills[P_BUMPER]          = load_named("bumper");
        g_options.pills[P_TURBO]           = load_named("turbo");
        g_options.pills[P_OFFROAD]         = load_named("offroad");
    }

    void free_options_sprites()
    {
        rspq_wait();
        free_field(g_options.bg);
        free_field(g_options.caret);
        for (auto& s : g_options.labels) free_field(s);
        free_field(g_options.world);
        free_field(g_options.japan);
        free_field(g_options.off);
        for (auto& s : g_options.diff)   free_field(s);
        for (auto& s : g_options.colors) free_field(s);
        free_field(g_options.manual);
        free_field(g_options.automatic);
        for (auto& s : g_options.pills)  free_field(s);
    }

    // ===== Render helpers ===================================================

    // Per-frame mode setup: combiner = TEX0.rgb × PRIM.rgb, alpha = TEX0.a.
    // Gives runtime tinting of every CI4 sprite while RGBA16 backgrounds
    // pass through unchanged when PRIM = white.
    void setup_render_mode()
    {
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0),
                                          (0, 0, 0, TEX0)));
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    }

    void blit_tinted(sprite_t* s, float x, float y, color_t tint)
    {
        rdpq_set_prim_color(tint);
        rdpq_sprite_blit(s, x, y, nullptr);
    }

    // Sprite hotspot = its centre, so (centre_x, centre_y) is the destination
    // pivot point. The default top-left blit can't rotate, so the caret goes
    // through this path even when theta == 0.
    void blit_caret(sprite_t* s, float centre_x, float centre_y,
                    float theta, color_t tint)
    {
        rdpq_set_prim_color(tint);
        rdpq_blitparms_t p = {};
        p.cx    = s->width  / 2;
        p.cy    = s->height / 2;
        p.theta = theta;
        rdpq_sprite_blit(s, centre_x, centre_y, &p);
    }

    // ===== EEPROM <-> live config ==========================================

    // Same projection used by the old rdpq_text menu — the on-disk record
    // schema hasn't changed. Every field is serialised so dropped UI rows
    // (Prototype / Sound / FPS / Freeze Timer / Disable Traffic / Cont
    // Traffic / TT Traffic) still round-trip through their defaults.
    void seed_record(saved_settings_v1& s)
    {
        std::memset(&s, 0, sizeof(s));
        s.version          = 1;
        s.jap              = config.engine.jap        ? 1 : 0;
        s.prototype        = config.engine.prototype  ? 1 : 0;
        s.fps_mode         = config.video.fps & 0x3;
        s.widescreen       = config.video.widescreen  ? 1 : 0;
        s.sound_on         = config.sound.enabled     ? 1 : 0;
        s.freeze_timer     = config.engine.freeze_timer    ? 1 : 0;
        s.disable_traffic  = config.engine.disable_traffic ? 1 : 0;
        s.grippy_tyres     = config.engine.grippy_tyres    ? 1 : 0;
        s.offroad          = config.engine.offroad         ? 1 : 0;
        s.bumper           = config.engine.bumper          ? 1 : 0;
        s.turbo            = config.engine.turbo           ? 1 : 0;
        s.freeplay         = config.engine.freeplay        ? 1 : 0;
        s.fix_bugs         = config.engine.fix_bugs        ? 1 : 0;
        s.new_attract      = config.engine.new_attract     ? 1 : 0;
        s.dip_time         = config.engine.dip_time    & 0x3;
        s.dip_traffic      = config.engine.dip_traffic & 0x3;
        s.cont_traffic     = config.cont_traffic       & 0x3;
        s.gear             = config.controls.gear      & 0x3;
        s.car_pal          = config.engine.car_pal     & 0x7;
        s.ttrial_laps      = (uint8_t)((config.ttrial.laps - 1) & 0x7);
        s.ttrial_traffic   = config.ttrial.traffic     & 0x3;
        s.steer_speed      = (uint8_t)(config.controls.steer_speed & 0xF);
        s.pedal_speed      = (uint8_t)(config.controls.pedal_speed & 0xF);
        s.invert_x         = config.controls.invert[0] ? 1 : 0;
        s.invert_y         = config.controls.invert[1] ? 1 : 0;
        s.invert_z         = config.controls.invert[2] ? 1 : 0;
        s.music_timer      = (uint8_t)config.sound.music_timer;
        float r = config.controls.rumble;
        if (r < 0.0f) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        s.rumble           = (uint8_t)(r * 255.0f);
        for (int i = 0; i < 12; ++i)
            n64save::padconfig_set(s.padconfig_packed, i,
                                   (uint8_t)(config.controls.padconfig[i] & 0xF));
    }

    void save_to_eeprom()
    {
        saved_settings_v1 s;
        seed_record(s);
        n64save::save_settings(s);
    }

    // ===== Value cycling ====================================================

    // TIME DIFFICULTY composes freeze_timer + dip_time. Menu indices:
    //   0 = OFF (freeze_timer = true, dip_time preserved)
    //   1..4 = EASY / NORMAL / HARD / HARDEST (freeze_timer = false,
    //          dip_time = idx - 1)
    int time_index_of()
    {
        return config.engine.freeze_timer ? 0
             : 1 + (config.engine.dip_time & 3);
    }
    void time_set_from_index(int idx)
    {
        while (idx < 0) idx += 5;
        idx %= 5;
        if (idx == 0)
        {
            config.engine.freeze_timer = true;
        }
        else
        {
            config.engine.freeze_timer = false;
            config.engine.dip_time     = (idx - 1) & 3;
        }
    }

    // TRAFFIC DIFFICULTY composes disable_traffic + dip_traffic. Same
    // convention. The single knob also syncs cont_traffic + ttrial.traffic
    // since the engine reads them in different code paths and a per-row
    // breakdown was confusing without an OFF cue on each.
    int traffic_index_of()
    {
        return config.engine.disable_traffic ? 0
             : 1 + (config.engine.dip_traffic & 3);
    }
    void traffic_set_from_index(int idx)
    {
        while (idx < 0) idx += 5;
        idx %= 5;
        if (idx == 0)
        {
            config.engine.disable_traffic = true;
        }
        else
        {
            config.engine.disable_traffic = false;
            const int v = (idx - 1) & 3;
            config.engine.dip_traffic = v;
            config.cont_traffic       = v;
            config.ttrial.traffic     = v;
        }
    }

    void cycle_option_row(int row, int dir)
    {
        switch (row)
        {
            case O_REGION:
                config.engine.jap = !config.engine.jap;
                break;
            case O_TIME:
                time_set_from_index(time_index_of() + dir);
                break;
            case O_TRAFFIC:
                traffic_set_from_index(traffic_index_of() + dir);
                break;
            case O_COLOR:
            {
                int v = config.engine.car_pal + dir;
                while (v < 0) v += 5;
                while (v > 4) v -= 5;
                config.engine.car_pal = v;
                break;
            }
            case O_TRANSMISSION:
                // Engine recognises 0..3 (BUTTON / PRESS / SEPARATE / AUTO);
                // we expose MANUAL=0 and AUTOMATIC=3.
                config.controls.gear = (config.controls.gear == 3) ? 0 : 3;
                break;
        }
    }

    bool pill_is_on(int pill)
    {
        switch (pill)
        {
            case P_TIRES:   return config.engine.grippy_tyres;
            case P_BUMPER:  return config.engine.bumper;
            case P_TURBO:   return config.engine.turbo;
            case P_OFFROAD: return config.engine.offroad;
        }
        return false;
    }
    void toggle_pill(int pill)
    {
        switch (pill)
        {
            case P_TIRES:   config.engine.grippy_tyres = !config.engine.grippy_tyres; break;
            case P_BUMPER:  config.engine.bumper       = !config.engine.bumper;       break;
            case P_TURBO:   config.engine.turbo        = !config.engine.turbo;        break;
            case P_OFFROAD: config.engine.offroad      = !config.engine.offroad;      break;
        }
    }

    sprite_t* options_value(int row)
    {
        switch (row)
        {
            case O_REGION:
                return config.engine.jap ? g_options.japan : g_options.world;
            case O_TIME:
            {
                const int i = time_index_of();
                return (i == 0) ? g_options.off : g_options.diff[i - 1];
            }
            case O_TRAFFIC:
            {
                const int i = traffic_index_of();
                return (i == 0) ? g_options.off : g_options.diff[i - 1];
            }
            case O_COLOR:
            {
                int v = config.engine.car_pal;
                if (v < 0 || v > 4) v = 0;
                return g_options.colors[v];
            }
            case O_TRANSMISSION:
                return (config.controls.gear == 3) ? g_options.automatic
                                                   : g_options.manual;
        }
        return nullptr;
    }

    // ===== Title screen =====================================================

    void draw_title(int selected)
    {
        surface_t* fb = display_get();
        rdpq_attach(fb, nullptr);
        setup_render_mode();

        rdpq_set_prim_color(COL_WHITE);
        rdpq_sprite_blit(g_title.bg, 0, 0, nullptr);

        int laps = config.ttrial.laps;
        if (laps < 1) laps = 1;
        else if (laps > 5) laps = 5;

        sprite_t* rows[T_COUNT] = {
            g_title.arcade,
            g_title.continuous,
            g_title.tt[laps - 1],
            g_title.options,
        };

        for (int i = 0; i < T_COUNT; ++i)
        {
            const float y = TITLE_Y0 + i * TITLE_STRIDE;
            const color_t tint = (i == selected) ? COL_HIGHLIGHT : COL_WHITE;
            blit_tinted(rows[i], TITLE_X, y, tint);
        }

        // Cursor placement: right caret left of the row for non-TT rows;
        // left + right carets flanking the row for TIME TRIALS (the inline
        // laps editor cue).
        const float row_y = TITLE_Y0 + selected * TITLE_STRIDE;
        const float row_cy = row_y + TITLE_LABEL_H * 0.5f;
        if (selected == T_TIME_TRIALS)
        {
            const float left_cx  = TITLE_X - CARET_GAP - CARET_DIM * 0.5f;
            const float right_cx = TITLE_X + TITLE_LABEL_W + CARET_GAP + CARET_DIM * 0.5f;
            blit_caret(g_title.caret, left_cx,  row_cy, CARET_LEFT,  COL_HIGHLIGHT);
            blit_caret(g_title.caret, right_cx, row_cy, CARET_RIGHT, COL_HIGHLIGHT);
        }
        else
        {
            const float cx = TITLE_X - CARET_GAP - CARET_DIM * 0.5f;
            blit_caret(g_title.caret, cx, row_cy, CARET_RIGHT, COL_HIGHLIGHT);
        }

        rdpq_detach_show();
    }

    enum title_result_t
    {
        TR_ARCADE,
        TR_CONTINUOUS,
        TR_TIME_TRIALS,
        TR_OPTIONS,
    };

    title_result_t run_title()
    {
        int selected = T_ARCADE;
        while (true)
        {
            pump_audio();
            input.poll();

            if (input.has_pressed(Input::DOWN))
            {
                selected = (selected + 1) % T_COUNT;
                play_sfx(&g_sfx_beep1);
            }
            else if (input.has_pressed(Input::UP))
            {
                selected = (selected + T_COUNT - 1) % T_COUNT;
                play_sfx(&g_sfx_beep1);
            }

            // Inline laps editor only on TT row. Wrap 1 ↔ 5 so a single
            // press from either end gets the player to the other extreme.
            if (selected == T_TIME_TRIALS)
            {
                if (input.has_pressed(Input::LEFT))
                {
                    config.ttrial.laps = (config.ttrial.laps <= 1)
                                       ? 5 : config.ttrial.laps - 1;
                    play_sfx(&g_sfx_beep1);
                }
                if (input.has_pressed(Input::RIGHT))
                {
                    config.ttrial.laps = (config.ttrial.laps >= 5)
                                       ? 1 : config.ttrial.laps + 1;
                    play_sfx(&g_sfx_beep1);
                }
            }

            const bool confirm = input.has_pressed(Input::START)
                              || input.has_pressed(Input::ACCEL);

            draw_title(selected);
            input.frame_done();

            if (confirm)
            {
                // OPTIONS is a navigation step (beep2). Picking a game mode
                // commits to launching the game — coin_in mirrors the
                // arcade's "you're in" jingle.
                play_sfx(selected == T_OPTIONS ? &g_sfx_beep2
                                               : &g_sfx_coin);
                switch (selected)
                {
                    case T_ARCADE:        return TR_ARCADE;
                    case T_CONTINUOUS:    return TR_CONTINUOUS;
                    case T_TIME_TRIALS:   return TR_TIME_TRIALS;
                    case T_OPTIONS:       return TR_OPTIONS;
                }
            }
        }
    }

    // ===== Options screen ===================================================

    void draw_options(int selected, int pill_selected)
    {
        surface_t* fb = display_get();
        rdpq_attach(fb, nullptr);
        setup_render_mode();

        rdpq_set_prim_color(COL_WHITE);
        rdpq_sprite_blit(g_options.bg, 0, 0, nullptr);

        // Top 5 rows.
        for (int i = 0; i < 5; ++i)
        {
            const float y = OPT_Y0 + i * OPT_STRIDE;
            const color_t tint = (selected == i) ? COL_HIGHLIGHT : COL_WHITE;
            blit_tinted(g_options.labels[i], OPT_X, y, tint);
            sprite_t* val = options_value(i);
            if (val)
            {
                // Right-align the value inside its 92-wide slot so values of
                // different widths still hug the right margin.
                const float x = OPT_VALUE_X + (OPT_VALUE_W - val->width);
                blit_tinted(val, x, y, tint);
            }
        }

        // Row caret on the active top-5 row (left side, ▶).
        if (selected < 5)
        {
            const float row_y  = OPT_Y0 + selected * OPT_STRIDE;
            const float row_cy = row_y + OPT_LABEL_H * 0.5f;
            const float cx     = OPT_X - CARET_GAP - CARET_DIM * 0.5f;
            blit_caret(g_options.caret, cx, row_cy, CARET_RIGHT, COL_HIGHLIGHT);
        }

        // Pill row — pills flush at x=36, no gap.
        int pill_x[P_COUNT];
        int x = PILL_X0;
        for (int i = 0; i < P_COUNT; ++i)
        {
            pill_x[i] = x;
            sprite_t* pill = g_options.pills[i];
            const color_t tint = pill_is_on(i) ? COL_HIGHLIGHT : COL_PILL_OFF;
            blit_tinted(pill, x, PILL_Y, tint);
            x += pill->width;
        }

        // Pill cursor (▲) below the active pill, only when the row is
        // selected.
        if (selected == O_CHEATS)
        {
            sprite_t* pill = g_options.pills[pill_selected];
            const float cx = pill_x[pill_selected] + pill->width * 0.5f;
            const float cy = PILL_Y + PILL_H + 2 + CARET_DIM * 0.5f;
            blit_caret(g_options.caret, cx, cy, CARET_UP, COL_HIGHLIGHT);
        }

        rdpq_detach_show();
    }

    void run_options()
    {
        int selected = O_REGION;
        int pill_selected = 0;

        while (true)
        {
            pump_audio();
            input.poll();

            if (input.has_pressed(Input::DOWN))
            {
                selected = (selected + 1) % O_COUNT;
                play_sfx(&g_sfx_beep1);
            }
            else if (input.has_pressed(Input::UP))
            {
                selected = (selected + O_COUNT - 1) % O_COUNT;
                play_sfx(&g_sfx_beep1);
            }

            if (selected == O_CHEATS)
            {
                if (input.has_pressed(Input::LEFT))
                {
                    pill_selected = (pill_selected + P_COUNT - 1) % P_COUNT;
                    play_sfx(&g_sfx_beep1);
                }
                else if (input.has_pressed(Input::RIGHT))
                {
                    pill_selected = (pill_selected + 1) % P_COUNT;
                    play_sfx(&g_sfx_beep1);
                }
                if (input.has_pressed(Input::ACCEL))
                {
                    toggle_pill(pill_selected);
                    play_sfx(&g_sfx_beep2);
                }
            }
            else
            {
                if (input.has_pressed(Input::LEFT))
                {
                    cycle_option_row(selected, -1);
                    play_sfx(&g_sfx_beep1);
                }
                else if (input.has_pressed(Input::RIGHT)
                      || input.has_pressed(Input::ACCEL))
                {
                    cycle_option_row(selected, +1);
                    play_sfx(&g_sfx_beep1);
                }
            }

            // B / START exits back to title (auto-saves on the way out).
            const bool exit = input.has_pressed(Input::BRAKE)
                           || input.has_pressed(Input::START);

            draw_options(selected, pill_selected);
            input.frame_done();

            if (exit)
            {
                play_sfx(&g_sfx_beep2);
                save_to_eeprom();
                return;
            }
        }
    }
}

namespace n64 { namespace boot_menu
{

void apply_saved_settings()
{
    saved_settings_v1 s;
    if (!n64save::load_settings(s)) return;

    config.engine.jap             = s.jap;
    config.engine.prototype       = 0;
    config.video.fps              = s.fps_mode;
    config.video.widescreen       = 0;
    config.sound.enabled          = 1;
    config.engine.freeze_timer    = s.freeze_timer;
    config.engine.disable_traffic = s.disable_traffic;
    config.engine.grippy_tyres    = s.grippy_tyres;
    config.engine.offroad         = s.offroad;
    config.engine.bumper          = s.bumper;
    config.engine.turbo           = s.turbo;
    config.engine.freeplay        = true;
    config.engine.fix_bugs        = true;
    config.engine.new_attract     = 1;
    config.engine.dip_time        = s.dip_time;
    config.engine.dip_traffic     = s.dip_traffic;
    config.cont_traffic           = s.cont_traffic;
    config.controls.gear          = s.gear;
    config.engine.car_pal         = s.car_pal;
    config.ttrial.laps            = s.ttrial_laps + 1;
    config.ttrial.traffic         = s.ttrial_traffic;
    config.controls.steer_speed   = s.steer_speed;
    config.controls.pedal_speed   = s.pedal_speed;
    config.controls.invert[0]     = s.invert_x;
    config.controls.invert[1]     = s.invert_y;
    config.controls.invert[2]     = s.invert_z;
    if (s.music_timer) config.sound.music_timer = s.music_timer;
    config.controls.rumble       = s.rumble / 255.0f;
    for (int i = 0; i < 12; ++i)
        config.controls.padconfig[i] =
            n64save::padconfig_get(s.padconfig_packed, i);
}

void run()
{
    start_music();
    load_title_sprites();

    while (true)
    {
        title_result_t r = run_title();

        if (r == TR_OPTIONS)
        {
            free_title_sprites();
            load_options_sprites();
            run_options();
            free_options_sprites();
            load_title_sprites();
            continue;
        }

        // A game mode was picked — set cannonball_mode and bow out so the
        // caller can proceed with engine init.
        switch (r)
        {
            case TR_ARCADE:      outrun.cannonball_mode = Outrun::MODE_ORIGINAL; break;
            case TR_CONTINUOUS:  outrun.cannonball_mode = Outrun::MODE_CONT;     break;
            case TR_TIME_TRIALS: outrun.cannonball_mode = Outrun::MODE_TTRIAL;   break;
            default: break;
        }
        save_to_eeprom();
        free_title_sprites();
        stop_music();

        // Scrub both framebuffer pages to black before engine init takes
        // over. The engine only paints the 320×224 playfield, leaving the
        // top/bottom letterbox stripes untouched — without this clear the
        // residual title-screen pixels flash through the letterbox on the
        // first few engine frames.
        for (int i = 0; i < 2; ++i)
        {
            surface_t* fb = display_get();
            rdpq_attach_clear(fb, nullptr);
            rdpq_detach_show();
        }
        return;
    }
}

}}
