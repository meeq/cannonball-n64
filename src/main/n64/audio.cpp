/***************************************************************************
    N64 Audio — Phase 4c (RSP mixer + wav64 dispatch).

    Everything audible runs through libdragon's RSP audio mixer.

    SegaPCM voices map 1:1 onto mixer channels 0..15: each is a custom 8-bit
    mono waveform_t that streams bytes out of the (in-place-converted-to-
    signed) PCM ROM. Per-frame, reconcile_pcm() scans the SegaPCM register
    file via osoundint.pcm_ram and translates Z80 writes into mixer state
    (play/stop, freq, volume, loop). Engine tone and traffic noise live on
    these channels too (engine_process / traffic_process write straight
    into pcm_ram).

    The eleven YM2151-driven sounds — four music tracks, two music-channel
    jingles, five FM SFX — were pre-rendered host-side to VADPCM wav64 and
    bake into the DFS payload. Their sound::* IDs are intercepted at the
    Z80 queue (OSoundInt::add_to_queue → wav64_intercept) and dispatched
    onto mixer channels 16–17 (music + jingles, stereo pair) and 18–19
    (FM SFX, stereo pair) directly, bypassing the YM2151 emulator entirely.
    The YM2151 chip object stays allocated so OSound's cheap register-poll
    calls (read_status, write_reg) keep working, but stream_update() is
    never invoked.

    SegaPCM::stream_update() is also never called on N64; its 16-voice C++
    mix loop was the dominant chip-side CPU cost and the RSP path replaces
    it wholesale.
***************************************************************************/

#include "audio.hpp"

#include <libdragon.h>
#include <cstring>

#include "../engine/audio/osoundint.hpp"
#include "../engine/audio/commands.hpp"
#include "../frontend/config.hpp"
#include "../roms.hpp"
#include "../romloader.hpp"
#include "rendersurface.hpp"  // n64_profile counters

namespace
{
    // ---- Mixer channel layout -------------------------------------------
    //
    // 0..15  SegaPCM voices (one mixer ch per chip voice)
    // 16..17 wav64 music + music-channel jingles (stereo pair: 16=L, 17=R)
    // 18..19 wav64 FM SFX (stereo pair: 18=L, 19=R)
    //
    // The wav64s are stereo (rendered host-side from the SDL2 mix), and
    // libdragon's mixer represents a stereo waveform as TWO consecutive
    // channels — the second is flagged CH_FLAGS_STEREO_SUB and may not be
    // played independently. So each wav64 slot reserves a pair.
    //
    // YM2151 has no audible channel any more: every sound the Z80 would
    // have routed to YM is intercepted at the queue and replaced with a
    // wav64. The YM2151 object stays allocated so OSound can still call
    // ym->read_status() / ym->write_reg() (both cheap), but its
    // stream_update() is never invoked on N64.
    constexpr int N_PCM_CH     = 16;
    constexpr int WAV64_MUS_CH = N_PCM_CH;       // 16 (+17 stereo-sub)
    constexpr int WAV64_SFX_CH = N_PCM_CH + 2;   // 18 (+19 stereo-sub)
    constexpr int N_MIXER_CH   = N_PCM_CH + 4;   // 20

    // OutRun's SegaPCM is constructed with BANK_512 against the 512 KiB
    // PCM ROM. SegaPCM's constructor resolves these to fixed values that
    // we mirror here rather than plumbing accessors through the chip:
    //   bankshift = 12, bankmask = 0x70 (post-shift, after rom_mask clamp),
    //   rgnmask   = 0x7FFFF.
    // If a different OutRun ROM revision shipped with a different PCM size
    // these would have to be recomputed.
    constexpr int      PCM_BANK_SHIFT  = 12;
    constexpr uint32_t PCM_BANK_MASK   = 0x70;
    constexpr uint32_t PCM_REGION_MASK = 0x7FFFF;

    inline uint32_t voice_bank_offset(uint8_t flags86)
    {
        return ((uint32_t)flags86 & PCM_BANK_MASK) << PCM_BANK_SHIFT;
    }

    struct PcmVoiceCtx
    {
        uint32_t base_byte; // ROM byte offset of waveform start (bank-adjusted)
    };

    waveform_t   pcm_wave[N_PCM_CH];
    PcmVoiceCtx  pcm_ctx[N_PCM_CH];

