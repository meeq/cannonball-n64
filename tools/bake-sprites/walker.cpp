// Static ROM walker for bake-sprites.
//
// Enumerates every (bank, addr, pitch, max_h) tuple the OutRun sprite hardware
// could ever be asked to render, by walking the master CPU ROM's sprite/anim
// descriptor tables (oaddresses.hpp constants), then decoding each entry's
// 5 zoom-level sub-descriptors (ZOOM_LOOKUP SIZE1..SIZE5).
//
// Sub-descriptor format (mirror of osprites.cpp:619-768):
//   +1  width_index   -> WH_TABLE
//   +3  height_index  -> WH_TABLE
//   +5  pitch         (signed byte, << 1 at engine call)
//   +7  bank          (low 3 bits, << 1 at engine call)
//   +8  uint16 BE     offset within selected sprite bank
//
// Note: WH_TABLE indexing uses a composed value (high byte from
// lookup_mask, low byte from descriptor). The lookup_mask high byte varies
// across zoom rows. To avoid replicating the full WH_TABLE composition,
// the walker samples a fan of plausible high-byte values and takes the max
// observed height. WH_TABLE entries that produce nonsense (h > 256 or h == 0)
// are clamped/ignored.
//
// flip is NOT in the descriptor — it's set dynamically at each consumer call
// site. The walker emits both flip variants for every (bank, addr, pitch)
// tuple it finds.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "roms.hpp"
#include "walker.hpp"

// ----------------------------------------------------------------------------
// ROM addresses (REV B / Western set). Sourced from oaddresses.hpp.
// JP-only addresses are not walked — the runtime swap path uses the same
// underlying sprite ROM, so the W set is a superset of the descriptor
// addresses that the engine can hand to hwsprites.
// ----------------------------------------------------------------------------

static const uint32_t WH_TABLE          = 0x20000u;
static const uint32_t ROM0_SIZE         = 0x40000u;  // bound for sanity-check

// ZOOM_LOOKUP SIZE constants (sub-descriptor offsets within input->addr).
// SIZE1 = largest sprite (offset 0), SIZE5 = smallest (offset 0x28).
static const uint16_t SUB_DESC_OFFSETS[5] = {
    0x0000, 0x000A, 0x0014, 0x001E, 0x0028
};

// ----------------------------------------------------------------------------
// Table descriptors.
//
// Kind::Long  means each entry is a single long (read32) holding an
//             absolute input->addr in rom0.
// Kind::Stride means each entry is `stride` bytes; the input->addr is at
//             `addr_off` within the entry.
// Kind::Imm   means base IS the input->addr (single-sprite tables).
//
// `count` is a loose upper bound — the walker validates each candidate
// addr before accepting it, so over-counting is safe.
// ----------------------------------------------------------------------------

enum class Kind { Imm, Long, Stride };

struct TableDesc {
    const char* name;
    uint32_t    base;
    Kind        kind;
    uint32_t    stride;    // bytes per entry (Stride only)
    uint32_t    addr_off;  // offset of long addr within entry (Stride only)
    uint32_t    count;     // entries to walk (Long, Stride)
};

