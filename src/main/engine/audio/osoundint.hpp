/***************************************************************************
    Interface to Ported Z80 Code.
    Handles the interface between 68000 program code and Z80.

    Also abstracted here, so the more complex OSound class isn't exposed
    to the main code directly
    
    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#pragma once

#include "hwaudio/segapcm.hpp"
#include "hwaudio/ym2151.hpp"
#include "engine/audio/commands.hpp"

class OSoundInt
{
public:
    // SoundChip: Sega Custom Sample Generator
    SegaPCM* pcm;

    // SoundChip: Yamaha YM2151
    YM2151*  ym;

    const static uint16_t PCM_RAM_SIZE = 0x100;

    // Note whether the game has booted
    bool has_booted;

    // [+0] Unused
    // [+1] Engine pitch high
    // [+2] Engine pitch low
    // [+3] Engine pitch vol
    // [+4] Traffic data #1
    // [+5] Traffic data #2
    // [+6] Traffic data #3
    // [+7] Traffic data #4
    uint8_t engine_data[8];

    // SegaPCM register RAM. Exposed publicly on N64 so the libdragon mixer
    // backend (Audio::tick) can scan voice state directly without going
    // through SegaPCM::stream_update — SegaPCM's per-sample mix loop is
    // bypassed on N64 in favor of RSP-side mixing.
    uint8_t* pcm_ram;

    OSoundInt();
    ~OSoundInt();

    void init();
    void reset();
    void tick();

    // Advance the Z80 audio code by an explicit number of 125 Hz ticks.
    // tick() above advances by (125/config.fps) ticks per call, which assumes
    // it's invoked from a frame-rate-paced loop. On N64 the main loop's
    // frame rate is content-dependent, so audio drives this directly from
    // wall clock instead.
    void advance(int ticks);

    void play_queued_sound();
    void queue_sound_service(uint8_t snd);
    void queue_sound(uint8_t snd);
    void queue_clear();

    // Optional platform-installed intercept. Called from add_to_queue before
    // the command lands in the Z80 queue; returning true consumes the
    // command (Z80 audio code never sees it). The N64 backend installs this
    // to route YM2151-rendered sounds to pre-decoded wav64 playback while
    // leaving SegaPCM SFX to flow through the original Z80 dispatch.
    typedef bool (*intercept_fn_t)(uint8_t snd);
    void set_intercept(intercept_fn_t fn) { intercept = fn; }

private:
    intercept_fn_t intercept = nullptr;
    // 4 MHz
    static const uint32_t SOUND_CLOCK = 4000000;

    // Fractionally counts number of times audio code must be called
    // We call the audio code 125 times per frame from a timing perspective.
    double audio_ticks;

    // Controls what type of sound we're going to process in the interrupt routine
    uint8_t sound_counter;

    static const uint8_t QUEUE_LENGTH = 0x1F;
    uint8_t queue[QUEUE_LENGTH + 1];

    // Number of sounds queued
    uint8_t sounds_queued;

    // Positions in the queue
    uint8_t sound_head, sound_tail;

    void add_to_queue(uint8_t snd);
};

extern OSoundInt osoundint;