    // Last-seen register snapshot per voice. Used to detect "the Z80 just
    // (re)triggered this voice" — there's no explicit trigger bit, so we
    // diff the active flag and start/end address fields.
    struct PcmTrack
    {
        uint8_t prev_flags86;
        uint8_t prev_addr_lo;
        uint8_t prev_addr_hi;
        uint8_t prev_end;
        bool    playing;
    };
    PcmTrack pcm_track[N_PCM_CH];

    // PCM ROM converted from unsigned-biased to signed (one-time, in-place).
    int8_t* pcm_rom_signed = nullptr;
    int     pcm_rom_len    = 0;

    void pcm_voice_read(void* ctx_, samplebuffer_t* sbuf, int wpos, int wlen, bool /*seeking*/)
    {
        const PcmVoiceCtx* ctx = (const PcmVoiceCtx*)ctx_;
        uint8_t* dst = (uint8_t*)samplebuffer_append(sbuf, wlen);
        const int8_t* src = pcm_rom_signed + ctx->base_byte + wpos;
        memcpy(dst, src, wlen);
    }

    // SegaPCM samples ship unsigned-biased; the mixer needs signed PCM. We
    // convert the ROM in place since SegaPCM::stream_update() is never
    // called on N64 and nothing else reads roms.pcm.rom. The conversion
    // must wait until roms.load_revb_roms() has run, which happens *after*
    // audio.init() — so do it lazily on the first reconcile that finds the
    // ROM loaded. Skipping this gate meant pcm_rom_len stayed 0 and every
    // play attempt was silently rejected by the bank+length guard.
    void pcm_rom_signed_ensure()
    {
        if (pcm_rom_signed || !roms.pcm.loaded) return;
        pcm_rom_len = (int)roms.pcm.length;
        for (int i = 0; i < pcm_rom_len; i++)
            roms.pcm.rom[i] = (uint8_t)(roms.pcm.rom[i] ^ 0x80);
        pcm_rom_signed = (int8_t*)roms.pcm.rom;
    }

    void reconcile_pcm()
    {
        pcm_rom_signed_ensure();
        if (!pcm_rom_signed) return;  // ROMs not loaded yet — nothing to play

        for (int ch = 0; ch < N_PCM_CH; ch++)
        {
            uint8_t* regs = osoundint.pcm_ram + 8 * ch;

            uint8_t flags86 = regs[0x86];
            bool active     = (flags86 & 1) == 0;
            bool was_active = (pcm_track[ch].prev_flags86 & 1) == 0;
            bool loop_off   = (flags86 & 2) != 0;

            uint8_t addr_lo = regs[0x04];
            uint8_t addr_hi = regs[0x05];
            uint8_t end     = regs[0x06];

            bool addr_changed = (addr_lo != pcm_track[ch].prev_addr_lo) ||
                                (addr_hi != pcm_track[ch].prev_addr_hi) ||
                                (end     != pcm_track[ch].prev_end);

            if (!active)
            {
                if (pcm_track[ch].playing)
                {
                    mixer_ch_stop(ch);
                    pcm_track[ch].playing = false;
                }
            }
            else if (!was_active || addr_changed)
            {
                uint32_t bank_off  = voice_bank_offset(flags86);
                uint32_t addr_byte = (((uint32_t)addr_hi << 8) | addr_lo) & PCM_REGION_MASK;
                uint32_t end_byte  = ((uint32_t)end + 1) << 8;
                uint32_t base      = bank_off + addr_byte;
                int      length    = (int)(end_byte - addr_byte);

                // The previous version silently `continue`d when length
                // <= 0 or base+length > pcm_rom_len. That hid the fact
                // that pcm_rom_len was 0 (audio.init ran before roms
                // loaded) — every PCM voice was dropped for months.
                // Assert loudly per feedback_never_silent_drop_content.
                assertf(length > 0,
                        "pcm ch=%d: bad sample length %d (addr=%04x end=%02x)",
                        ch, length, (unsigned)((addr_hi << 8) | addr_lo), end);
                assertf((int)(base + length) <= pcm_rom_len,
                        "pcm ch=%d: sample [%lu..%lu) past ROM end %d",
                        ch, (unsigned long)base,
                        (unsigned long)(base + length), pcm_rom_len);

                pcm_ctx[ch].base_byte = base;
                pcm_wave[ch].len      = length;
                pcm_wave[ch].loop_len = loop_off ? 0 : length;

                // Force the mixer to re-read len/loop_len from the
                // waveform. mixer_ch_play's fast path keeps the cached
                // channel state when uuid matches, which would mean
                // playing the *previous* sample's length on a retrigger.
                pcm_wave[ch].__uuid = 0;
                mixer_ch_play(ch, &pcm_wave[ch]);
                pcm_track[ch].playing = true;
            }

            if (pcm_track[ch].playing)
            {
                // regs[7] is the per-sample increment at SegaPCM's native
                // 32 kHz. delta=256 means "advance one source byte per
                // 32 kHz tick" → playback rate = 32000 * delta/256.
                float freq = 32000.0f * (float)regs[7] / 256.0f;
                if (freq < 1.0f) freq = 1.0f;
                mixer_ch_set_freq(ch, freq);

                // Per-voice attenuation. SegaPCM treats vol=0xFF as full
                // scale per voice (sample = int8 × regs[2] in MAME), so
                // regs[2]/255 matches the SDL build's per-voice level.
                // OutRun's Z80 caps VOL_L/VOL_R at 0x40 (see osound.cpp
                // VOL_MAX) and engine tones at 0x3F (get_adjusted_vol), so
                // a max-volume voice sits at ~0.25 amplitude — leaves room
                // for the typical 1–3 simultaneous voices to sum without
                // clipping, and matches what the SDL mixer hears.
                float lvol = (float)regs[2] / 255.0f;
                float rvol = (float)regs[3] / 255.0f;
                mixer_ch_set_vol(ch, lvol, rvol);

                // One-shot completion: the chip-side stream_update would
                // set bit 0 when a non-looping voice runs off the end of
                // the sample. Replicate that so the Z80 polling code sees
                // the voice as free.
                if (!mixer_ch_playing(ch))
                {
                    regs[0x86] |= 1;
                    pcm_track[ch].playing = false;
                }
            }

            pcm_track[ch].prev_flags86 = regs[0x86];
            pcm_track[ch].prev_addr_lo = addr_lo;
            pcm_track[ch].prev_addr_hi = addr_hi;
            pcm_track[ch].prev_end     = end;
        }
    }