static const TableDesc kTables[] = {
    // ----- Single-addr SPRITE_* constants (sprite->addr = adr.X) -----
    // Player car
    {"sprite_porsche",        0xF290,  Kind::Imm,    0, 0, 1},

    // Logo
    {"sprite_logo_bg",        0x11162, Kind::Imm,    0, 0, 1},
    {"sprite_logo_car",       0x1128E, Kind::Imm,    0, 0, 1},
    {"sprite_logo_bird1",     0x112C0, Kind::Imm,    0, 0, 1},
    {"sprite_logo_bird2",     0x112F2, Kind::Imm,    0, 0, 1},
    {"sprite_logo_base",      0x1125C, Kind::Imm,    0, 0, 1},
    {"sprite_logo_text",      0x11194, Kind::Imm,    0, 0, 1},
    {"sprite_logo_palm1",     0x111C6, Kind::Imm,    0, 0, 1},
    {"sprite_logo_palm2",     0x111F8, Kind::Imm,    0, 0, 1},
    {"sprite_logo_palm3",     0x1122A, Kind::Imm,    0, 0, 1},

    // Music-select
    {"sprite_fm_left",        0x11892, Kind::Imm,    0, 0, 1},
    {"sprite_fm_centre",      0x1189C, Kind::Imm,    0, 0, 1},
    {"sprite_fm_right",       0x118A6, Kind::Imm,    0, 0, 1},
    {"sprite_dial_left",      0x118B0, Kind::Imm,    0, 0, 1},
    {"sprite_dial_centre",    0x118BA, Kind::Imm,    0, 0, 1},
    {"sprite_dial_right",     0x118C4, Kind::Imm,    0, 0, 1},
    {"sprite_eq",             0x118CE, Kind::Imm,    0, 0, 1},
    {"sprite_radio",          0x118D8, Kind::Imm,    0, 0, 1},
    {"sprite_hand_left",      0x118E2, Kind::Imm,    0, 0, 1},
    {"sprite_hand_centre",    0x118EC, Kind::Imm,    0, 0, 1},
    {"sprite_hand_right",     0x118F6, Kind::Imm,    0, 0, 1},

    // Shadow
    {"sprite_shdw_small",     0x1193C, Kind::Imm,    0, 0, 1},
    // shadow_data is BOTH a Long-table base AND used directly as an input_addr
    // (oanimseq.cpp:413/434/455/487 and osprites.cpp:92/95/100 assign
    // sprite->addr = outrun.adr.shadow_data without dereferencing).
    {"sprite_shadow_data_imm",0x103B6, Kind::Imm,    0, 0, 1},

    // Coursemap minicar
    {"sprite_minicar_right",  0x10C58, Kind::Imm,    0, 0, 1},
    {"sprite_minicar_up",     0x10C62, Kind::Imm,    0, 0, 1},
    {"sprite_minicar_down",   0x10C6C, Kind::Imm,    0, 0, 1},

    // Coursemap (3 separate component sprites)
    {"sprite_coursemap_top",  0x3784,  Kind::Imm,    0, 0, 1},
    {"sprite_coursemap_bot",  0x386C,  Kind::Imm,    0, 0, 1},
    {"sprite_coursemap_end",  0x3954,  Kind::Imm,    0, 0, 1},

    // ----- Long pointer tables (read32 per entry) -----
    // sprite_type_table: indexed by sprite->type, max ~256 longs.
    {"sprite_type_table",     0x11ED2, Kind::Long,   4, 0, 256},

    // Sprite frame tables (varying entry counts; 4-byte longs).
    // These are read via `read32(table + i*4)` style.
    {"sprite_cloud_frames",   0x4246,  Kind::Long,   4, 0, 64},
    {"sprite_minitree_frames",0x435C,  Kind::Long,   4, 0, 64},
    {"sprite_grass_frames",   0x4548,  Kind::Long,   4, 0, 16},
    {"sprite_sand_frames",    0x4588,  Kind::Long,   4, 0, 16},
    {"sprite_stone_frames",   0x45C8,  Kind::Long,   4, 0, 16},
    {"sprite_water_frames",   0x4608,  Kind::Long,   4, 0, 16},
    {"sprite_pass_frames",    0xA6EC,  Kind::Long,   4, 0, 8},

    // sprite_shdw_frames: depth-indexed shadow frames.
    {"sprite_shdw_frames",    0x7862,  Kind::Long,   4, 0, 64},

    // sprite_shadow_data: per-type shadow lookups.
    {"sprite_shadow_data",    0x103B6, Kind::Long,   4, 0, 256},

    // Passenger skid frames (4 separate small tables).
    {"sprite_pass1_skidl",    0x1107C, Kind::Long,   4, 0, 16},
    {"sprite_pass1_skidr",    0x110C2, Kind::Long,   4, 0, 16},
    {"sprite_pass2_skidl",    0x110CC, Kind::Long,   4, 0, 16},
    {"sprite_pass2_skidr",    0x11112, Kind::Long,   4, 0, 16},

    // ----- 8-byte frame entries (long addr at +0) -----
    // Crash animation tables. Each entry: +0 long addr, +4 flip-byte, +5 pal,
    // +6 pass_frame/x_off, +7 sentinel/y_off.
    {"sprite_crash_spin1",    0x2294,  Kind::Stride, 8, 0, 8},
    {"sprite_crash_spin2",    0x22D4,  Kind::Stride, 8, 0, 8},
    {"sprite_bump_data1",     0x2314,  Kind::Stride, 8, 0, 3},
    {"sprite_bump_data2",     0x232C,  Kind::Stride, 8, 0, 3},
    {"sprite_crash_man1",     0x2344,  Kind::Stride, 8, 0, 14},
    {"sprite_crash_girl1",    0x23B4,  Kind::Stride, 8, 0, 14},
    {"sprite_crash_flip",     0x2424,  Kind::Stride, 8, 0, 8},
    {"sprite_crash_flip_m1",  0x2464,  Kind::Stride, 8, 0, 16},
    {"sprite_crash_flip_g1",  0x255C,  Kind::Stride, 8, 0, 16},
    {"sprite_crash_flip_m2",  0x24DC,  Kind::Stride, 8, 0, 16},
    {"sprite_crash_flip_g2",  0x25D4,  Kind::Stride, 8, 0, 6},
    {"sprite_crash_man2",     0x2604,  Kind::Stride, 8, 0, 12},
    {"sprite_crash_girl2",    0x2660,  Kind::Stride, 8, 0, 12},

    // Ferrari frames: 8-byte entries, long at +0, indexed by turn+incline.
    {"sprite_ferrari_frames", 0x9ECC,  Kind::Stride, 8, 0, 16},
    {"sprite_skid_frames",    0x9F1C,  Kind::Stride, 8, 0, 32},

    // Smoke/spray: 4-byte longs per smoke type.
    {"smoke_data",            0xACC6,  Kind::Long,   4, 0, 16},
    {"spray_data",            0xAD06,  Kind::Long,   4, 0, 16},

    // End-sequence animation frames (10 obj tables of 8-byte entries).
    {"anim_endseq_obj1",      0x124B0, Kind::Stride, 8, 0, 5},
    {"anim_endseq_obj2",      0x124D8, Kind::Stride, 8, 0, 5},
    {"anim_endseq_obj3",      0x12500, Kind::Stride, 8, 0, 5},
    {"anim_endseq_obj4",      0x12528, Kind::Stride, 8, 0, 5},
    {"anim_endseq_obj5",      0x12550, Kind::Stride, 8, 0, 5},
    {"anim_endseq_obj6",      0x12578, Kind::Stride, 8, 0, 5},
    {"anim_endseq_obj7",      0x125A0, Kind::Stride, 8, 0, 5},
    {"anim_endseq_obj8",      0x125C8, Kind::Stride, 8, 0, 5},
    {"anim_endseq_objA",      0x125F0, Kind::Stride, 8, 0, 5},
    {"anim_endseq_objB",      0x12618, Kind::Stride, 8, 0, 5},

    // Animation chain tables (curr/next pairs).
    {"anim_seq_flag",         0x12382, Kind::Stride, 8, 0, 16},
    {"anim_ferrari_curr",     0x12970, Kind::Stride, 8, 0, 10},
    {"anim_ferrari_next",     0x129C0, Kind::Stride, 8, 0, 1},
    {"anim_pass1_curr",       0x129C8, Kind::Stride, 8, 0, 10},
    {"anim_pass1_next",       0x12A18, Kind::Stride, 8, 0, 1},
    {"anim_pass2_curr",       0x12A20, Kind::Stride, 8, 0, 10},
    {"anim_pass2_next",       0x12A70, Kind::Stride, 8, 0, 1},

    // anim_ferrari_frames: 8 bytes per entry, +0 long, +7 hflip.
    {"anim_ferrari_frames",   0xA2F0,  Kind::Stride, 8, 0, 64},

    // ----- Traffic -----
    // traffic_data: 0x20-byte boundaries per traffic type, with 8 directional
    // long-addr frames within each block. 6 traffic types.
    {"traffic_data",          0x5424,  Kind::Long,   4, 0, 48},
};
static const int kNumTables = sizeof(kTables) / sizeof(kTables[0]);

