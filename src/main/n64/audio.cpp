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
    // SegaPCM exposes 16 hardware voices but OutRun's observed peak is 7
    // simultaneous; voice indices used cluster in the upper half (mask
    // 0xfaa0 in 4 minutes of attract). Mapping each voice to its own
    // mixer channel wastes ~32 KiB of heap (4 KiB × 8 idle channels) —
    // critical on the 4 MiB build where post-roms+hwroad headroom is
    // ~58 KiB. We keep 16 PcmTrack entries (matches the SegaPCM register
    // file 1:1) but allocate mixer channels from a smaller pool, binding
    // a slot at voice key-on and releasing on key-off.
    //
    // 0..7   SegaPCM voice pool (dynamically bound to active voices)
    // 8..9   wav64 music + music-channel jingles (stereo pair: 8=L, 9=R)
    // 10..11 wav64 FM SFX (stereo pair: 10=L, 11=R)
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
    constexpr int N_PCM_CH     = 16;             // SegaPCM HW voice count
    constexpr int N_PCM_SLOTS  = 8;              // mixer-channel pool size
    constexpr int WAV64_MUS_CH = N_PCM_SLOTS;        // 8 (+9 stereo-sub)
    constexpr int WAV64_SFX_CH = N_PCM_SLOTS + 2;    // 10 (+11 stereo-sub)
    constexpr int N_MIXER_CH   = N_PCM_SLOTS + 4;    // 12

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

    // Mixer-channel pool entry. `voice` is the bound SegaPCM voice index
    // (0..15) or -1 if the slot is free. `base_byte` is the bank-adjusted
    // ROM offset that pcm_voice_read streams from. `last_used_us` advances
    // each tick the slot is active so LRU eviction can pick the stalest
    // slot if peak concurrency ever exceeds N_PCM_SLOTS.
    struct PcmSlot
    {
        int8_t   voice;
        uint32_t base_byte;
        uint64_t last_used_us;
    };

    PcmSlot     pcm_slot[N_PCM_SLOTS];
    waveform_t  pcm_wave[N_PCM_SLOTS];

    // Last-seen register snapshot per voice + the currently-bound slot.
    // mix_slot == -1 means no mixer channel is currently rendering this
    // voice (either inactive, or evicted under pool pressure).
    struct PcmTrack
    {
        uint8_t prev_flags86;
        uint8_t prev_addr_lo;
        uint8_t prev_addr_hi;
        uint8_t prev_end;
        int8_t  mix_slot;
    };
    PcmTrack pcm_track[N_PCM_CH];

    // Counter for the dip log: how often did the pool overflow and force
    // an eviction of an already-active voice? Should stay 0 in normal
    // play (census peak = 7, pool = 8); non-zero means the pool is too
    // small and the audible loss is real.
    uint32_t pool_evictions = 0;

    int8_t pcm_pool_claim(int voice, uint64_t now_us)
    {
        // Prefer a free slot.
        for (int s = 0; s < N_PCM_SLOTS; s++)
        {
            if (pcm_slot[s].voice < 0)
            {
                pcm_slot[s].voice        = (int8_t)voice;
                pcm_slot[s].last_used_us = now_us;
                return (int8_t)s;
            }
        }
        // Pool full — evict the LRU slot. The previously-bound voice
        // loses its mixer channel; its next tick treats it as inactive
        // and re-claims if still active. Audible result: a brief drop
        // for the evicted voice.
        int8_t  evict  = 0;
        uint64_t oldest = pcm_slot[0].last_used_us;
        for (int s = 1; s < N_PCM_SLOTS; s++)
        {
            if (pcm_slot[s].last_used_us < oldest)
            {
                oldest = pcm_slot[s].last_used_us;
                evict  = (int8_t)s;
            }
        }
        int v_old = pcm_slot[evict].voice;
        mixer_ch_stop(evict);
        if (v_old >= 0) pcm_track[v_old].mix_slot = -1;
        pcm_slot[evict].voice        = (int8_t)voice;
        pcm_slot[evict].last_used_us = now_us;
        pool_evictions++;
        return evict;
    }

    void pcm_pool_release(int slot)
    {
        int v = pcm_slot[slot].voice;
        if (v >= 0) pcm_track[v].mix_slot = -1;
        pcm_slot[slot].voice = -1;
        mixer_ch_stop(slot);
    }

    // PCM ROM converted from unsigned-biased to signed (one-time, in-place).
    int8_t* pcm_rom_signed = nullptr;
    int     pcm_rom_len    = 0;

    void pcm_voice_read(void* ctx_, samplebuffer_t* sbuf, int wpos, int wlen, bool /*seeking*/)
    {
        const PcmSlot* slot = (const PcmSlot*)ctx_;
        uint8_t* dst = (uint8_t*)samplebuffer_append(sbuf, wlen);
        const int8_t* src = pcm_rom_signed + slot->base_byte + wpos;
        memcpy(dst, src, wlen);
    }

    // Silent read used only to prime mixer channel sample buffers at init.
    // libdragon mixer_ch_play lazily malloc_uncached's a per-channel buffer
    // sized by the channel's set_limits. Deferring that to mid-game means a
    // 4 MiB heap shortfall manifests as an OOM the first time a never-yet-
    // played voice fires (e.g. crash SFX after a long drive). Drain the lazy
    // allocs at init so any OOM happens here, predictably.
    void prime_silent_read(void*, samplebuffer_t* sbuf, int /*wpos*/, int wlen, bool)
    {
        void* dst = samplebuffer_append(sbuf, wlen);
        // wlen is in samples; samplebuffer_append returns enough bytes for
        // the channel's bit-depth × channel count. Zero it out — buffer is
        // never actually consumed because we mixer_ch_stop right after.
        memset(dst, 0, wlen * 4);
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

        const uint64_t now_us = get_ticks_us();

        for (int v = 0; v < N_PCM_CH; v++)
        {
            uint8_t* regs = osoundint.pcm_ram + 8 * v;

            uint8_t flags86 = regs[0x86];
            bool active     = (flags86 & 1) == 0;
            bool was_active = (pcm_track[v].prev_flags86 & 1) == 0;
            bool loop_off   = (flags86 & 2) != 0;

            uint8_t addr_lo = regs[0x04];
            uint8_t addr_hi = regs[0x05];
            uint8_t end     = regs[0x06];

            bool addr_changed = (addr_lo != pcm_track[v].prev_addr_lo) ||
                                (addr_hi != pcm_track[v].prev_addr_hi) ||
                                (end     != pcm_track[v].prev_end);

            int8_t slot = pcm_track[v].mix_slot;

            if (!active)
            {
                if (slot >= 0)
                {
                    pcm_pool_release(slot);
                    slot = -1;
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
                        "pcm v=%d: bad sample length %d (addr=%04x end=%02x)",
                        v, length, (unsigned)((addr_hi << 8) | addr_lo), end);
                assertf((int)(base + length) <= pcm_rom_len,
                        "pcm v=%d: sample [%lu..%lu) past ROM end %d",
                        v, (unsigned long)base,
                        (unsigned long)(base + length), pcm_rom_len);

                // Claim a slot if we don't already own one (key-on, or
                // re-acquire after a pool eviction). Retrigger on the
                // same slot otherwise.
                if (slot < 0)
                {
                    slot = pcm_pool_claim(v, now_us);
                    pcm_track[v].mix_slot = slot;
                }

                pcm_slot[slot].base_byte = base;
                pcm_wave[slot].len       = length;
                pcm_wave[slot].loop_len  = loop_off ? 0 : length;

                // Force the mixer to re-read len/loop_len from the
                // waveform. mixer_ch_play's fast path keeps the cached
                // channel state when uuid matches, which would mean
                // playing the *previous* sample's length on a retrigger.
                pcm_wave[slot].__uuid = 0;
                mixer_ch_play(slot, &pcm_wave[slot]);
            }

            if (slot >= 0)
            {
                pcm_slot[slot].last_used_us = now_us;

                // regs[7] is the per-sample increment at SegaPCM's native
                // 32 kHz. delta=256 means "advance one source byte per
                // 32 kHz tick" → playback rate = 32000 * delta/256.
                float freq = 32000.0f * (float)regs[7] / 256.0f;
                if (freq < 1.0f) freq = 1.0f;
                mixer_ch_set_freq(slot, freq);

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
                mixer_ch_set_vol(slot, lvol, rvol);

                // One-shot completion: the chip-side stream_update would
                // set bit 0 when a non-looping voice runs off the end of
                // the sample. Replicate that so the Z80 polling code sees
                // the voice as free.
                if (!mixer_ch_playing(slot))
                {
                    regs[0x86] |= 1;
                    pcm_pool_release(slot);
                }
            }

            pcm_track[v].prev_flags86 = regs[0x86];
            pcm_track[v].prev_addr_lo = addr_lo;
            pcm_track[v].prev_addr_hi = addr_hi;
            pcm_track[v].prev_end     = end;
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

    for (int s = 0; s < N_PCM_SLOTS; s++)
    {
        pcm_wave[s].name       = "segapcm_slot";
        pcm_wave[s].bits       = 8;
        pcm_wave[s].channels   = 1;
        pcm_wave[s].frequency  = 32000.0f;
        pcm_wave[s].len        = 0;
        pcm_wave[s].loop_len   = 0;
        pcm_wave[s].start      = nullptr;
        pcm_wave[s].read       = pcm_voice_read;
        pcm_wave[s].ctx        = &pcm_slot[s];
        pcm_wave[s].state_size = 0;
        pcm_wave[s].__uuid     = 0;

        pcm_slot[s].voice        = -1;
        pcm_slot[s].base_byte    = 0;
        pcm_slot[s].last_used_us = 0;

        // Pin each channel to its actual upper bound (8-bit @ 32 kHz),
        // so the mixer doesn't allocate the default 16-bit-at-output-rate
        // sized sample buffer.
        mixer_ch_set_limits(s, 8, 32000.0f, 0);
    }

    for (int v = 0; v < N_PCM_CH; v++)
    {
        pcm_track[v].prev_flags86 = 1;      // "inactive" so first active
        pcm_track[v].prev_addr_lo = 0;      // edge triggers a play
        pcm_track[v].prev_addr_hi = 0;
        pcm_track[v].prev_end     = 0;
        pcm_track[v].mix_slot     = -1;
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

    debugf("audio: init rate=%d, mixer up with %d PCM-pool ch (%d HW voices) + %d/%d wav64 loaded\n",
           audio_get_frequency(), N_PCM_SLOTS, N_PCM_CH, n_wav64_ok, N_WAV64);
}

// See header comment. Plays a silent dummy waveform on each mixer channel
// so libdragon's mixer_ch_play allocates the lazy per-channel sample
// buffer up-front. Covers both the SegaPCM slot pool (N_PCM_SLOTS @ 8-bit
// mono) AND the wav64 stereo pairs (16-bit, 2 channels each) — without
// the wav64 prime the first jingle/SFX fire OOMs mid-game when only
// ~18 KiB remains. Buffer sizes are set by mixer_ch_set_limits in
// Audio::init; priming just realises them.
void Audio::prime_mixer_buffers()
{
    if (!dac_initialised) return;

    // Order matters under tight 4 MiB heap: alloc the two big stereo wav64
    // buffers (~11 KiB each, 16-byte aligned) *first*, while one contiguous
    // ~58 KiB free block still exists. The 8× ~4 KiB PCM-pool blocks slot
    // in afterward without trouble. The reverse order leaves the second
    // wav64 alloc looking for 11 KiB contiguous in a heap already split by
    // 8 pool blocks, and asserts inside mixer_ch_play.
    //
    // wav64 channels are primed with a real wav64 file (not a stub) so the
    // mixer allocates a sample buffer sized for the actual VADPCM
    // state_size (~48 B). A state_size=0 stub would trigger a realloc on
    // first real wav64_play, defeating the prime.
    int primed_mus = -1, primed_sfx = -1;
    for (int i = 0; i < N_WAV64 && (primed_mus < 0 || primed_sfx < 0); i++)
    {
        if (!wav64_loaded[i]) continue;
        if (WAV64_TABLE[i].is_music && primed_mus < 0)
        {
            wav64_play(&wav64_files[i], WAV64_MUS_CH);
            mixer_ch_stop(WAV64_MUS_CH);
            primed_mus = i;
        }
        else if (!WAV64_TABLE[i].is_music && primed_sfx < 0)
        {
            wav64_play(&wav64_files[i], WAV64_SFX_CH);
            mixer_ch_stop(WAV64_SFX_CH);
            primed_sfx = i;
        }
    }

    // SegaPCM pool — re-uses the per-slot waveform with read fn swapped.
    for (int s = 0; s < N_PCM_SLOTS; s++)
    {
        const WaveformRead real_read = pcm_wave[s].read;
        pcm_wave[s].read = prime_silent_read;
        pcm_wave[s].len  = 16;
        mixer_ch_play(s, &pcm_wave[s]);
        mixer_ch_stop(s);
        pcm_wave[s].read = real_read;
        pcm_wave[s].len  = 0;
        pcm_wave[s].__uuid = 0;  // force re-cache on real first play
    }

    debugf("audio: primed %d PCM-pool + wav64 mus=%d sfx=%d\n",
           N_PCM_SLOTS, primed_mus, primed_sfx);
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
