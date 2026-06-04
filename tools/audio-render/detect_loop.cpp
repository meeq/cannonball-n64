// Per-music-track loop length detector.
//
// Drives the OSound simulator just like audio-render does, but instead of
// dumping audio we poll OSound::loop_fires after each 125 Hz tick to spot
// the first two times each channel processes the LOOP_FOREVER MML opcode.
// The wall-clock distance between those two fires == the loop length in
// 125 Hz ticks, which we report in seconds.
//
// Usage:
//   detect-loop <sound-id-hex> [--rom-path PATH] [--max-seconds N]
//
// Example:
//   detect-loop 0x81 --rom-path roms/     # MUSIC_BREEZE

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "frontend/config.hpp"
#include "roms.hpp"
#include "engine/outrun.hpp"
#include "engine/audio/osound.hpp"
#include "engine/audio/osoundint.hpp"

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr,
            "usage: detect-loop <sound-id-hex> [--rom-path PATH] [--max-seconds N]\n");
        return 1;
    }

    uint32_t    sound_id    = (uint32_t)std::strtoul(argv[1], nullptr, 0);
    std::string rom_path    = "roms/";
    double      max_seconds = 180.0;

    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--rom-path" && i + 1 < argc) rom_path = argv[++i];
        else if (a == "--max-seconds" && i + 1 < argc) max_seconds = std::strtod(argv[++i], nullptr);
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 1; }
    }

    config.data.rom_path   = rom_path;
    config.sound.rate      = 22050;
    config.sound.advertise = 1;
    config.sound.enabled   = 1;
    config.fps             = 60;
    outrun.game_state      = 7; // GS_MUSIC, bypass attract music gate

    if (!roms.load_revb_roms(false))
    {
        std::fprintf(stderr, "ROM load failed (check --rom-path)\n");
        return 2;
    }

    osoundint.pcm = nullptr;
    osoundint.ym  = nullptr;
    osoundint.init();
    osoundint.has_booted = true;

    osoundint.queue_sound_service((uint8_t)sound_id);

    // OSound ticks at 125 Hz; one render "frame" advances ticks_per_frame ticks.
    // Step one tick at a time so we can snapshot loop_fires precisely.
    const int    ticks_per_frame = (int)(125.0 / config.fps + 0.5);
    const int    max_ticks       = (int)(max_seconds * 125.0 + 0.5);

    // Track tick numbers of the first two LOOP_FOREVER fires per channel.
    uint64_t first_tick[16] = {0};
    uint64_t second_tick[16] = {0};
    uint32_t prev_fires[16] = {0};

    std::printf("detecting loops for sound id=0x%02X (max %.0fs)...\n",
                sound_id, max_seconds);

    // Advance one frame of OSound work at a time. process_command runs
    // inside osoundint.advance via play_queued_sound + osound.tick, so the
    // sound is dispatched on the very first tick.
    int tick = 0;
    bool any_loops = true;
    while (tick < max_ticks)
    {
        osoundint.advance(ticks_per_frame);
        tick += ticks_per_frame;

        for (int ch = 0; ch < 16; ++ch)
        {
            uint32_t now = osound.loop_fires[ch];
            if (now > prev_fires[ch])
            {
                if (prev_fires[ch] == 0 && first_tick[ch] == 0)
                    first_tick[ch] = (uint64_t)tick;
                else if (now == 2 && second_tick[ch] == 0)
                    second_tick[ch] = (uint64_t)tick;
                prev_fires[ch] = now;
            }
        }

        // Bail early once every YM channel (0..7) that has fired at least
        // once has also fired a second time. PCM_DRUM channels (8..13) tick
        // at the drum-beat rate (often sub-second), which is musically
        // uninteresting — we want the longer YM-channel period that
        // corresponds to the actual musical loop.
        bool any_ym = false;
        bool all_ym_done = true;
        for (int ch = 0; ch < 8; ++ch)
        {
            if (first_tick[ch] != 0)
            {
                any_ym = true;
                if (second_tick[ch] == 0) { all_ym_done = false; break; }
            }
        }
        if (any_ym && all_ym_done) break;
        (void)any_loops;
    }

    static const char* names[16] = {
        "YM1","YM2","YM3","YM4","YM5","YM6","YM7","YM8",
        "PCM_DRUM1","PCM_DRUM2","PCM_DRUM3","PCM_DRUM4","PCM_DRUM5","PCM_DRUM6",
        "?14","?15"
    };

    std::printf("\n%-12s %10s %10s %10s\n", "channel", "1st(s)", "2nd(s)", "period(s)");
    std::printf("------------ ---------- ---------- ----------\n");
    double max_period = 0.0;
    for (int ch = 0; ch < 16; ++ch)
    {
        if (first_tick[ch] == 0) continue;
        double t1 = first_tick[ch]  / 125.0;
        double t2 = second_tick[ch] / 125.0;
        double dt = t2 - t1;
        if (dt > max_period) max_period = dt;
        std::printf("%-12s %10.3f %10.3f %10.3f%s\n",
            names[ch], t1, t2, dt,
            second_tick[ch] == 0 ? "  (no 2nd fire within window)" : "");
    }

    if (max_period > 0)
    {
        std::printf("\nmaster loop period: %.3f s\n", max_period);
        std::printf("recommended render duration: %.3f s\n", max_period);
    }
    else
    {
        std::printf("\nno LOOP_FOREVER fires detected — track may be one-shot\n");
    }

    return 0;
}
