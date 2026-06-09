// Find a music track's best loop point by combining structural and acoustic
// criteria.
//
// The audio output of the OutRun audio simulator is determined by two
// state buckets that evolve at very different rates:
//
//   MUSIC state (chan_ram + pcm_ram + aux): the Z80 MML walker's program
//     state and the SegaPCM register file. This is what the song author
//     controls. It returns to the same value every N seconds — that's the
//     STRUCTURAL period (typically 7..60 s for OutRun tracks).
//
//   AUDIO state (YM2151 internals + SegaPCM read positions): operator
//     phase accumulators, envelope generators, LFO, noise RNG, fractional
//     PCM read positions. Driven by the chip clock, NOT by the MML script.
//     Drifts incommensurately with the structural period — typically never
//     returns to the same value within any practical window.
//
// Two earlier tools failed at extremes of this:
//
//   find-song-loop (full state) — requires both buckets to match exactly.
//     Music state matches every N seconds, but audio state never does, so
//     no loop is ever found.
//
//   measure-splice (audio seam) — ignores the music state entirely; only
//     scores the immediate sample-step at the splice. Picks loops that
//     happen to be quiet at the seam but cut across musical phrases, so
//     the wrapped audio replays the wrong bar.
//
// This tool reconciles them: require the MUSIC state to match exactly
// (so the song wraps to a true structural boundary, never mid-phrase),
// then among candidate (intro, period) pairs that satisfy that, pick the
// one with the smallest AUDIO state delta (so the YM2151 envelopes/LFO
// at the splice are as close as possible to the wrap target).
//
// Usage:
//   find-music-loop <sound-id-hex> [--rom-path PATH] [--max-seconds N]
//                                  [--top K] [--min-period-s S]
//
// Example:
//   find-music-loop 0x85 --max-seconds 1200          # MAGICAL, 20 minutes

