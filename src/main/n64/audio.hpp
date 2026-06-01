/***************************************************************************
    N64 / libdragon Audio.

    Same public class shape as src/main/sdl2/audio.hpp so engine and frontend
    code is platform-agnostic. Phase 1 ships as a no-op (sound disabled);
    Phase 4 wires the libdragon mixer / PCM+YM streams.
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
    // Enable/Disable Sound — public so config and main code can read it,
    // matching the SDL Audio's member.
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
};