    // ---- Z80 audio code wall-clock driver -------------------------------
    //
    // The Z80 audio code (osound) must advance at a fixed 125 Hz independent
    // of how fast the main loop runs — otherwise music tempo scales with the
    // renderer's frame rate. We accumulate elapsed wall-clock microseconds
    // per audio.tick() and dispatch the corresponding number of Z80 ticks.

    constexpr double US_PER_Z80_TICK = 1000000.0 / 125.0;  // 8000 us
    constexpr int    Z80_MAX_CATCHUP = 50;                 // cap per-call burst

    double   z80_us_pending = 0.0;
    uint64_t z80_last_us    = 0;

    void advance_z80_audio()
    {
        uint64_t now = get_ticks_us();
        if (z80_last_us == 0)
        {
            z80_last_us = now;
            return;
        }

        z80_us_pending += (double)(now - z80_last_us);
        z80_last_us = now;

        // If the loop stalled for a long time (DFS load, init), don't try
        // to replay the full backlog — that would crash the music forward
        // and possibly overflow the sound command queue.
        const double MAX_PENDING = US_PER_Z80_TICK * Z80_MAX_CATCHUP;
        if (z80_us_pending > MAX_PENDING)
            z80_us_pending = MAX_PENDING;

        int n = (int)(z80_us_pending / US_PER_Z80_TICK);
        if (n > 0)
        {
            osoundint.advance(n);
            z80_us_pending -= n * US_PER_Z80_TICK;
        }
    }

    // ---- wav64 intercept for YM-rendered sounds ------------------------
    //
    // The eleven sound IDs the Z80 audio code routes to YM2151 — four music
    // tracks, two music-channel jingles, five FM SFX — are pre-rendered
    // host-side (tools/audio-render) to VADPCM wav64 files baked into the
    // DFS payload. We intercept those IDs at the queue and play the wav64
    // through the mixer directly, bypassing the FM emulator for them. The
    // YM2151 emulator stays running so engine-tone + traffic noise (which
    // are continuous and not pre-renderable) keep working.

    struct Wav64Slot
    {
        uint8_t     id;
        const char* path;
        bool        is_music; // routes to MUS channel (replaces prior music); else SFX channel
    };

