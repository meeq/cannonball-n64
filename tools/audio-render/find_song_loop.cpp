// Find a music track's TRUE audio loop period by fingerprinting the full
// forward-evolving state of the audio simulation each frame.
//
// The rendered audio (what wav64 captures) is a deterministic function of
//   (chan_ram, pcm_ram, OSound aux counters, YM2151 chip state, SegaPCM low[])
// Once that tuple repeats at two different frames t0 < t1, every sample
// produced after t0 is bit-identical to the samples produced after t1, so
// splicing the rendered WAV at those points yields a glitch-free loop.
//
// An earlier version of this tool snapshotted only chan_ram + pcm_ram and
// reported "state cycles" of 7-60 s for the music tracks, but the audible
// loop was still glitchy because the YM2151 envelope generators, phase
// accumulators, and LFO were mid-evolution at the splice points. Adding
// the chip state is what makes the result authoritative.
//
// Stepping cadence:
//   We step at the audio chunk rate (config.fps Hz), matching render.cpp.
//   Each step advances `125/fps` Z80 ticks via osoundint.advance() and
//   then calls stream_update() on both chips. At fps=25 that's 5 Z80
//   ticks per audio frame (exact integer); the long-run Z80 rate matches
//   the audio output rate. Snapshots are taken AFTER stream_update() so
//   the chip state captured is the post-frame state.
//
// Usage:
//   find-song-loop <sound-id-hex> [--rom-path PATH] [--max-seconds N]
//                                 [--warmup-seconds N]
//
// Example:
//   find-song-loop 0x81 --rom-path roms/ --max-seconds 600

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

#include "frontend/config.hpp"
#include "roms.hpp"
#include "engine/outrun.hpp"
#include "engine/audio/osound.hpp"
#include "engine/audio/osoundint.hpp"
#include "hwaudio/ym2151.hpp"
#include "hwaudio/segapcm.hpp"

namespace
{
    constexpr size_t CHAN_BYTES = OSound::CHAN_RAM_BYTES;     // 0x800
    constexpr size_t PCM_BYTES  = OSoundInt::PCM_RAM_SIZE;    // 0x100
    constexpr size_t AUX_BYTES  = 2;   // (counters & 1 packed) + (sound_props & 3)
    constexpr size_t SEGAPCM_STATE_BYTES = SegaPCM::STATE_BYTES;