// ----------------------------------------------------------------------------
// Per-sub-descriptor decode result.
// ----------------------------------------------------------------------------

struct DescTuple {
    uint16_t bank;
    uint16_t addr;     // offset within bank
    int16_t  pitch;
    uint16_t h;        // max derived height (pixels)
};

// Compute the max pixel height for a height-index byte by scanning the
// WH_TABLE across every legal high-byte modifier the runtime can ask for.
// Runtime composes the 16-bit WH_TABLE index from
// (input->draw_props | (input->zoom << 8)), then either:
//   (a) height = WH_TABLE[high|h_idx]                       (top_bit==0)
//   (b) height = WH_TABLE[(zoom & 0x7C) << 8 | h_idx] + h_idx (top_bit==1)
// zoom is constrained to 0..0x7F (set_zoom takes 7 bits), so the high byte
// stays in 0x00..0x7F. We sweep every byte in that range for path (a) and
// every multiple-of-4 byte for path (b). High bytes >= 0x80 are invalid for
// the runtime (would inflate h with garbage from the lower part of WH_TABLE
// that holds packed scale multipliers, not real source row counts).
static uint16_t lookup_max_height(const Roms& r, uint8_t h_idx)
{
    uint32_t max_h = 0;
    for (uint32_t high = 0; high <= 0x7F; high++) {
        uint32_t idx = WH_TABLE + (high << 8) + h_idx;
        if (idx + 1 > r.rom0p->length) continue;
        uint32_t h = r.rom0p->read8(idx);
        if (h > 0 && h <= 256 && h > max_h) max_h = h;

        // top_bit==1 path: high byte is (zoom & 0x7C), so any multiple of 4.
        // The looked-up height has h_idx added.
        if ((high & 0x03) == 0) {
            uint32_t h2 = h + (uint32_t)h_idx;
            if (h > 0 && h2 <= 256 && h2 > max_h) max_h = h2;
        }
    }
    return (uint16_t)max_h;
}

