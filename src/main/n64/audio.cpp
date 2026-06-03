/***************************************************************************
    N64 Audio — Phase 4b (RSP mixer).

    Everything audible runs through libdragon's RSP audio mixer.

    SegaPCM voices map 1:1 onto mixer channels 0..15: each is a custom 8-bit
    mono waveform_t that streams bytes out of the (in-place-converted-to-
    signed) PCM ROM. Per-frame, reconcile_pcm() scans the SegaPCM register
    file via osoundint.pcm_ram and translates Z80 writes into mixer state
    (play/stop, freq, volume, loop).

    The YM2151 emulator still runs on the main CPU (no RSP FM ucode exists
    in libdragon), but its output rides the mixer too: channel 16+17 hold a
    16-bit stereo streaming waveform whose WaveformRead callback drains an
    intermediate ring topped up from ym->stream_update(). This lets the RSP
    do the final stereo sum + clip with the PCM voices and means Audio::tick
    is just mixer_try_play() — no CPU pass over the output buffer.

    SegaPCM::stream_update() is no longer called on N64; its 16-voice C++
    mix loop was the dominant chip-side CPU cost and the RSP path replaces
    it wholesale.
***************************************************************************/

#include "audio.hpp"

#include <libdragon.h>
#include <cstring>

#include "../engine/audio/osoundint.hpp"
#include "../frontend/config.hpp"
#include "../roms.hpp"
#include "../romloader.hpp"

namespace
{
    // ---- SegaPCM voice → mixer channel mapping --------------------------

    constexpr int N_PCM_CH   = 16;
    constexpr int YM_CH      = N_PCM_CH;        // channel 16
    constexpr int N_MIXER_CH = N_PCM_CH + 2;    // YM is stereo → uses 16+17

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

    void reconcile_pcm()
    {
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

                if (length > 0 && (int)(base + length) <= pcm_rom_len)
                {
                    pcm_ctx[ch].base_byte   = base;
                    pcm_wave[ch].len        = length;
                    pcm_wave[ch].loop_len   = loop_off ? 0 : length;

                    // Force the mixer to re-read len/loop_len from the
                    // waveform. mixer_ch_play's fast path keeps the cached
                    // channel state when uuid matches, which would mean
                    // playing the *previous* sample's length on a retrigger.
                    pcm_wave[ch].__uuid = 0;
                    mixer_ch_play(ch, &pcm_wave[ch]);
                    pcm_track[ch].playing = true;
                }
            }

