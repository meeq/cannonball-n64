/***************************************************************************
    N64 / libdragon Audio.

    Same public class shape as src/main/sdl2/audio.hpp so engine and frontend
    code is platform-agnostic. Phase 4c routes SegaPCM through libdragon's
    RSP mixer (one channel per voice) and dispatches pre-rendered VADPCM
    wav64 files for the eleven YM2151-driven music + FM SFX commands; the
    YM2151 emulator is retained for its cheap register interface only.
***************************************************************************/

#pragma once

#include "../stdint.hpp"
#include "../globals.hpp"

struct wav_t
{
    uint8_t  loaded;
    int16_t *data;
    uint32_t pos;
    uint32_t length;
};

class Audio
{
public:
    // Public so config and main code can read it (matches SDL Audio).
    bool sound_enabled = false;

    Audio();
    ~Audio();

    void   init();
    void   tick();
    void   start_audio();
    void   stop_audio();
    double adjust_speed();
    void   load_wav(const char* filename);
    void   clear_wav();

private:
    wav_t wavfile{};
    bool  dac_initialised = false;
};