// Decode one sub-descriptor at `sub_addr`. Returns false if the bytes don't
// pass sanity checks (allowing the walker to skip non-descriptor data).
static bool decode_sub_descriptor(const Roms& r, uint32_t sub_addr,
                                  DescTuple& out)
{
    if (sub_addr + 10 > r.rom0p->length) return false;

    const uint8_t  h_idx     = r.rom0p->read8(sub_addr + 3);
    const uint8_t  pitch_raw = r.rom0p->read8(sub_addr + 5);
    const uint8_t  bank_raw  = r.rom0p->read8(sub_addr + 7);
    const uint16_t addr      = r.rom0p->read16(sub_addr + 8);

    // Bank field uses low 3 bits.
    out.bank  = (uint16_t)(bank_raw & 0x7);
    out.addr  = addr;
    // Runtime pitch composition. set_pitch stores `(pitch_raw << 1) & 0xFE`
    // shifted into data[2] bits [15:9]; the renderer recovers it as
    // pitch_raw bits [6:0] (bit 7 of pitch_raw is lost). data[4] bit 12 is
    // never set (set_hzoom always writes 0 there), so there is no flip-bit
    // overlay. Pitch is always a positive uint8 in 0..0x7F.
    out.pitch = (int16_t)(pitch_raw & 0x7Fu);

    out.h = lookup_max_height(r, h_idx);
    if (out.h == 0) return false;  // bogus h -> not a real descriptor

    // Each bank is 0x10000 words. The renderer walks `h` rows starting at
    // `addr`, stepping forward by `pitch` per row, and consumes up to
    // MAX_WORDS_PER_ROW (32) words per row. CAP h (don't reject) so a
    // descriptor whose inflated max-h spills past the bank still bakes the
    // largest valid subrange — the runtime will request a smaller source_h
    // for actual zoom levels and find a matching entry.
    const int32_t  MAX_ROW_WORDS = 32;
    if (out.pitch > 0) {
        const int32_t max_h_rows =
            ((0x10000 - MAX_ROW_WORDS) - (int32_t)addr) / (int32_t)out.pitch + 1;
        if (max_h_rows <= 0) return false;
        if ((int32_t)out.h > max_h_rows) out.h = (uint16_t)max_h_rows;
    } else {
        // pitch==0: only the first row is read repeatedly. One row suffices.
        out.h = 1;
    }

    return true;
}