    constexpr Wav64Slot WAV64_TABLE[] =
    {
        // Music — looping per wav2wav64.sh except LASTWAVE (one-shot; ~93 s).
        { sound::MUSIC_BREEZE,   "rom:/audio/music_breeze.wav64",   true  },
        { sound::MUSIC_SPLASH,   "rom:/audio/music_splash.wav64",   true  },
        { sound::MUSIC_MAGICAL,  "rom:/audio/music_magical.wav64",  true  },
        { sound::MUSIC_LASTWAVE, "rom:/audio/music_lastwave.wav64", true  },

        // Music-channel jingles — fm_reset's in the original, so they
        // clobber whatever music was playing.
        { sound::UFO,            "rom:/audio/ym_ufo.wav64",         true  },
        { sound::BEEP2,          "rom:/audio/ym_beep2.wav64",       true  },

        // FM SFX — YM_FX1 in the original, one-shot.
        { sound::COIN_IN,        "rom:/audio/ym_coin_in.wav64",     false },
        { sound::YM_CHECKPOINT,  "rom:/audio/ym_checkpoint.wav64",  false },
        { sound::SIGNAL1,        "rom:/audio/ym_signal1.wav64",     false },
        { sound::SIGNAL2,        "rom:/audio/ym_signal2.wav64",     false },
        { sound::BEEP1,          "rom:/audio/ym_beep1.wav64",       false },
    };
    constexpr int N_WAV64 = sizeof(WAV64_TABLE) / sizeof(WAV64_TABLE[0]);

    wav64_t wav64_files[N_WAV64];
    bool    wav64_loaded[N_WAV64] = {false};

    int wav64_index_of(uint8_t snd)
    {
        for (int i = 0; i < N_WAV64; i++)
            if (WAV64_TABLE[i].id == snd) return i;
        return -1;
    }

    bool wav64_intercept(uint8_t snd)
    {
        // FM_RESET (and 0xFF, treated identically by OSound::process_command)
        // is the engine's "stop music" command. Silence the wav64 music
        // channel here and let Z80 still see it so the YM2151 engine-tone /
        // traffic-noise state also resets correctly.
        if (snd == sound::FM_RESET || snd == 0xFF)
        {
            mixer_ch_stop(WAV64_MUS_CH);
            return false;
        }

        int idx = wav64_index_of(snd);
        if (idx < 0)            return false;  // not ours; let Z80 handle
        if (!wav64_loaded[idx]) return true;   // wav64 missing → silently drop

        int ch = WAV64_TABLE[idx].is_music ? WAV64_MUS_CH : WAV64_SFX_CH;

        // Stop whatever was on this channel — switching tracks, retriggering
        // an SFX mid-play, or interrupting music with a jingle.
        mixer_ch_stop(ch);
        wav64_play(&wav64_files[idx], ch);
        return true;
    }
}

Audio::Audio() = default;

Audio::~Audio()
{
    if (dac_initialised)
    {
        mixer_close();
        audio_close();
    }
}

