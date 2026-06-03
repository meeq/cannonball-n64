/***************************************************************************
    N64 / libdragon Audio.

    Same public class shape as src/main/sdl2/audio.hpp so engine and frontend
    code is platform-agnostic. Phase 4b routes SegaPCM through libdragon's
    RSP mixer (one channel per voice) and CPU-adds the YM2151 stream on top
    of the mixer's PCM output before handing the buffer to the AI.
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