    uint64_t fnv1a(const uint8_t* p, size_t n)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= 0x100000001b3ull;
        }
        return h;
    }

    // Build a per-frame snapshot of everything that determines the audio
    // output from this frame onwards. Layout (in order):
    //   chan_ram  [0x800]   Z80 MML walker state
    //   pcm_ram   [0x100]   SegaPCM register file
    //   aux       [2]       OSound counter1..4 BIT_0s + sound_props bits 0/1
    //   ym2151    [~4 KB]   operators, EG, LFO, noise, timers, status, …
    //   segapcm   [16]      per-channel fractional read positions
    // See YM2151::state_view / SegaPCM::state_view for what's inside the
    // chip blobs.
    void snapshot(uint8_t* dst, size_t ym_bytes)
    {
        size_t off = 0;
        std::memcpy(dst + off, osound.chan_ram_view(), CHAN_BYTES);
        off += CHAN_BYTES;
        std::memcpy(dst + off, osoundint.pcm_ram, PCM_BYTES);
        off += PCM_BYTES;

        OSound::AuxState a = osound.aux_state();
        uint8_t packed = (uint8_t)((a.counter1 & 1)
                                   | ((a.counter2 & 1) << 1)
                                   | ((a.counter3 & 1) << 2)
                                   | ((a.counter4 & 1) << 3));
        dst[off++] = packed;
        dst[off++] = (uint8_t)(a.sound_props & 0x03);

        osoundint.ym->state_view(dst + off);
        off += ym_bytes;

        osoundint.pcm->state_view(dst + off);
        // off += SEGAPCM_STATE_BYTES;  // (caller knows the full size)
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr,
            "usage: find-song-loop <sound-id-hex> [--rom-path PATH]\n"
            "                      [--max-seconds N] [--warmup-seconds N]\n");
        return 1;
    }

    uint32_t    sound_id        = (uint32_t)std::strtoul(argv[1], nullptr, 0);
    std::string rom_path        = "roms/";
    double      max_seconds     = 600.0;   // 10 minutes upper bound
    double      warmup_seconds  = 0.0;     // Start snapshotting from t=0 by default

    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--rom-path" && i + 1 < argc) rom_path = argv[++i];
        else if (a == "--max-seconds" && i + 1 < argc) max_seconds = std::strtod(argv[++i], nullptr);
        else if (a == "--warmup-seconds" && i + 1 < argc) warmup_seconds = std::strtod(argv[++i], nullptr);
        else if (a == "--diff-period" && i + 1 < argc) ++i;  // handled later
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 1; }
    }

    config.data.rom_path   = rom_path;
    config.sound.rate      = 44100;         // match render.cpp
    config.sound.advertise = 1;
    config.sound.enabled   = 1;
    config.fps             = 25;            // 125/25 = 5 ticks/frame, exact
    outrun.game_state      = 7;             // GS_MUSIC

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

    // Now that the chips are allocated, compute final snapshot size.
    const size_t YM_BYTES   = YM2151::state_bytes();
    const size_t SNAP_BYTES = CHAN_BYTES + PCM_BYTES + AUX_BYTES
                            + YM_BYTES + SEGAPCM_STATE_BYTES;

    const int ticks_per_frame = 125 / config.fps;        // 5 at fps=25
    const int max_frames      = (int)(max_seconds    * config.fps + 0.5);
    const int warmup_frames   = (int)(warmup_seconds * config.fps + 0.5);

    std::printf("searching for audio loop  id=0x%02X  max=%.0fs  warmup=%.0fs\n",
                sound_id, max_seconds, warmup_seconds);
    std::printf("snapshot = chan_ram %zu + pcm_ram %zu + aux %zu"
                " + ym2151 %zu + segapcm %zu = %zu bytes\n",
                CHAN_BYTES, PCM_BYTES, AUX_BYTES,
                YM_BYTES, SEGAPCM_STATE_BYTES, SNAP_BYTES);
    std::printf("step = 1 audio frame at %d fps (%d Z80 ticks + stream_update)\n",
                config.fps, ticks_per_frame);

    // Stream snapshots into a flat byte buffer + parallel frame index, with
    // a hash -> snapshot-index multimap for O(1) lookup. Memory budget:
    //   max_frames * (SNAP_BYTES + ~24 bytes overhead).
    // At 600 s × 25 fps × ~6.5 KB = ~95 MB raw snapshots — fine for a
    // host tool. Reserve up-front so we don't pay vector-grow copies.
    std::vector<uint8_t>  snap_buf;
    std::vector<uint64_t> snap_frame;
    std::unordered_multimap<uint64_t, size_t> by_hash;
    snap_buf.reserve(static_cast<size_t>(max_frames) * SNAP_BYTES);
    snap_frame.reserve(max_frames);
    by_hash.reserve(max_frames);

    std::vector<uint8_t> cur(SNAP_BYTES);
    bool found = false;
    uint64_t loop_start_frame = 0;
    uint64_t loop_period_frames = 0;

    for (int f = 1; f <= max_frames && !found; ++f)
    {
        osoundint.advance(ticks_per_frame);
        osoundint.pcm->stream_update();
        osoundint.ym->stream_update();

        if (f < warmup_frames) continue;

        snapshot(cur.data(), YM_BYTES);
        uint64_t h = fnv1a(cur.data(), SNAP_BYTES);

        auto range = by_hash.equal_range(h);
        for (auto it = range.first; it != range.second; ++it)
        {
            size_t idx = it->second;
            const uint8_t* prev = snap_buf.data() + idx * SNAP_BYTES;
            if (std::memcmp(prev, cur.data(), SNAP_BYTES) == 0)
            {
                loop_start_frame   = snap_frame[idx];
                loop_period_frames = (uint64_t)f - loop_start_frame;
                found = true;
                break;
            }
        }

        if (!found)
        {
            by_hash.emplace(h, snap_frame.size());
            snap_frame.push_back((uint64_t)f);
            snap_buf.insert(snap_buf.end(), cur.begin(), cur.end());
        }
    }

    const double frame_s = 1.0 / config.fps;   // 0.04 s at fps=25

    if (!found)
    {
        std::printf("\nNO LOOP FOUND within %.0f s — state never repeats.\n",
                    max_seconds);
        std::printf("captured %zu snapshots, %.1f MB\n",
                    snap_frame.size(),
                    (snap_buf.size() / (1024.0 * 1024.0)));

        // Diagnose: dump which bytes differ between a mid-song snapshot and
        // a later one EXACTLY N seconds apart. Bytes that stay equal across
        // many frames are stable; those that differ are the drift sources
        // keeping the state from exactly repeating.
        double diff_period = 7.68;
        for (int i = 2; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--diff-period" && i + 1 < argc)
                diff_period = std::strtod(argv[i + 1], nullptr);
        }
        if (snap_frame.size() >= 4)
        {
            size_t a_idx = snap_frame.size() / 4;
            size_t b_idx = a_idx + (size_t)(diff_period * config.fps + 0.5);
            if (b_idx >= snap_frame.size()) b_idx = snap_frame.size() - 1;
            const uint8_t* a = snap_buf.data() + a_idx * SNAP_BYTES;
            const uint8_t* b = snap_buf.data() + b_idx * SNAP_BYTES;
            std::printf("\nbyte-level diff between t=%.3fs and t=%.3fs"
                        " (%.3f s apart):\n",
                        snap_frame[a_idx] * frame_s,
                        snap_frame[b_idx] * frame_s,
                        (snap_frame[b_idx] - snap_frame[a_idx]) * frame_s);
            int diffs = 0;
            for (size_t i = 0; i < SNAP_BYTES; ++i)
            {
                if (a[i] != b[i])
                {
                    const char* region;
                    size_t off;
                    if (i < CHAN_BYTES) {
                        region = "chan_ram"; off = i;
                    } else if (i < CHAN_BYTES + PCM_BYTES) {
                        region = "pcm_ram "; off = i - CHAN_BYTES;
                    } else if (i < CHAN_BYTES + PCM_BYTES + AUX_BYTES) {
                        region = "aux     "; off = i - CHAN_BYTES - PCM_BYTES;
                    } else if (i < CHAN_BYTES + PCM_BYTES + AUX_BYTES + YM_BYTES) {
                        region = "ym2151  ";
                        off = i - CHAN_BYTES - PCM_BYTES - AUX_BYTES;
                    } else {
                        region = "segapcm ";
                        off = i - CHAN_BYTES - PCM_BYTES - AUX_BYTES - YM_BYTES;
                    }
                    std::printf("  %s[0x%04zx] = 0x%02x vs 0x%02x   delta=%+d\n",
                                region, off, a[i], b[i], (int)b[i] - (int)a[i]);
                    if (++diffs >= 64) { std::printf("  ... (more)\n"); break; }
                }
            }
        }
        return 3;
    }

    double intro_s  = loop_start_frame   * frame_s;
    double period_s = loop_period_frames * frame_s;

    std::printf("\nFOUND: state at frame %llu (%.6fs) matches frame %llu (%.6fs)\n",
                (unsigned long long)(loop_start_frame + loop_period_frames),
                (loop_start_frame + loop_period_frames) * frame_s,
                (unsigned long long)loop_start_frame,
                intro_s);
    std::printf("\n");
    std::printf("intro_seconds  = %.6f\n", intro_s);
    std::printf("period_seconds = %.6f\n", period_s);
    std::printf("intro_frames   = %llu\n", (unsigned long long)loop_start_frame);
    std::printf("period_frames  = %llu\n", (unsigned long long)loop_period_frames);
    return 0;
}