// Walk one (input_addr) — emit up to 5 (bank, addr, pitch, h) tuples.
static void decode_input_addr(const Roms& r, uint32_t input_addr,
                              std::vector<DescTuple>& out)
{
    if (input_addr == 0) return;
    if (input_addr + 0x32 > r.rom0p->length) return;

    for (int i = 0; i < 5; i++) {
        uint32_t sub = input_addr + SUB_DESC_OFFSETS[i];
        DescTuple t{};
        if (decode_sub_descriptor(r, sub, t)) {
            out.push_back(t);
        }
    }
}

// Returns true if EOR-walking sprite data at (bank, addr) terminates within
// MAX_WORDS_PER_ROW for the requested flip direction. This is the same loop
// the real renderer runs — random ROM bytes almost never satisfy it for both
// flips, so it's a strong validity check.
static bool sprite_row_eor_terminates(const std::vector<uint32_t>& spr,
                                      uint16_t bank, uint16_t addr,
                                      bool flip)
{
    // Only banks 0..3 are populated (sprites[] is 0x40000 words = 4 banks).
    // Bank 4..7 access would walk past the buffer — reject the candidate.
    const uint32_t numbanks = (uint32_t)(spr.size() / 0x10000u);
    if ((uint32_t)bank >= numbanks) return false;
    const uint32_t* spritedata = spr.data() + 0x10000u * (uint32_t)bank;
    static const int MAX_ROW_WORDS = 32;
    uint32_t cur = addr;
    for (int i = 0; i < MAX_ROW_WORDS; i++) {
        if (cur >= 0x10000u) return false;
        uint32_t pixels = flip ? spritedata[cur--] : spritedata[cur++];
        const uint32_t eor_mask = flip ? 0x0f000000u : 0x000000f0u;
        if ((pixels & eor_mask) == eor_mask) return true;
    }
    return false;
}

