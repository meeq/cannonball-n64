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

    // Pre-flush libdragon mixer's lazy per-channel sample-buffer alloc.
    // Must run AFTER roms.load_revb_roms — the ~1 MiB ROM allocation needs
    // a contiguous heap, and priming bites into that headroom. Calling
    // from main() at the right point keeps the failure mode predictable:
    // if 4 MiB can't fit all the per-channel buffers, OOM here at boot
    // instead of during gameplay the first time an unused voice fires.
    void   prime_mixer_buffers();

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
