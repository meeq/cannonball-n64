/***************************************************************************
    XML Configuration File Handling.

    Load Settings.
    Load & Save Hi-Scores.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include "config.hpp"
#include "globals.hpp"
#include "../main.hpp"
#include "../utils.hpp"

#include "engine/ohiscore.hpp"
#include "engine/outils.hpp"
#include "engine/audio/osoundint.hpp"
#include "../n64/save.hpp"

Config config;

Config::Config(void)
{
    data.cfg_file = "./config.xml";
    
    // Setup default sounds
    music_t magical, breeze, splash;
    magical.title = "MAGICAL SOUND SHOWER";
    breeze.title  = "PASSING BREEZE";
    splash.title  = "SPLASH WAVE";
    magical.type  = music_t::IS_YM_INT;
    breeze.type   = music_t::IS_YM_INT;
    splash.type   = music_t::IS_YM_INT;
    magical.cmd   = sound::MUSIC_MAGICAL;
    breeze.cmd    = sound::MUSIC_BREEZE;
    splash.cmd    = sound::MUSIC_SPLASH;
    sound.music.push_back(magical);
    sound.music.push_back(breeze);
    sound.music.push_back(splash);
}


Config::~Config(void)
{
}


// Set Path to load and save config to
void Config::set_config_file(const std::string& file)
{
    data.cfg_file = file;
}

// Hardcoded defaults. Save data persistence is delegated to the platform
// layer (see src/main/n64/save.cpp).
void Config::load()
{
    // Data Settings — platform layer overrides rom_path/res_path/save_path
    // after load() if defaults aren't right (eg. libdragon DFS "rom:/" prefix).
    data.rom_path  = "./roms/";
    data.res_path  = "./res/";
    data.save_path = "./";
    data.crc32     = 0;         // filename mode by default (no dirent assumed)

    data.file_scores      = data.save_path + "hiscores.xml";
    data.file_scores_jap  = data.save_path + "hiscores_jap.xml";
    data.file_ttrial      = data.save_path + "hiscores_timetrial.xml";
    data.file_ttrial_jap  = data.save_path + "hiscores_timetrial_jap.xml";
    data.file_cont        = data.save_path + "hiscores_continuous.xml";
    data.file_cont_jap    = data.save_path + "hiscores_continuous_jap.xml";

    // Menu — boot straight into attract on first cut
    menu.enabled           = 0;
    menu.road_scroll_speed = 50;

    // Video — minimal-output defaults (no widescreen, no hi-res). Concrete
    // mode/scale set by the platform layer (eg. matches N64 framebuffer).
    video.mode       = video_settings_t::MODE_FULL;
    video.scale      = 1;
    video.scanlines  = 0;
    video.fps        = 0;
    video.fps_count  = 0;
    video.widescreen = 0;
    video.hires      = 0;
    video.filtering  = 0;
    video.vsync      = 1;
    video.shadow     = 0;

    // Sound — disabled when no XML; platform layer can flip on after init.
    sound.enabled     = 0;
    sound.rate        = 22050;
    sound.advertise   = 1;
    sound.preview     = 1;
    sound.fix_samples = 1;
    sound.music_timer = MUSIC_TIMER;

    // SMARTYPI — off by default
    smartypi.enabled = 0;
    smartypi.ouputs  = 0;
    smartypi.cabinet = 1;

    // Controls — platform layer overrides padconfig/axis to match its input
    // backend. keyconfig is harmless on platforms without a keyboard.
    controls.gear        = controls_settings_t::GEAR_AUTO;
    controls.steer_speed = 3;
    controls.pedal_speed = 4;
    controls.rumble      = 1.0f;
    for (int i = 0; i < 12; ++i)  controls.keyconfig[i] = 0;
    for (int i = 0; i < 15; ++i)  controls.padconfig[i] = -1;
    controls.analog        = 1;
    controls.pad_id        = 0;
    for (int i = 0; i < 4; ++i)   controls.axis[i] = -1;
    for (int i = 0; i < 3; ++i)   controls.invert[i] = 0;
    controls.asettings[0]  = 75;
    controls.asettings[1]  = 0;
    controls.haptic        = 0;
    controls.max_force     = 9000;
    controls.min_force     = 8500;
    controls.force_duration= 20;

    // Engine
    engine.dip_time        = 0;
    engine.dip_traffic     = 1;
    engine.freeze_timer    = false;
    engine.disable_traffic = false;
    engine.freeplay        = false;
    engine.jap             = 0;
    engine.prototype       = 0;
    engine.level_objects   = 1;
    engine.randomgen       = 1;
    engine.fix_bugs_backup = engine.fix_bugs = true;
    engine.fix_timer       = false;
    engine.layout_debug    = false;
    engine.hiscore_delete  = 1;
    engine.hiscore_timer   = HIGHSCORE_TIMER;
    engine.new_attract     = 1;
    engine.offroad         = false;
    engine.grippy_tyres    = false;
    engine.bumper          = false;
    engine.turbo           = false;
    engine.car_pal         = 0;

    ttrial.laps    = 5;
    ttrial.traffic = 3;
    cont_traffic   = 3;
}

bool Config::save() { return true; }

void Config::load_scores(bool original_mode)
{
    // EEPROM read failure (no-save build / fresh wipe / corrupted block) keeps
    // the defaults the caller seeded via ohiscore.init_def_scores().
    n64save::load_scores(original_mode, engine.jap != 0, ohiscore.scores);
}

void Config::save_scores(bool original_mode)
{
    n64save::save_scores(original_mode, engine.jap != 0, ohiscore.scores);
}

void Config::load_tiletrial_scores()
{
    static const uint16_t COUNTER_1M_15 = 0x11D0;
    if (n64save::load_ttrial(engine.jap != 0, ttrial.best_times))
        return;
    for (int i = 0; i < 15; i++)
        ttrial.best_times[i] = COUNTER_1M_15;
}

void Config::save_tiletrial_scores()
{
    n64save::save_ttrial(engine.jap != 0, ttrial.best_times);
}

bool Config::clear_scores()
{
    ohiscore.init_def_scores();
    // Re-seed the engine's in-RAM ttrial table too — Config::load_tiletrial_scores
    // already does this from defaults when EEPROM read fails.
    static const uint16_t COUNTER_1M_15 = 0x11D0;
    for (int i = 0; i < 15; i++)
        ttrial.best_times[i] = COUNTER_1M_15;
    n64save::wipe();
    return true;
}

void Config::set_fps(int fps)
{
    video.fps = fps;
    // Set core FPS to 30fps or 60fps
    this->fps = video.fps == 0 ? 30 : 60;
    
    // Original game ticks sprites at 30fps but background scroll at 60fps
    tick_fps  = video.fps < 2 ? 30 : 60;

    cannonball::frame_ms = 1000.0 / this->fps;

    if (config.sound.enabled)
        cannonball::audio.stop_audio();
    osoundint.init();
    if (config.sound.enabled)
        cannonball::audio.start_audio();
}

// Inc time setting from menu
void Config::inc_time()
{
    if (engine.dip_time == 3)
    {
        if (!engine.freeze_timer)
            engine.freeze_timer = 1;
        else
        {
            engine.dip_time = 0;
            engine.freeze_timer = 0;
        }
    }
    else
        engine.dip_time++;
}

// Inc traffic setting from menu
void Config::inc_traffic()
{
    if (engine.dip_traffic == 3)
    {
        if (!engine.disable_traffic)
            engine.disable_traffic = 1;
        else
        {
            engine.dip_traffic = 0;
            engine.disable_traffic = 0;
        }
    }
    else
        engine.dip_traffic++;
}