// Strict check: walk EVERY row of an (addr, pitch, h) sprite and confirm
// each EOR-terminates within MAX_ROW_WORDS in both flip directions. Real
// sprites also have CONSISTENT row widths (the EOR column is the same on
// every row, by construction of the sprite hardware's bitmap format).
// Random ROM bytes pass per-row EOR easily (eor_mask is a single nibble,
// ~6% hit per word, ~94% within 32 words) — but the width-consistency
// check knocks the false-positive rate to near zero.
static bool sprite_block_eor_terminates(const std::vector<uint32_t>& spr,
                                        const DescTuple& t)
{
    const uint32_t numbanks = (uint32_t)(spr.size() / 0x10000u);
    if ((uint32_t)t.bank >= numbanks) return false;
    const uint32_t* spritedata = spr.data() + 0x10000u * (uint32_t)t.bank;
    static const int MAX_ROW_WORDS = 32;

    int width_min[2] = {0x7fffffff, 0x7fffffff};
    int width_max[2] = {0, 0};

    uint32_t row_base = t.addr;
    for (int row = 0; row < (int)t.h; row++) {
        for (int flip = 0; flip < 2; flip++) {
            uint32_t cur = row_base;
            int words = 0;
            bool ok = false;
            for (int i = 0; i < MAX_ROW_WORDS; i++) {
                if (cur >= 0x10000u) return false;
                uint32_t pixels = flip ? spritedata[cur--] : spritedata[cur++];
                words++;
                const uint32_t eor_mask = flip ? 0x0f000000u : 0x000000f0u;
                if ((pixels & eor_mask) == eor_mask) { ok = true; break; }
            }
            if (!ok) return false;
            if (words < width_min[flip]) width_min[flip] = words;
            if (words > width_max[flip]) width_max[flip] = words;
        }
        row_base += (uint32_t)(int32_t)t.pitch;
    }
    // Real sprites: per-flip width is consistent across rows (delta <= 1
    // would be conservative; allow up to 2 for safety on irregular shapes).
    for (int flip = 0; flip < 2; flip++) {
        if (width_max[flip] - width_min[flip] > 2) return false;
    }
    return true;
}

