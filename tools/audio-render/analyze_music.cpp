// Static MML music-loop analyzer.
//
// Walks the Z80 music data for a given sound id by interpreting the MML
// bytecode directly out of the z80 ROM — no chip emulation, no playback,
// no simulator. For each FM channel of a music track we trace notes,
// CALL/RET subroutines, finite LOOPs and LOOP_FOREVER opcodes, accumulate
// the wall-clock duration in 125 Hz ticks, and report the structural loop
// period of the longest channel.
//
// Why a separate tool from detect-loop: the runtime detector watches
// loop_fires counters as the simulator ticks, so it can only see loops that
// the simulator actually completes within its time window — and it can't
// see whether a LOOP_FOREVER even has a path back to itself. The static
// walker decides that from the source bytes alone, so it can tell when a
// track is one-shot, when it uses CALL/RET as its loop primitive, or when
// the LOOP_FOREVER target lies before END_FM_TRACK (so the second fire
// never happens).
//
// Usage:
//   analyze-music <sound-id-hex> [--rom-path PATH] [--max-ticks N]
//
// Only music ids are supported (0x81 BREEZE, 0x82 SPLASH, 0x85 MAGICAL,
// 0xA5 LASTWAVE, plus the 0x9A UFO / 0x9B BEEP2 jingles that route through
// the music channels).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

#include "frontend/config.hpp"
#include "roms.hpp"
#include "engine/audio/osound.hpp"   // for the mml:: enum values
#include "engine/audio/osoundadr.hpp"

namespace
{
    // Per-sound MML entry point (1st arg of init_sound at the sound id's
    // dispatch case). Only the music ids are listed — FM SFX are short
    // one-shots and aren't interesting for loop analysis.
    struct MusicEntry
    {
        uint8_t  id;
        uint16_t data_addr;     // points at the channel-setup-table pointer
        const char* name;
    };

    constexpr MusicEntry music_table[] = {
        { 0x81, z80_adr::DATA_BREEZE,   "BREEZE"   },
        { 0x82, z80_adr::DATA_SPLASH,   "SPLASH"   },
        { 0x85, z80_adr::DATA_MAGICAL,  "MAGICAL"  },
        { 0xA5, z80_adr::DATA_LASTWAVE, "LASTWAVE" },
    };

    const MusicEntry* lookup_music(uint8_t id)
    {
        for (const auto& m : music_table) if (m.id == id) return &m;
        return nullptr;
    }

    // Mirrors OSound::process_section's note duration calc (osound.cpp:532).
    //
    // Returns ticks consumed by a NOTE (cmd byte < 0x80) or a PCM sample
    // (cmd byte >= 0xBF). Reads end-marker bytes after the cmd byte and
    // advances `pos` past them. `fm_marker` mirrors chan[FM_MARKER]; on
    // entry bit 0 (LONG) and bit 1 (FIXED_TEMPO) may be set by prior
    // opcodes — we clear bit 0 the way osound's calc_end_marker does.
    uint32_t consume_note_duration(uint16_t& pos, uint8_t end_mult,
                                   uint8_t& fm_marker)
    {
        uint32_t dur = roms.z80.read8(pos);
        pos++;

        if (fm_marker & 0x02) // FIXED_TEMPO: end marker is read literally
        {
            if (fm_marker & 0x01) // LONG: high byte follows
            {
                dur |= (uint32_t)roms.z80.read8(pos) << 8;
                pos++;
                fm_marker &= ~0x01;
            }
            return dur;
        }
        return dur * (uint32_t)end_mult;
    }
}

struct Walker
{
    uint16_t pos;
    uint8_t  end_mult;          // chan[END_MARKER], note-duration multiplier
    uint8_t  fm_marker;         // chan[FM_MARKER]: bit0=LONG, bit1=FIXED_TEMPO
    uint64_t ticks;             // wall-clock ticks accumulated so far

    // Subroutine return stack (MEM_OFFSET grows downward in the real engine;
    // we just use a vector for clarity).
    std::vector<uint16_t> call_stack;

    // Finite-LOOP counters keyed by the loop's "register slot" byte. Same
    // semantics as chan[offset] in OSound::do_loop (osound.cpp:740).
    std::unordered_map<uint8_t, uint8_t> loop_counters;

    // Stamp the tick value the first time each LOOP_FOREVER opcode is
    // entered. If we re-enter the same opcode address, ticks_now minus the
    // stamp is the structural loop period.
    std::unordered_map<uint16_t, uint64_t> loop_forever_first;
};

static bool g_trace = false;

