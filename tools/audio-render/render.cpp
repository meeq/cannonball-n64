// Offline renderer for OutRun YM2151 + SegaPCM sound IDs.
//
// Drives the same osoundint / ym2151 / segapcm code that ships in the N64
// build, but bypasses the realtime audio backend — we tick the simulation
// as fast as possible and dump int16 stereo straight to a WAV file.
//
// Usage:
//   audio-render <sound-id-hex> <duration-seconds> <out.wav>
//                [--rom-path PATH] [--rate HZ] [--no-pcm] [--no-ym]
//
// Examples:
//   audio-render 0x81 90 audio/raw/music_breeze.wav        # MUSIC_BREEZE
//   audio-render 0x84  1 audio/raw/coin_in.wav             # COIN_IN
//
// --no-pcm / --no-ym are useful when isolating one chip's output for
// FM SFX captures (which should be YM-only).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

#include "frontend/config.hpp"
#include "roms.hpp"
#include "engine/outrun.hpp"
#include "engine/audio/osoundint.hpp"
#include "engine/audio/commands.hpp"
#include "hwaudio/ym2151.hpp"
#include "hwaudio/segapcm.hpp"

#include "wav_writer.hpp"

namespace
{
    struct Options
    {
        uint32_t    sound_id    = 0;
        double      duration_s  = 1.0;
        std::string out_path;
        std::string rom_path    = "roms/";
        uint32_t    rate        = 44100;
        bool        mute_pcm    = false;
        bool        mute_ym     = false;
    };

    bool parse_args(int argc, char** argv, Options& opt)
    {
        if (argc < 4) return false;
        opt.sound_id   = (uint32_t)std::strtoul(argv[1], nullptr, 0);
        opt.duration_s = std::strtod(argv[2], nullptr);
        opt.out_path   = argv[3];
        for (int i = 4; i < argc; ++i)
        {
            std::string a = argv[i];
            if (a == "--rom-path" && i + 1 < argc) opt.rom_path = argv[++i];
            else if (a == "--rate" && i + 1 < argc) opt.rate = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
            else if (a == "--no-pcm") opt.mute_pcm = true;
            else if (a == "--no-ym")  opt.mute_ym  = true;
            else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return false; }
        }
        return true;
    }

    int16_t clip_i16(int32_t v)
    {
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return (int16_t)v;
    }
}

int main(int argc, char** argv)
{
    Options opt;
    if (!parse_args(argc, argv, opt))
    {
        std::fprintf(stderr,
            "usage: audio-render <sound-id-hex> <duration-seconds> <out.wav>\n"
            "                    [--rom-path PATH] [--rate HZ]\n"
            "                    [--no-pcm] [--no-ym]\n");
        return 1;
    }

    // Engine config fields the audio path actually reads.
    config.data.rom_path   = opt.rom_path;
    config.sound.rate      = (int)opt.rate;
    config.sound.advertise = 1;  // permit any ID via queue_sound
    config.sound.enabled   = 1;
    // 25 makes 125/fps and rate/fps both clean integers: 5 ticks of Z80 audio
    // (40 ms) per chunk against samples_per_chunk = rate/25 (also 40 ms at any
    // common rate divisible by 25). fps=60 silently truncated 125/60 = 2.083
    // to 2 — Z80 ran at 120 Hz instead of 125 Hz, so the rendered music drifted
    // 4.17% slow against its true tempo.
    config.fps             = 25;

    // Bypass attract-mode music filter — see osoundint::queue_sound.
    outrun.game_state = 7; // GS_MUSIC

    // Load the OutRun ROM set (Rev B with sample fixes off; matches the
    // N64 build's roms.load_revb_roms(false) path).
    if (!roms.load_revb_roms(false))
    {
        std::fprintf(stderr, "ROM load failed (check --rom-path)\n");
        return 2;
    }

    osoundint.pcm = nullptr;
    osoundint.ym  = nullptr;
    osoundint.init();
    osoundint.has_booted = true;

    // Number of (125 Hz audio) ticks per output buffer chunk. osoundint
    // ticks 125 Hz internally, and one chip stream_update() fills exactly
    // (rate/fps) stereo samples. With config.fps = 25 both quantities are
    // exact integers (5 ticks, rate/25 samples) and the long-run Z80 rate
    // matches the audio output rate.
    const int ticks_per_chunk = 125 / config.fps;
    const int samples_per_chunk = (int)(opt.rate / config.fps);  // stereo frames
    const int total_chunks = (int)(opt.duration_s * config.fps + 0.5);

    WavWriter wav;
    if (!wav.open(opt.out_path.c_str(), opt.rate, 2))
    {
        std::fprintf(stderr, "cannot open output: %s\n", opt.out_path.c_str());
        return 3;
    }

    // Queue the sound. queue_sound_service skips the attract-mode filter
    // entirely (vs queue_sound), which is what we want for renders.
    osoundint.queue_sound_service((uint8_t)opt.sound_id);

    std::printf("rendering id=0x%02X duration=%.2fs rate=%u → %s\n",
                opt.sound_id, opt.duration_s, opt.rate, opt.out_path.c_str());

    std::vector<int16_t> mix;
    mix.resize(samples_per_chunk * 2);

    for (int c = 0; c < total_chunks; ++c)
    {
        osoundint.advance(ticks_per_chunk);
        osoundint.pcm->stream_update();
        osoundint.ym->stream_update();

        const int16_t* pcm_buf = osoundint.pcm->get_buffer();
        const int16_t* ym_buf  = osoundint.ym->get_buffer();

        for (int i = 0; i < samples_per_chunk * 2; ++i)
        {
            int32_t sum = 0;
            if (!opt.mute_ym)  sum += ym_buf[i];
            if (!opt.mute_pcm) sum += pcm_buf[i];
            mix[i] = clip_i16(sum);
        }

        wav.append(mix.data(), mix.size());
    }

    wav.close();
    std::printf("wrote %d chunks (%d stereo samples)\n",
                total_chunks, total_chunks * samples_per_chunk);
    return 0;
}