            if (pcm_track[ch].playing)
            {
                // regs[7] is the per-sample increment at SegaPCM's native
                // 32 kHz. delta=256 means "advance one source byte per
                // 32 kHz tick" → playback rate = 32000 * delta/256.
                float freq = 32000.0f * (float)regs[7] / 256.0f;
                if (freq < 1.0f) freq = 1.0f;
                mixer_ch_set_freq(ch, freq);

                // SegaPCM's CPU mixer accumulates 16 voices of (int8*uint8)
                // into an int32 then clips. To keep peak amplitudes in the
                // same ballpark when the mixer sums signed PCM directly,
                // pre-attenuate by the channel count.
                float lvol = (float)regs[2] / 255.0f / (float)N_PCM_CH;
                float rvol = (float)regs[3] / 255.0f / (float)N_PCM_CH;
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

    // ---- YM2151 → mixer channel 16 (stereo) -----------------------------
    //
    // The YM2151 emulator produces frame_size = rate/fps stereo samples per
    // stream_update() call (e.g. 22050/60 = 367). The mixer's audio buffer
    // is rate/25 stereo samples (~880 at 22 kHz). A small ring decouples
    // the two cadences so the WaveformRead callback can satisfy whatever
    // wlen the mixer asks for without partial-frame bookkeeping.

    constexpr int YM_RING_CAP = 4096;       // stereo samples, must be pow2
    int16_t ym_ring[YM_RING_CAP * 2];       // interleaved L,R
    int     ym_ring_wpos  = 0;
    int     ym_ring_rpos  = 0;
    int     ym_ring_count = 0;

    waveform_t ym_wave;

    inline void ym_ring_push(int16_t l, int16_t r)
    {
        ym_ring[ym_ring_wpos*2 + 0] = l;
        ym_ring[ym_ring_wpos*2 + 1] = r;
        ym_ring_wpos = (ym_ring_wpos + 1) & (YM_RING_CAP - 1);
        ym_ring_count++;
    }

    inline void ym_ring_pop(int16_t& l, int16_t& r)
    {
        l = ym_ring[ym_ring_rpos*2 + 0];
        r = ym_ring[ym_ring_rpos*2 + 1];
        ym_ring_rpos = (ym_ring_rpos + 1) & (YM_RING_CAP - 1);
        ym_ring_count--;
    }

    void ym_voice_read(void* /*ctx*/, samplebuffer_t* sbuf, int /*wpos*/, int wlen, bool /*seeking*/)
    {
        // Top up the ring until we can satisfy this request, bounded so a
        // slow YM chip can't stall the mixer arbitrarily. Each stream_update
        // produces frame_size samples (rate/fps); at 22 kHz / 60 fps that's
        // 367 per call, so 3 calls (1101) covers a full ~880-frame audio
        // buffer plus carry-over for the next callback.
        constexpr int YM_BUDGET = 3;
        int pushed = 0;
        while (ym_ring_count < wlen && pushed < YM_BUDGET)
        {
            osoundint.ym->stream_update();
            const int entries = (int)osoundint.ym->buffer_size;
            const int samples = entries / 2;
            if (samples == 0) break;
            int16_t* src = osoundint.ym->get_buffer();
            for (int i = 0; i < samples; i++)
            {
                if (ym_ring_count == YM_RING_CAP) break;
                ym_ring_push(src[2*i + 0], src[2*i + 1]);
            }
            pushed++;
        }

        // samplebuffer is 16-bit stereo: each "sample" is one L/R frame,
        // 4 bytes wide. wlen is frames; we write wlen * 2 int16s.
        int16_t* dst = (int16_t*)samplebuffer_append(sbuf, wlen);
        const int copy = (ym_ring_count < wlen) ? ym_ring_count : wlen;
        for (int i = 0; i < copy; i++)
        {
            int16_t l, r;
            ym_ring_pop(l, r);
            dst[2*i + 0] = l;
            dst[2*i + 1] = r;
        }
        for (int i = copy; i < wlen; i++)
        {
            dst[2*i + 0] = 0;
            dst[2*i + 1] = 0;
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

    // SegaPCM samples ship unsigned-biased; the mixer needs signed PCM. We
    // convert the ROM in place since SegaPCM::stream_update() is never
    // called on N64 and nothing else reads roms.pcm.rom.
    if (!pcm_rom_signed && roms.pcm.loaded)
    {
        pcm_rom_len = (int)roms.pcm.length;
        for (int i = 0; i < pcm_rom_len; i++)
            roms.pcm.rom[i] = (uint8_t)(roms.pcm.rom[i] ^ 0x80);
        pcm_rom_signed = (int8_t*)roms.pcm.rom;
    }

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

    ym_ring_wpos  = 0;
    ym_ring_rpos  = 0;
    ym_ring_count = 0;

    ym_wave.name       = "ym2151";
    ym_wave.bits       = 16;
    ym_wave.channels   = 2;
    ym_wave.frequency  = (float)config.sound.rate;
    ym_wave.len        = WAVEFORM_UNKNOWN_LEN;
    ym_wave.loop_len   = 0;
    ym_wave.start      = nullptr;
    ym_wave.read       = ym_voice_read;
    ym_wave.ctx        = nullptr;
    ym_wave.state_size = 0;
    ym_wave.__uuid     = 0;

    // Pin the YM pair to its actual playback shape so the mixer doesn't
    // size sample buffers for a higher cap than we need.
    mixer_ch_set_limits(YM_CH, 16, (float)config.sound.rate, 0);
    mixer_ch_play(YM_CH, &ym_wave);
    mixer_ch_set_vol(YM_CH, 1.0f, 1.0f);

    z80_us_pending = 0.0;
    z80_last_us    = 0;

    sound_enabled = true;

    debugf("audio: init rate=%d, mixer up with %d PCM channels + YM stereo\n",
           audio_get_frequency(), N_PCM_CH);
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
    // Always advance the Z80 audio code on wall-clock, even before
    // sound_enabled flips on, so the chip register stream stays consistent
    // with the engine. Cheap when there are no pending ticks.
    advance_z80_audio();

    if (!sound_enabled) return;

    reconcile_pcm();

    const int blen = audio_get_buffer_length();
    if (blen <= 0) return;

    // Cap mixer_poll iterations per tick. mixer_try_play loops while
    // audio_can_write(), which fills every free buffer in one shot — and
    // when the queue is fully drained (e.g. after a track-load burst on the
    // music-select screen), each iteration runs the YM callback with up to
    // YM_BUDGET stream_updates: worst case 2*3=6 chip calls, capped.
    // Without the cap a single tick can run 16+ chip calls and block for
    // seconds. 2 polls × ~40 ms of audio per buffer = 80 ms of audio per
    // tick, enough to sustain the 25 buffers/sec consumption rate down to
    // ~12 fps tick rate before the queue underruns.
    constexpr int MAX_POLLS_PER_TICK = 2;
    int polls = 0;
    while (audio_can_write() && polls < MAX_POLLS_PER_TICK)
    {
        int16_t* buf = audio_write_begin();
        mixer_poll(buf, blen);
        audio_write_end();
        polls++;
    }
}