// Strict structural check: does `input_addr` look like a real 5-sub-descriptor
// block? Each of the 5 SIZE slots must decode AND its first sprite-data row
// must EOR-terminate within MAX_WORDS_PER_ROW words in BOTH flip directions.
// We additionally require the 5 SIZE slots to form a monotonic non-increasing
// (h, pitch) sequence — SIZE1 is the largest zoom and SIZE5 the smallest,
// so real descriptors progress that way. These three checks together knock
// the false-positive rate to near zero.
static bool looks_like_input_addr(const Roms& r,
                                  const std::vector<uint32_t>& sprites_words,
                                  uint32_t input_addr)
{
    if (input_addr == 0) return false;
    if (input_addr + 0x32 > r.rom0p->length) return false;

    DescTuple ts[5];
    for (int i = 0; i < 5; i++) {
        if (!decode_sub_descriptor(r, input_addr + SUB_DESC_OFFSETS[i], ts[i]))
            return false;
        if (ts[i].pitch == 0) return false;
        if (!sprite_row_eor_terminates(sprites_words, ts[i].bank, ts[i].addr, false))
            return false;
        if (!sprite_row_eor_terminates(sprites_words, ts[i].bank, ts[i].addr, true))
            return false;
    }
    for (int i = 1; i < 5; i++) {
        if (ts[i].h     > ts[i-1].h)     return false;
        if (ts[i].pitch > ts[i-1].pitch) return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Public entry point: walk all known tables and emit a deduplicated set of
// (bank, addr, pitch, max_h) tuples.
// ----------------------------------------------------------------------------

void walk_static_addrs(const Roms& r,
                       const std::vector<uint32_t>& sprites_words,
                       std::vector<StaticTuple>& out)
{
    // First, collect every candidate input_addr.
    std::set<uint32_t> input_addrs;

    for (int ti = 0; ti < kNumTables; ti++) {
        const TableDesc& T = kTables[ti];

        if (T.kind == Kind::Imm) {
            input_addrs.insert(T.base);
            continue;
        }

        // Long / Stride: walk entries.
        for (uint32_t i = 0; i < T.count; i++) {
            uint32_t entry_base = T.base
                + (T.kind == Kind::Long ? i * T.stride : i * T.stride);
            uint32_t addr_loc = entry_base + T.addr_off;
            if (addr_loc + 4 > r.rom0p->length) break;
            uint32_t addr = r.rom0p->read32(addr_loc);
            // Drop obviously-bogus addresses (out of rom0 range or zero).
            if (addr == 0 || addr >= ROM0_SIZE) continue;
            input_addrs.insert(addr);
        }
    }

    std::printf("walker: %zu input_addrs from tables\n", input_addrs.size());

    // Brute-force fallback: scan rom0 for every position that strictly looks
    // like a 5-sub-descriptor block. The validator requires all 5 SIZE slots
    // to decode, EOR-terminate in both flip directions, and form a monotonic
    // non-increasing (h, pitch) sequence — false positives are rare. Found
    // input_addrs that weren't already in our table-driven set come from
    // data-driven sources we don't statically traverse (scenerymap stream,
    // anim_seq_* chains, smoke_data indirection, etc).
    const uint32_t scan_end = (r.rom0p->length > 0x32u)
                              ? (r.rom0p->length - 0x32u) : 0u;
    size_t before = input_addrs.size();
    for (uint32_t scan = 0; scan < scan_end; scan++) {
        if (input_addrs.count(scan)) continue;
        if (!looks_like_input_addr(r, sprites_words, scan)) continue;
        input_addrs.insert(scan);
    }
    std::printf("walker: brute-scan added %zu input_addrs (total %zu)\n",
                input_addrs.size() - before, input_addrs.size());


    // Now decode each. Dedup tuples in a temporary set.
    struct TupleKey {
        uint16_t bank; uint16_t addr; int16_t pitch;
        bool operator<(const TupleKey& o) const {
            if (bank != o.bank)   return bank   < o.bank;
            if (addr != o.addr)   return addr   < o.addr;
            return pitch < o.pitch;
        }
    };
    std::map<TupleKey, uint16_t> tuple_max_h;

    int total_subs = 0;
    int valid_subs = 0;
    for (uint32_t ia : input_addrs) {
        std::vector<DescTuple> subs;
        decode_input_addr(r, ia, subs);
        total_subs += 5;
        valid_subs += (int)subs.size();
        for (const auto& s : subs) {
            TupleKey k{s.bank, s.addr, s.pitch};
            auto it = tuple_max_h.find(k);
            if (it == tuple_max_h.end()) tuple_max_h[k] = s.h;
            else                          it->second   = std::max(it->second, s.h);
        }
    }

    std::printf("walker: %d/%d sub-descriptors decoded, %zu unique (bank,addr,pitch)\n",
        valid_subs, total_subs, tuple_max_h.size());

    // Second-pass brute scan: directly enumerate sub-descriptor candidates.
    // The first pass found input_addrs and walked their 5 SIZE slots; this
    // pass scans every 10-byte window in rom0 and accepts it if it decodes
    // to a sub-descriptor whose sprite data EOR-terminates in BOTH flip
    // directions. This catches data-driven sprites whose enclosing input_addr
    // never appears in a table we recognize (scenerymap stream entries,
    // anim_seq_* indirect chains, etc).
    {
        const size_t before_sub = tuple_max_h.size();
        const uint32_t scan_end_sub = (r.rom0p->length > 10u)
                                      ? (r.rom0p->length - 10u) : 0u;
        const uint32_t numbanks = (uint32_t)(sprites_words.size() / 0x10000u);
        for (uint32_t sub = 0; sub < scan_end_sub; sub++) {
            DescTuple t{};
            if (!decode_sub_descriptor(r, sub, t)) continue;
            if (t.pitch == 0) continue;
            if ((uint32_t)t.bank >= numbanks) continue;
            if (!sprite_block_eor_terminates(sprites_words, t))
                continue;
            TupleKey k{t.bank, t.addr, t.pitch};
            auto it = tuple_max_h.find(k);
            if (it == tuple_max_h.end()) tuple_max_h[k] = t.h;
            else                          it->second   = std::max(it->second, t.h);
        }
        std::printf("walker: sub-scan added %zu unique tuples (total %zu)\n",
                    tuple_max_h.size() - before_sub, tuple_max_h.size());
    }

    out.clear();
    out.reserve(tuple_max_h.size());
    for (const auto& kv : tuple_max_h) {
        StaticTuple st;
        st.bank  = kv.first.bank;
        st.addr  = kv.first.addr;
        st.pitch = kv.first.pitch;
        st.h     = kv.second;
        out.push_back(st);
    }
}