void Audio::init()
{
    if (!config.sound.enabled)
        return;

    // 4 buffers at rate/25 each = ~160 ms of slack.
    audio_init(config.sound.rate, 4);
    dac_initialised = true;

    mixer_init(N_MIXER_CH);

    // The signed-conversion of roms.pcm.rom happens lazily in reconcile_pcm
    // once the ROM is actually loaded — roms.load_revb_roms() runs after
    // Audio::init() in n64main, so it's not loaded yet here.

    for (int ch = 0; ch < N_PCM_CH; ch++)
    {
        pcm_wave[ch].name       = "segapcm_voice";
        pcm_wave[ch].bits       = 8;
        pcm_wave[ch].channels   = 1;
        pcm_wave[ch].frequency  = 32000.0f;
        pcm_wave[ch].len        = 0;
        pcm_wave[ch].loop_len   = 0;
        pcm_wave[ch].start      = nullptr;
        pcm_wave[ch].read       = pcm_voice_read;
        pcm_wave[ch].ctx        = &pcm_ctx[ch];
        pcm_wave[ch].state_size = 0;
        pcm_wave[ch].__uuid     = 0;

        // Pin each channel to its actual upper bound (8-bit @ 32 kHz),
        // so the mixer doesn't allocate the default 16-bit-at-output-rate
        // sized sample buffer.
        mixer_ch_set_limits(ch, 8, 32000.0f, 0);

        pcm_track[ch].prev_flags86 = 1;     // "inactive" so first active
        pcm_track[ch].prev_addr_lo = 0;     // edge triggers a play
        pcm_track[ch].prev_addr_hi = 0;
        pcm_track[ch].prev_end     = 0;
        pcm_track[ch].playing      = false;
    }

    // wav64 channels: 16-bit, native wav64 sample rate (22050 Hz from the
    // convert script). Defaults would work but pinning avoids the mixer
    // sizing buffers for a higher cap than the wav64s actually use.
    constexpr float WAV64_MAX_RATE = 22050.0f;
    mixer_ch_set_limits(WAV64_MUS_CH, 16, WAV64_MAX_RATE, 0);
    mixer_ch_set_limits(WAV64_SFX_CH, 16, WAV64_MAX_RATE, 0);

    int n_wav64_ok = 0;
    for (int i = 0; i < N_WAV64; i++)
    {
        // wav64_open asserts on missing file rather than returning an error,
        // so probe the DFS handle first to avoid panicking on partial builds.
        int fp = dfs_open(WAV64_TABLE[i].path + 5);  // strip "rom:/"
        if (fp < 0)
        {
            debugf("audio: wav64 missing %s\n", WAV64_TABLE[i].path);
            wav64_loaded[i] = false;
            continue;
        }
        dfs_close(fp);

        wav64_open(&wav64_files[i], WAV64_TABLE[i].path);
        wav64_loaded[i] = true;
        n_wav64_ok++;
    }

    osoundint.set_intercept(wav64_intercept);

    z80_us_pending = 0.0;
    z80_last_us    = 0;

    sound_enabled = true;

    debugf("audio: init rate=%d, mixer up with %d PCM ch + %d/%d wav64 loaded\n",
           audio_get_frequency(), N_PCM_CH, n_wav64_ok, N_WAV64);
}

// Config::set_fps() bounces stop_audio/start_audio around osoundint.init().
// The DAC is brought up once in init() and stays up; these just gate tick().
void Audio::start_audio() { if (dac_initialised) sound_enabled = true;  }
void Audio::stop_audio()  { sound_enabled = false; }

double Audio::adjust_speed() { return 1.0; }

void Audio::load_wav(const char* /*filename*/) {}
void Audio::clear_wav() {}

void Audio::tick()
{
    auto smooth = [](uint32_t& acc, uint64_t sample)
    {
        acc = (uint32_t)((acc * 7 + sample) >> 3);
    };

    // Always advance the Z80 audio code on wall-clock, even before
    // sound_enabled flips on, so the chip register stream stays consistent
    // with the engine. Cheap when there are no pending ticks.
    uint64_t t_z80_0 = get_ticks_us();
    advance_z80_audio();
    uint64_t t_z80_1 = get_ticks_us();
    smooth(n64_profile::aud_z80_us, t_z80_1 - t_z80_0);
    n64_profile::raw_aud_z80_us = (uint32_t)(t_z80_1 - t_z80_0);

    if (!sound_enabled) return;

    uint64_t t_pcm_0 = get_ticks_us();
    reconcile_pcm();
    uint64_t t_pcm_1 = get_ticks_us();
    smooth(n64_profile::aud_pcm_us, t_pcm_1 - t_pcm_0);
    n64_profile::raw_aud_pcm_us = (uint32_t)(t_pcm_1 - t_pcm_0);

    const int blen = audio_get_buffer_length();
    if (blen <= 0) return;

    // mixer_poll fills one libdragon audio buffer at a time. The audio queue
    // holds 4 buffers (~160 ms at 22 kHz), so one poll per tick keeps the
    // queue topped up at the 25 buffers/sec consumption rate without piling
    // up CPU work in a single tick when the queue had drained.
    constexpr int MAX_POLLS_PER_TICK = 2;
    int polls = 0;
    uint64_t t_mix_0 = get_ticks_us();
    while (audio_can_write() && polls < MAX_POLLS_PER_TICK)
    {
        int16_t* buf = audio_write_begin();
        mixer_poll(buf, blen);
        audio_write_end();
        polls++;
    }
    uint64_t t_mix_1 = get_ticks_us();
    smooth(n64_profile::aud_mix_us, t_mix_1 - t_mix_0);
    n64_profile::raw_aud_mix_us = (uint32_t)(t_mix_1 - t_mix_0);
}
