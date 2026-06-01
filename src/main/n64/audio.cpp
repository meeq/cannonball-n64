/***************************************************************************
    N64 Audio — Phase 1 stubs. No-ops keep the engine and config wiring
    quiet while sound stays disabled. Phase 4 swaps the bodies for a
    libdragon mixer integration that drains osoundint's PCM/YM streams.
***************************************************************************/

#include "audio.hpp"

Audio::Audio()  = default;
Audio::~Audio() = default;

void   Audio::init()        {}
void   Audio::tick()        {}
void   Audio::start_audio() { sound_enabled = false; }
void   Audio::stop_audio()  { sound_enabled = false; }
double Audio::adjust_speed(){ return 1.0; }
void   Audio::load_wav(const char* /*filename*/) {}
void   Audio::clear_wav()   {}
