// Diagnostic: identify which bytes of chan_ram are TRULY periodic at the
// suspected musical period vs which drift slowly across cycles.
//
// Compares chan_ram[t] vs chan_ram[t+period] at two points: an "early"
// time (t = early_s) and a "late" time (t = late_s). For each byte:
//   * If always equal at both early+period and late+period: stable. Always
//     part of the musical loop.
//   * If equal only at late+period (not at early+period): drifts initially,
//     stabilises later. These bytes contain slow-evolving state that
//     could probably be masked out to find a shorter intro.
//   * If never equal: drifts every cycle, never returns. Definitely needs
//     masking.
//
// Usage: diagnose-chan-ram <id> [--rom-path PATH] [--period S]
//                              [--early S] [--late S]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "frontend/config.hpp"
#include "roms.hpp"
#include "engine/outrun.hpp"
#include "engine/audio/osound.hpp"
#include "engine/audio/osoundint.hpp"
#include "hwaudio/ym2151.hpp"
#include "hwaudio/segapcm.hpp"

namespace { constexpr size_t CHAN_BYTES = OSound::CHAN_RAM_BYTES; }

static const char* field_name(size_t off_in_chan)
{
    switch (off_in_chan) {
        case 0x00: return "FLAGS"; case 0x01: return "FM_FLAGS";
        case 0x02: return "END_MARKER";
        case 0x03: case 0x04: return "SEQ_POS";
        case 0x05: case 0x06: return "SEQ_END";
        case 0x07: case 0x08: return "SEQ_CMD";
        case 0x09: return "NOTE_OFFSET"; case 0x0A: return "MEM_OFFSET";
        case 0x0B: return "FM_PHASETBL"; case 0x0C: return "FM_BLOCK";
        case 0x0D: return "FM_MARKER";   case 0x0E: return "COMMAND";
        case 0x0F: return "UNKNOWN";     case 0x10: return "FM_PHASEOFF";
        case 0x11: return "VOL_L";       case 0x12: return "VOL_R";
        case 0x13: return "PCM_ADR1L/FM_NOTE";
        case 0x14: return "PCM_ADR1H/FM_PHASE_AMP";
        case 0x15: return "PCM_ADR2";    case 0x16: return "PCM_PITCH";
        case 0x17: return "CTRL";        case 0x18: return "FM_LOOP";
        case 0x1C: case 0x1D: return "SEQ_ADR1";
        case 0x1E: case 0x1F: return "SEQ_ADR2";
        default: return "?";
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: diagnose-chan-ram <id> [--rom-path PATH] [--period S]\n"
            "                             [--early S] [--late S]\n"
            "                             [--max-seconds N]\n");
        return 1;
    }
    uint32_t    sound_id = (uint32_t)std::strtoul(argv[1], nullptr, 0);
    std::string rom_path = "roms/";
    double      period_s = 30.72;
    double      early_s  = 40.0;
    double      late_s   = 300.0;
    double      max_s    = 400.0;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--rom-path"   && i+1 < argc) rom_path = argv[++i];
        else if (a == "--period"     && i+1 < argc) period_s = std::strtod(argv[++i], nullptr);
        else if (a == "--early"      && i+1 < argc) early_s  = std::strtod(argv[++i], nullptr);
        else if (a == "--late"       && i+1 < argc) late_s   = std::strtod(argv[++i], nullptr);
        else if (a == "--max-seconds"&& i+1 < argc) max_s    = std::strtod(argv[++i], nullptr);
    }

    config.data.rom_path   = rom_path;
    config.sound.rate      = 44100;
    config.sound.advertise = 1;
    config.sound.enabled   = 1;
    config.fps             = 25;
    outrun.game_state      = 7;

    if (!roms.load_revb_roms(false)) return 2;
    osoundint.pcm = nullptr; osoundint.ym = nullptr;
    osoundint.init();
    osoundint.has_booted = true;
    osoundint.queue_sound_service((uint8_t)sound_id);

    int frames = (int)(max_s * config.fps + 0.5);
    int tpf = 125 / config.fps;

    // Capture chan_ram at four moments: early, early+period, late, late+period.
    int target_early_a = (int)(early_s            * config.fps + 0.5);
    int target_early_b = (int)((early_s+period_s) * config.fps + 0.5);
    int target_late_a  = (int)(late_s             * config.fps + 0.5);
    int target_late_b  = (int)((late_s +period_s) * config.fps + 0.5);
    int last_target = std::max({target_early_a, target_early_b, target_late_a, target_late_b});
    if (last_target >= frames) frames = last_target + 1;

    std::vector<uint8_t> ea(CHAN_BYTES), eb(CHAN_BYTES);
    std::vector<uint8_t> la(CHAN_BYTES), lb(CHAN_BYTES);
    bool got_ea=false, got_eb=false, got_la=false, got_lb=false;

    for (int f = 0; f < frames; ++f) {
        osoundint.advance(tpf);
        osoundint.pcm->stream_update();
        osoundint.ym->stream_update();
        if (f == target_early_a) { std::memcpy(ea.data(), osound.chan_ram_view(), CHAN_BYTES); got_ea = true; }
        if (f == target_early_b) { std::memcpy(eb.data(), osound.chan_ram_view(), CHAN_BYTES); got_eb = true; }
        if (f == target_late_a)  { std::memcpy(la.data(), osound.chan_ram_view(), CHAN_BYTES); got_la = true; }
        if (f == target_late_b)  { std::memcpy(lb.data(), osound.chan_ram_view(), CHAN_BYTES); got_lb = true; }
    }
    if (!got_ea || !got_eb || !got_la || !got_lb) {
        std::fprintf(stderr, "didn't reach all snapshot times — increase --max-seconds\n");
        return 3;
    }

    std::printf("# diagnose chan_ram drift at period=%.4fs\n", period_s);
    std::printf("# early=%.3fs..%.3fs  late=%.3fs..%.3fs\n",
                early_s, early_s+period_s, late_s, late_s+period_s);

    int early_mismatches = 0, late_mismatches = 0, both_mismatches = 0;
    std::printf("\nbytes that differ in EARLY pair (chan_ram doesn't loop at %.4fs)\n", period_s);
    std::printf("but MATCH in LATE pair (so they've stabilised — these are the slow drifters):\n");
    std::printf("%-3s  %-6s  %-20s  %-6s  %-6s  %-12s\n", "ch", "off", "field", "early_a", "early_b", "delta_early");
    for (size_t i = 0; i < CHAN_BYTES; ++i) {
        bool em = ea[i] != eb[i];
        bool lm = la[i] != lb[i];
        if (em) ++early_mismatches;
        if (lm) ++late_mismatches;
        if (em && lm) ++both_mismatches;
        if (em && !lm) {
            int ch = i / 0x20;
            size_t off = i % 0x20;
            std::printf("%-3d  0x%02zx    %-20s  0x%02x   0x%02x   %+d\n",
                        ch, off, field_name(off),
                        ea[i], eb[i], (int)eb[i] - (int)ea[i]);
        }
    }

    std::printf("\nbytes that differ in BOTH pairs (never converge):\n");
    std::printf("%-3s  %-6s  %-20s  %-6s  %-6s  %-6s  %-6s\n",
                "ch", "off", "field", "ea", "eb", "la", "lb");
    for (size_t i = 0; i < CHAN_BYTES; ++i) {
        if (ea[i] != eb[i] && la[i] != lb[i]) {
            int ch = i / 0x20;
            size_t off = i % 0x20;
            std::printf("%-3d  0x%02zx    %-20s  0x%02x   0x%02x   0x%02x   0x%02x\n",
                        ch, off, field_name(off), ea[i], eb[i], la[i], lb[i]);
        }
    }

    std::printf("\nsummary:\n");
    std::printf("  early pair (t=%.3fs vs %.3fs):  %d / %zu bytes differ\n",
                early_s, early_s+period_s, early_mismatches, CHAN_BYTES);
    std::printf("  late  pair (t=%.3fs vs %.3fs):  %d / %zu bytes differ\n",
                late_s, late_s+period_s, late_mismatches, CHAN_BYTES);
    std::printf("  stabilised between early and late: %d bytes\n",
                early_mismatches - both_mismatches);
    std::printf("  still drifting at late time: %d bytes\n", both_mismatches);
    return 0;
}