#include <algorithm>
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
    constexpr size_t AUX_BYTES  = 2;
    // pcm_ram is excluded from the music-state hash: SegaPCM stores its
    // per-channel current playhead address (regs[0x84]/[0x85]) inside that
    // buffer, which advances every audio sample. Those bytes only return
    // to a prior value when ALL 16 PCM channels' playheads coincidentally
    // realign — which happens at LCM-of-drum-sample-lengths periods, far
    // longer than the structural musical period. The MML walker's own
    // state is chan_ram alone; the drum-trigger timing is determined by
    // chan_ram, and re-triggering a PCM voice resets its playhead anyway.
    constexpr size_t CHAN_COUNT = CHAN_BYTES / 0x20; // 64 channel slots

    uint64_t fnv1a(const uint8_t* p, size_t n)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
        return h;
    }

    // Per-channel bytes that count as "musically visible" state. Other
    // bytes (CALL stack, finite-LOOP counters, MEM_OFFSET, etc.) reflect
    // which iteration of an inner LOOP a channel is on — that controls
    // which variation of a phrase plays next, but the moment-to-moment
    // audio is the same melody/rhythm/volume.
    //
    // Including them in the hash forces us to wait until ALL channels'
    // finite-LOOP counters happen to coincide simultaneously, which can
    // be 290+ seconds in for OutRun music (because each channel has its
    // own multi-iteration outer LOOP). Excluding them lets the hash
    // match at the much shorter audible loop period.
    constexpr size_t CHAN_SIZE = 0x20;
    constexpr uint8_t MASK_OFFSETS[] = {
        0x00, // FLAGS
        0x01, // FM_FLAGS
        0x02, // END_MARKER
        0x03, 0x04, // SEQ_POS (current note countdown)
        0x05, 0x06, // SEQ_END (current note duration)
        0x07, 0x08, // SEQ_CMD (MML program counter)
        0x09, // NOTE_OFFSET
        0x0B, // FM_PHASETBL
        0x0C, // FM_BLOCK
        0x0D, // FM_MARKER
        0x0E, // COMMAND
        0x10, // FM_PHASEOFF
        0x11, 0x12, // VOL_L, VOL_R
        0x13, 0x14, 0x15, // PCM/FM note/phase
        0x16, // PCM_PITCH
        0x17, // CTRL (panning)
        // INTENTIONALLY EXCLUDED (drift slowly across cycles):
        //   0x0A MEM_OFFSET (per-channel stack pointer)
        //   0x0F UNKNOWN
        //   0x18 FM_LOOP (finite-LOOP iteration counter)
        //   0x19 ?
        //   0x1A,0x1B,0x1C,0x1D SEQ_ADR1 (call return slot, can be unused)
        //   0x1E,0x1F SEQ_ADR2 (call return slot, populated only after first CALL)
    };
    constexpr size_t MASK_PER_CHAN = sizeof(MASK_OFFSETS);
    constexpr size_t MUSIC_BYTES = CHAN_COUNT * MASK_PER_CHAN + AUX_BYTES;

    void snapshot_music(uint8_t* dst)
    {
        const uint8_t* ram = osound.chan_ram_view();
        size_t off = 0;
        for (size_t ch = 0; ch < CHAN_BYTES / CHAN_SIZE; ++ch)
        {
            const uint8_t* base = ram + ch * CHAN_SIZE;
            for (uint8_t off_in_chan : MASK_OFFSETS)
                dst[off++] = base[off_in_chan];
        }

        OSound::AuxState a = osound.aux_state();
        dst[off++] = (uint8_t)((a.counter1 & 1)
                              | ((a.counter2 & 1) << 1)
                              | ((a.counter3 & 1) << 2)
                              | ((a.counter4 & 1) << 3));
        dst[off++] = (uint8_t)(a.sound_props & 0x03);
    }

    // Sum-of-absolute-differences between two equal-length byte buffers.
    uint64_t sad(const uint8_t* a, const uint8_t* b, size_t n)
    {
        uint64_t s = 0;
        for (size_t i = 0; i < n; ++i)
        {
            int d = (int)a[i] - (int)b[i];
            s += (uint64_t)(d < 0 ? -d : d);
        }
        return s;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr,
            "usage: find-music-loop <sound-id-hex> [--rom-path PATH]\n"
            "                       [--max-seconds N] [--top K]\n"
            "                       [--min-period-s S]\n");
        return 1;
    }

    uint32_t    sound_id      = (uint32_t)std::strtoul(argv[1], nullptr, 0);
    std::string rom_path      = "roms/";
    double      max_seconds   = 600.0;
    double      min_period_s  = 1.0;     // Reject sub-second false matches
    int         top_k         = 8;
    double      delta_budget  = -1.0;    // Max acceptable delta/byte for compact ranking

    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if      (a == "--rom-path"   && i + 1 < argc) rom_path     = argv[++i];
        else if (a == "--max-seconds"&& i + 1 < argc) max_seconds  = std::strtod(argv[++i], nullptr);
        else if (a == "--top"        && i + 1 < argc) top_k        = (int)std::strtol(argv[++i], nullptr, 0);
        else if (a == "--min-period-s" && i + 1 < argc) min_period_s = std::strtod(argv[++i], nullptr);
        else if (a == "--delta-budget"&& i + 1 < argc) delta_budget = std::strtod(argv[++i], nullptr);
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 1; }
    }

    config.data.rom_path   = rom_path;
    config.sound.rate      = 44100;
    config.sound.advertise = 1;
    config.sound.enabled   = 1;
    config.fps             = 25;             // 125/25 = 5 ticks/frame, exact
    outrun.game_state      = 7;              // GS_MUSIC

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

    const size_t YM_BYTES    = YM2151::state_bytes();
    const size_t PCM_ST_BYTES= SegaPCM::STATE_BYTES;
    const size_t AUDIO_BYTES = YM_BYTES + PCM_ST_BYTES;

    const int ticks_per_frame = 125 / config.fps;
    const int max_frames      = (int)(max_seconds * config.fps + 0.5);
    const int min_period_fr   = std::max(1, (int)(min_period_s * config.fps + 0.5));

    std::printf("find-music-loop  id=0x%02X  max=%.0fs  fps=%d  min_period=%.2fs\n",
                sound_id, max_seconds, config.fps, min_period_s);
    std::printf("music snapshot = %zu bytes (%zu masked chan bytes * %zu chans + aux %zu)\n",
                MUSIC_BYTES, MASK_PER_CHAN, CHAN_COUNT, AUX_BYTES);
    std::printf("audio snapshot = %zu bytes (ym2151 %zu + segapcm %zu)\n",
                AUDIO_BYTES, YM_BYTES, PCM_ST_BYTES);
    std::printf("memory estimate ≈ %.1f MB for %d frames\n",
                max_frames * (double)(MUSIC_BYTES + AUDIO_BYTES) / (1024 * 1024),
                max_frames);

    // Frame-indexed flat buffers (avoid vector<vector<>> overhead).
    std::vector<uint8_t>  music_buf;
    std::vector<uint8_t>  audio_buf;
    music_buf.reserve((size_t)max_frames * MUSIC_BYTES);
    audio_buf.reserve((size_t)max_frames * AUDIO_BYTES);

    // music_hash -> list of frame indices with that hash.
    std::unordered_map<uint64_t, std::vector<int>> by_music_hash;
    by_music_hash.reserve(max_frames);

    std::vector<uint8_t> cur_music(MUSIC_BYTES);
    std::vector<uint8_t> cur_audio(AUDIO_BYTES);

    // Track best candidates: (audio_delta, intro_frame, period_frames).
    struct Cand { uint64_t delta; int intro; int period; };
    std::vector<Cand> cands;

    int frames_logged = 0;
    int collisions    = 0;

    for (int f = 0; f < max_frames; ++f)
    {
        osoundint.advance(ticks_per_frame);
        osoundint.pcm->stream_update();
        osoundint.ym->stream_update();

        snapshot_music(cur_music.data());
        osoundint.ym->state_view(cur_audio.data());
        osoundint.pcm->state_view(cur_audio.data() + YM_BYTES);

        uint64_t h = fnv1a(cur_music.data(), MUSIC_BYTES);

        auto it = by_music_hash.find(h);
        if (it != by_music_hash.end())
        {
            // Music hash collided — verify byte-exact match against each
            // prior frame in this bucket and score audio drift.
            for (int prev_f : it->second)
            {
                int period = f - prev_f;
                if (period < min_period_fr) continue;
                const uint8_t* prev_music = music_buf.data() + (size_t)prev_f * MUSIC_BYTES;
                if (std::memcmp(prev_music, cur_music.data(), MUSIC_BYTES) != 0)
                    continue;  // hash collision but not equal

                ++collisions;
                const uint8_t* prev_audio = audio_buf.data() + (size_t)prev_f * AUDIO_BYTES;
                uint64_t delta = sad(prev_audio, cur_audio.data(), AUDIO_BYTES);
                cands.push_back({delta, prev_f, period});
            }
        }

        by_music_hash[h].push_back(f);
        music_buf.insert(music_buf.end(), cur_music.begin(), cur_music.end());
        audio_buf.insert(audio_buf.end(), cur_audio.begin(), cur_audio.end());
        ++frames_logged;
    }

    const double frame_s = 1.0 / config.fps;

    if (cands.empty())
    {
        std::printf("\nNO MUSIC-STATE REPEAT FOUND within %.0f s.\n", max_seconds);
        std::printf("captured %d frames. The Z80 MML script never returned to a prior\n",
                    frames_logged);
        std::printf("state — the track may be a one-shot, or have a structural period\n");
        std::printf("longer than the search window.\n");
        return 3;
    }

    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b)
              {
                  // Prefer smaller audio delta. Tie-break: smaller period (earliest
                  // structural alignment), then earliest intro.
                  if (a.delta != b.delta) return a.delta < b.delta;
                  if (a.period != b.period) return a.period < b.period;
                  return a.intro < b.intro;
              });

    std::printf("\ncaptured %d frames, %d music-state collisions, %zu candidates\n",
                frames_logged, collisions, cands.size());

    std::printf("\ntop %d candidates ranked by audio-state delta\n",
                std::min(top_k, (int)cands.size()));
    std::printf("(SAD over %zu bytes of YM+SegaPCM internal state; 0 = bit-exact)\n\n",
                AUDIO_BYTES);
    std::printf("%4s  %12s  %12s  %14s  %14s  %s\n",
                "rank", "audio_delta", "delta/byte", "intro(s)", "period(s)", "intro_samp@44100");
    std::printf("----  ------------  ------------  --------------  --------------  ----------------\n");

    int shown = std::min(top_k, (int)cands.size());
    for (int i = 0; i < shown; ++i)
    {
        const auto& c = cands[i];
        double intro_s  = c.intro  * frame_s;
        double period_s = c.period * frame_s;
        double per_byte = (double)c.delta / (double)AUDIO_BYTES;
        long intro_samp = (long)(intro_s * 44100 + 0.5);
        std::printf("%4d  %12llu  %12.4f  %14.6f  %14.6f  %16ld\n",
                    i, (unsigned long long)c.delta, per_byte,
                    intro_s, period_s, intro_samp);
    }

    // Compact ranking: among candidates whose delta/byte is within budget of
    // the best, prefer the smallest total wav64 length (intro + period). Lets
    // us spend a few extra SAD/byte to halve the ROM footprint.
    if (delta_budget < 0)
        delta_budget = (double)cands[0].delta / AUDIO_BYTES + 0.05;
    uint64_t delta_cap = (uint64_t)(delta_budget * AUDIO_BYTES + 0.5);

    std::vector<Cand> compact;
    compact.reserve(cands.size());
    for (const auto& c : cands)
        if (c.delta <= delta_cap) compact.push_back(c);
    std::sort(compact.begin(), compact.end(),
              [](const Cand& a, const Cand& b)
              {
                  int sa = a.intro + a.period;
                  int sb = b.intro + b.period;
                  if (sa != sb) return sa < sb;
                  return a.delta < b.delta;
              });

    std::printf("\ncompact ranking (delta/byte <= %.4f), prefer smaller intro+period:\n",
                delta_budget);
    std::printf("%4s  %12s  %12s  %14s  %14s  %12s\n",
                "rank", "audio_delta", "delta/byte", "intro(s)", "period(s)", "total(s)");
    std::printf("----  ------------  ------------  --------------  --------------  ------------\n");
    int compact_shown = std::min(top_k, (int)compact.size());
    for (int i = 0; i < compact_shown; ++i)
    {
        const auto& c = compact[i];
        double intro_s  = c.intro  * frame_s;
        double period_s = c.period * frame_s;
        double per_byte = (double)c.delta / (double)AUDIO_BYTES;
        std::printf("%4d  %12llu  %12.4f  %14.6f  %14.6f  %12.3f\n",
                    i, (unsigned long long)c.delta, per_byte,
                    intro_s, period_s, intro_s + period_s);
    }

    // Earliest-intro ranking: just sort by intro frame ascending, regardless
    // of delta. Shows what the smallest possible (intro, period) is for any
    // valid chan_ram match — useful for picking the most compact loop when
    // some audible YM drift is acceptable. Filter to the smallest period
    // observed, since longer periods only inflate the file.
    int smallest_period_fr = INT32_MAX;
    for (const auto& c : cands) if (c.period < smallest_period_fr) smallest_period_fr = c.period;

    std::vector<Cand> earliest;
    for (const auto& c : cands)
        if (c.period == smallest_period_fr) earliest.push_back(c);
    std::sort(earliest.begin(), earliest.end(),
              [](const Cand& a, const Cand& b)
              {
                  if (a.intro != b.intro) return a.intro < b.intro;
                  return a.delta < b.delta;
              });

    std::printf("\nearliest-intro ranking at smallest period (%.6fs):\n",
                smallest_period_fr * frame_s);
    std::printf("%4s  %12s  %12s  %14s  %14s\n",
                "rank", "audio_delta", "delta/byte", "intro(s)", "intro_samp@44100");
    std::printf("----  ------------  ------------  --------------  ----------------\n");
    int earliest_shown = std::min(top_k, (int)earliest.size());
    for (int i = 0; i < earliest_shown; ++i)
    {
        const auto& c = earliest[i];
        double intro_s = c.intro * frame_s;
        double per_byte = (double)c.delta / (double)AUDIO_BYTES;
        long intro_samp = (long)(intro_s * 44100 + 0.5);
        std::printf("%4d  %12llu  %12.4f  %14.6f  %16ld\n",
                    i, (unsigned long long)c.delta, per_byte,
                    intro_s, intro_samp);
    }

    const auto& best = cands[0];
    std::printf("\nBEST:\n");
    std::printf("intro_seconds  = %.6f\n", best.intro  * frame_s);
    std::printf("period_seconds = %.6f\n", best.period * frame_s);
    std::printf("audio_delta    = %llu (%.4f/byte over %zu bytes)\n",
                (unsigned long long)best.delta,
                (double)best.delta / AUDIO_BYTES, AUDIO_BYTES);
    if (best.delta == 0)
        std::printf("PERFECT: full YM+SegaPCM state matches — loop is bit-exact.\n");
    else
    {
        // Dump which bytes drift the most, so we can judge audibility.
        // The YM2151 state ordering is documented in YM2151::state_view.
        const uint8_t* a = audio_buf.data() + (size_t)best.intro              * AUDIO_BYTES;
        const uint8_t* b = audio_buf.data() + (size_t)(best.intro+best.period)* AUDIO_BYTES;
        struct Diff { size_t off; int delta; };
        std::vector<Diff> diffs;
        diffs.reserve(64);
        for (size_t i = 0; i < AUDIO_BYTES; ++i)
            if (a[i] != b[i]) diffs.push_back({i, (int)b[i] - (int)a[i]});
        std::sort(diffs.begin(), diffs.end(),
                  [](const Diff& x, const Diff& y) { return std::abs(x.delta) > std::abs(y.delta); });

        std::printf("\nresidual byte deltas at best splice (%zu bytes differ):\n", diffs.size());
        int shown_diff = (int)std::min((size_t)32, diffs.size());
        for (int i = 0; i < shown_diff; ++i)
        {
            const char* region;
            size_t off;
            if (diffs[i].off < YM_BYTES) { region = "ym2151"; off = diffs[i].off; }
            else { region = "segapcm"; off = diffs[i].off - YM_BYTES; }
            std::printf("  %s[0x%04zx] = 0x%02x vs 0x%02x   delta=%+d\n",
                        region, off, a[diffs[i].off], b[diffs[i].off], diffs[i].delta);
        }
    }

    return 0;
}