// Returns: 0 = LOOP_FOREVER period found (period_ticks set),
//          1 = END_FM_TRACK reached without loop,
//          2 = walker bailed (max_ticks or runaway).
static int walk_channel(uint16_t start_pos, uint8_t initial_end_mult,
                        uint64_t max_ticks, uint64_t& period_ticks,
                        uint64_t& total_ticks)
{
    Walker w{};
    w.pos       = start_pos;
    w.end_mult  = initial_end_mult ? initial_end_mult : 1;
    w.fm_marker = 0;
    w.ticks     = 0;
    period_ticks = 0;

    // Safety: bound MML steps so a malformed walk can't spin forever.
    const uint64_t max_steps = 1'000'000;
    uint64_t steps = 0;

    while (steps++ < max_steps && w.ticks < max_ticks)
    {
        uint16_t cmd_pos = w.pos;
        uint8_t  cmd     = roms.z80.read8(w.pos);
        w.pos++;

        // Note or PCM sample → consume duration, contribute ticks.
        if (cmd < 0x80 || cmd >= 0xBF)
        {
            // PCM commands (cmd >= 0xBF, !=0): cmd byte selects the PCM index
            // then calc_end_marker reads the duration the same way notes do.
            // cmd == 0 also falls through calc_end_marker.
            w.ticks += consume_note_duration(w.pos, w.end_mult, w.fm_marker);
            continue;
        }

        // MML opcode dispatch — mirror OSound::next_mml_cmd (osound.cpp:562).
        uint8_t op = cmd & 0x3F;
        switch (op)
        {
            case 0x01: /* TEMPO  – no-op in osound, no extra bytes consumed */
                break;

            case mml::SAMPLE_LEVEL:
                // YM path: stores one byte into FM_MARKER (volume / sample
                // level — *not* the note-length multiplier; END_MARKER is
                // only set from the 14-byte initial channel state and
                // never moves). PCM path would consume two bytes but YM
                // music tracks never run this on PCM-flagged channels.
                w.pos++;
                break;

            case mml::END_FM_TRACK:
                total_ticks = w.ticks;
                return 1;

            case mml::KEY_FRACTIONS:
                w.pos++;
                break;

            case mml::CALL:
            {
                uint16_t target = roms.z80.read16(w.pos);
                w.pos += 2;
                // OSound's call_adr does `pos++` inside the routine then
                // sets pos = target - 1; the outer `pos++` after the switch
                // brings pos to `target`. We replicate by pushing the
                // post-arg position and jumping to target.
                w.call_stack.push_back(w.pos);
                w.pos = target;
                break;
            }

            case mml::RET:
                if (w.call_stack.empty()) { total_ticks = w.ticks; return 2; }
                w.pos = w.call_stack.back();
                w.call_stack.pop_back();
                break;

            case mml::LOOP_FOREVER:
            {
                uint16_t target = roms.z80.read16(w.pos);
                if (g_trace)
                    std::printf("    LOOP_FOREVER @%04X target=%04X ticks=%llu\n",
                                cmd_pos, target, (unsigned long long)w.ticks);
                // First time we hit *this* opcode address: stamp ticks.
                // Second time: that's the structural loop period.
                auto it = w.loop_forever_first.find(cmd_pos);
                if (it == w.loop_forever_first.end())
                {
                    w.loop_forever_first[cmd_pos] = w.ticks;
                    w.pos = target;
                }
                else
                {
                    period_ticks = w.ticks - it->second;
                    total_ticks  = w.ticks;
                    return 0;
                }
                break;
            }

            case mml::TRANSPOSE:
                w.pos++;
                break;

            case mml::LOOP:
            {
                // do_loop format: <reg> <count> <addr_lo> <addr_hi>
                uint8_t reg   = roms.z80.read8(w.pos);
                uint8_t count = roms.z80.read8((uint16_t)(w.pos + 1));

                auto it = w.loop_counters.find(reg);
                uint8_t& counter = (it == w.loop_counters.end())
                    ? w.loop_counters[reg]
                    : it->second;
                if (counter == 0) counter = count;
                counter--;

                if (counter != 0)
                {
                    uint16_t target = roms.z80.read16((uint16_t)(w.pos + 2));
                    w.pos = target;
                }
                else
                {
                    w.pos += 4; // skip past (reg, count, addr_lo, addr_hi)
                    w.loop_counters.erase(reg);
                }
                break;
            }

            case mml::PITCH_BEND_END:
                // pos-- in osound, then outer pos++ ⇒ no net movement.
                break;

            case mml::LOAD_PATCH:
                w.pos++;
                break;

            case mml::VOICE_PITCH:
                // PCM-only opcode; for YM channels osound's path doesn't
                // read any extra byte, but the engine increments past one
                // regardless. Stay safe and consume one.
                w.pos++;
                break;

            case mml::FIXED_TEMPO:
                w.fm_marker |= 0x02;
                break;

            case mml::LONG:
                w.fm_marker |= 0x01;
                break;

            case mml::RIGHT_CH_ONLY:
            case mml::LEFT_CH_ONLY:
            case mml::BOTH_CH:
                // pos-- balances outer pos++.
                break;

            case 0x19:
                total_ticks = w.ticks;
                return 1; // pcm_finalize ⇒ track ends

            default:
                // Unknown opcode — treat as terminator to be safe and
                // signal back via "bailed".
                std::fprintf(stderr, "  unknown MML opcode 0x%02X at 0x%04X\n",
                             cmd, cmd_pos);
                total_ticks = w.ticks;
                return 2;
        }
    }

    total_ticks = w.ticks;
    return 2;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr,
            "usage: analyze-music <sound-id-hex> [--rom-path PATH] [--max-ticks N]\n");
        return 1;
    }

    uint8_t  sound_id = (uint8_t)std::strtoul(argv[1], nullptr, 0);
    std::string rom_path = "roms/";
    uint64_t max_ticks   = 125 * 600; // 10 minutes is plenty
    int      trace_ch    = -1;

    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--rom-path" && i + 1 < argc) rom_path = argv[++i];
        else if (a == "--max-ticks" && i + 1 < argc)
            max_ticks = (uint64_t)std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--trace-ch" && i + 1 < argc)
            trace_ch = (int)std::strtol(argv[++i], nullptr, 0);
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 1; }
    }

    config.data.rom_path = rom_path;
    if (!roms.load_revb_roms(false))
    {
        std::fprintf(stderr, "ROM load failed (check --rom-path)\n");
        return 2;
    }

    const MusicEntry* m = lookup_music(sound_id);
    if (!m)
    {
        std::fprintf(stderr, "id 0x%02X is not a music id (try 0x81/0x82/0x85/0xA5)\n",
                     sound_id);
        return 1;
    }

    // Mirror init_sound (osound.cpp:847):
    //   src = read16(DATA_*)        ; pointer to channel setup table
    //   uint8_t channels = read8(&src)
    //   for each ch: uint16_t setup_addr = read16(&src); then 14 bytes at
    //   setup_addr describe the channel state (offsets 0x00..0x0D).
    uint16_t header_ptr = roms.z80.read16(m->data_addr);
    uint8_t  num_chans  = roms.z80.read8(header_ptr);
    header_ptr++;

    std::printf("track: 0x%02X %s   data=0x%04X  header=0x%04X  channels=%u\n",
                m->id, m->name, m->data_addr, header_ptr - 1, num_chans);
    std::printf("\n%-5s %-10s %-12s %-10s %-10s %-10s\n",
                "ch", "setup@", "MML@(SEQ_CMD)", "end_mult", "ticks", "period(s)");
    std::printf("----- ---------- ------------ ---------- ---------- ----------\n");

    uint64_t max_period_ticks = 0;
    for (uint8_t ch = 0; ch < num_chans; ++ch)
    {
        uint16_t setup_addr = roms.z80.read16(header_ptr);
        header_ptr += 2;

        // Read the 14-byte channel state. SEQ_CMD is at offset 0x07–0x08
        // (little-endian word) and END_MARKER at offset 0x02.
        uint8_t state[14];
        for (int i = 0; i < 14; ++i)
            state[i] = roms.z80.read8((uint16_t)(setup_addr + i));

        uint16_t seq_cmd  = state[0x07] | (state[0x08] << 8);
        uint8_t  end_mult = state[0x02];

        uint64_t period_ticks = 0;
        uint64_t total_ticks  = 0;
        g_trace = (trace_ch == (int)ch);
        int rc = walk_channel(seq_cmd, end_mult, max_ticks,
                              period_ticks, total_ticks);
        g_trace = false;

        const char* note = "";
        if      (rc == 1) note = " (one-shot, END_FM_TRACK)";
        else if (rc == 2) note = " (bail: no loop within window)";

        if (period_ticks > max_period_ticks) max_period_ticks = period_ticks;

        std::printf("%-5u 0x%04X     0x%04X       %-10u %-10llu %-10.3f%s\n",
                    ch, setup_addr, seq_cmd, end_mult,
                    (unsigned long long)total_ticks,
                    period_ticks / 125.0, note);
    }

    std::printf("\nmaster loop period (longest channel): %.3f s\n",
                max_period_ticks / 125.0);
    if (max_period_ticks == 0)
        std::printf("(no LOOP_FOREVER cycle found — track is one-shot or uses a different loop mechanism)\n");

    return 0;
}
