// bake-sprites — host-side asset pipeline tool.
//
// Reads the OutRun sprite ROM set (same files as the runtime), statically
// walks the master CPU ROM's sprite/anim descriptor tables to enumerate
// every (bank, addr, pitch, max source_h) tuple the hardware could ever be
// asked to render, and emits a pre-decoded CI4 atlas blob + a sorted lookup
// index. At runtime hwsprites::atlas_get_or_extract binary-searches the
// index and PI-DMAs the pre-decoded block straight into the bump pool —
// skipping the EOR walk + bitplane decode that costs ~500 us per sprite
// today.
//
// Decode logic mirrors hwsprites.cpp:atlas_get_or_extract bit-for-bit so the
// baked output is identical to what the runtime would produce.
//
// Also emits sprites_native.bin: the runtime hwsprites byte-swapped uint32_t
// sprite ROM (1 MiB), written big-endian on disk so a PI-DMA into N64 RAM
// reproduces the exact uint32_t array hwsprites::init() used to build. The
// runtime drops the 1 MiB resident `sprites[]` and reads bank slices on demand.
//
// Usage:
//   bake-sprites --roms <roms_dir> --blob <out.bin> --index <out.c>
//                --sprites-blob <out.bin>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "frontend/config.hpp"
#include "roms.hpp"
#include "walker.hpp"

// Mirrored from hwsprites.hpp/cpp — keep in sync.
static const uint32_t SPRITES_LENGTH    = 0x100000u >> 2;   // 256 K words
static const int      MAX_WORDS_PER_ROW = 32;                // 256 px cap

struct BakedEntry {
    uint16_t bank;
    uint16_t addr;
    int16_t  pitch;
    uint8_t  flip;
    uint8_t  has_shadow;
    uint16_t w;
    uint16_t h;
    uint16_t shadow_x0, shadow_y0, shadow_x1, shadow_y1;
    uint32_t blob_offset;       // byte offset into sprite_atlas.bin
    uint32_t blob_size;         // bytes (multiple of 8)
};

static int decode_one(
    const uint32_t* sprites_words,
    uint16_t bank, uint16_t addr, int16_t pitch, bool flip,
    uint16_t source_h,
    std::vector<uint8_t>& out,
    BakedEntry& entry)
{
    const uint32_t* spritedata = sprites_words + 0x10000u * (uint32_t)(bank & 7);

    // Pass 1: find max row width (in 32-bit words).
    int max_words = 0;
    {
        uint32_t row_base = addr;
        for (int row = 0; row < (int)source_h; row++) {
            uint32_t cur = row_base;
            int words = 0;
            for (;;) {
                if (cur >= 0x10000u) return 1;  // would walk outside bank
                uint32_t pixels = flip ? spritedata[cur--] : spritedata[cur++];
                words++;
                const uint32_t eor_mask = flip ? 0x0f000000u : 0x000000f0u;
                if ((pixels & eor_mask) == eor_mask) break;
                if (words >= MAX_WORDS_PER_ROW)      break;
            }
            if (words > max_words) max_words = words;
            row_base += (uint32_t)(int32_t)pitch;
        }
    }

    const int      padded_words = (max_words + 1) & ~1;
    const uint16_t w            = (uint16_t)(padded_words * 8);
    const uint16_t h            = source_h;
    const uint32_t ci4_stride   = (uint32_t)w / 2;
    const uint32_t bytes        = (ci4_stride * (uint32_t)h + 7u) & ~7u;

    out.assign(bytes, 0);

    // Pass 2: decode pixels into CI4.
    uint8_t  has_shadow = 0;
    uint16_t sh_x0 = 0xFFFF, sh_y0 = 0xFFFF, sh_x1 = 0, sh_y1 = 0;
    {
        uint32_t row_base = addr;
        for (int row = 0; row < (int)h; row++) {
            uint32_t cur = row_base;
            uint8_t* row_dst = out.data() + (uint32_t)row * ci4_stride;
            int col = 0;
            int words = 0;
            for (;;) {
                if (cur >= 0x10000u) return 1;  // would walk outside bank
                uint32_t pixels = flip ? spritedata[cur--] : spritedata[cur++];
                for (int n = 0; n < 8; n++) {
                    uint32_t pix = flip
                        ? ((pixels >> (4 * n)) & 0xf)
                        : ((pixels >> (28 - 4 * n)) & 0xf);
                    if (pix == 0xf) pix = 0;
                    if (pix == 0xa) {
                        has_shadow = 1;
                        if ((uint16_t)col     < sh_x0) sh_x0 = (uint16_t)col;
                        if ((uint16_t)col     >= sh_x1) sh_x1 = (uint16_t)(col + 1);
                        if ((uint16_t)row     < sh_y0) sh_y0 = (uint16_t)row;
                        if ((uint16_t)row     >= sh_y1) sh_y1 = (uint16_t)(row + 1);
                    }
                    uint8_t* bp = row_dst + (col >> 1);
                    if (col & 1) *bp = (uint8_t)((*bp & 0xf0) | pix);
                    else         *bp = (uint8_t)(pix << 4);
                    col++;
                }
                words++;
                const uint32_t eor_mask = flip ? 0x0f000000u : 0x000000f0u;
                if ((pixels & eor_mask) == eor_mask) break;
                if (words >= MAX_WORDS_PER_ROW)      break;
            }
            row_base += (uint32_t)(int32_t)pitch;
        }
    }

    entry.bank       = bank;
    entry.addr       = addr;
    entry.pitch      = pitch;
    entry.flip       = flip ? 1 : 0;
    entry.has_shadow = has_shadow;
    entry.w          = w;
    entry.h          = h;
    entry.shadow_x0  = has_shadow ? sh_x0 : 0;
    entry.shadow_y0  = has_shadow ? sh_y0 : 0;
    entry.shadow_x1  = has_shadow ? sh_x1 : 0;
    entry.shadow_y1  = has_shadow ? sh_y1 : 0;
    entry.blob_size  = bytes;
    return 0;
}

static bool write_blob(const std::string& path, const std::vector<uint8_t>& blob)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "bake-sprites: cannot open %s\n", path.c_str()); return false; }
    f.write((const char*)blob.data(), (std::streamsize)blob.size());
    return (bool)f;
}

static bool write_index(const std::string& path,
                        const std::vector<BakedEntry>& entries,
                        uint32_t blob_bytes)
{
    std::ofstream f(path);
    if (!f) { std::fprintf(stderr, "bake-sprites: cannot open %s\n", path.c_str()); return false; }

    f << "// AUTO-GENERATED by tools/bake-sprites. DO NOT EDIT.\n";
    f << "#include <stdint.h>\n";
    f << "#include \"hwvideo/hwsprites_baked.h\"\n";
    f << "\n";
    f << "const uint32_t hwsprites_baked_count = " << entries.size() << ";\n";
    f << "const uint32_t hwsprites_baked_blob_bytes = " << blob_bytes << ";\n";
    f << "\n";
    f << "const HwspritesBakedEntry hwsprites_baked_index[" << entries.size() << "] = {\n";
    for (const auto& e : entries) {
        // Pack 8 bytes of key: bank(8) | flip(8) | pitch(16) | addr(16) | reserved(16).
        // The runtime composes the same key with the same field widths.
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "    { 0x%02xu, 0x%02xu, %d, 0x%04xu, %u, %u, %u, %u, %u, %u, %u, %u },\n",
            (unsigned)e.bank, (unsigned)e.flip,
            (int)e.pitch, (unsigned)e.addr,
            (unsigned)e.w, (unsigned)e.h,
            (unsigned)e.has_shadow,
            (unsigned)e.shadow_x0, (unsigned)e.shadow_y0,
            (unsigned)e.shadow_x1, (unsigned)e.shadow_y1,
            (unsigned)e.blob_offset);
        f << buf;
    }
    f << "};\n";
    return (bool)f;
}

// Bitplane → CI4 conversion for OutRun tile graphics. Mirrors
// hwtiles::init() exactly (src/main/hwvideo/hwtiles.cpp): three 64 KiB
// bitplanes laid out back-to-back at offsets 0x00000/0x10000/0x20000 of
// the 0x30000-byte tile ROM, decoded into a 256 KiB uint32_t array
// indexed by [i], where i ∈ [0, 0x10000). Each output word holds 8 CI4
// pixels (leftmost in the high nibble of byte 0) — the exact CI4 byte
// layout the RDP expects on a big-endian N64.
static void bake_tiles_words(const uint8_t* src_tiles,
                             std::vector<uint32_t>& tiles_words)
{
    const uint32_t TILES_LENGTH = 0x10000u;
    tiles_words.assign(TILES_LENGTH, 0);
    for (uint32_t i = 0; i < TILES_LENGTH; i++) {
        const uint8_t p0 = src_tiles[i];
        const uint8_t p1 = src_tiles[i + 0x10000];
        const uint8_t p2 = src_tiles[i + 0x20000];
        uint32_t val = 0;
        for (int bit_i = 0; bit_i < 8; bit_i++) {
            const uint8_t bit = (uint8_t)(7 - bit_i);
            const uint8_t pix =
                (uint8_t)( ((p0 >> bit)       & 1)
                         | (((p1 >> bit) << 1) & 2)
                         | (((p2 >> bit) << 2) & 4) );
            val = (val << 4) | pix;
        }
        tiles_words[i] = val;
    }
}

int main(int argc, char** argv)
{
    std::string roms_dir, blob_path, index_path,
                sprites_blob_path, tiles_blob_path;
    bool japan_region = false;
    bool phase1_only  = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* who) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "bake-sprites: %s missing arg\n", who); std::exit(2); }
            return std::string(argv[++i]);
        };
        if      (a == "--roms")          roms_dir          = next("--roms");
        else if (a == "--blob")          blob_path         = next("--blob");
        else if (a == "--index")         index_path        = next("--index");
        else if (a == "--sprites-blob")  sprites_blob_path = next("--sprites-blob");
        else if (a == "--tiles-blob")    tiles_blob_path   = next("--tiles-blob");
        else if (a == "--japan")         japan_region      = true;
        else if (a == "--phase1-only")   phase1_only       = true;
        else { std::fprintf(stderr, "bake-sprites: unknown arg %s\n", a.c_str()); return 2; }
    }
    if (roms_dir.empty() || blob_path.empty() || index_path.empty()
        || sprites_blob_path.empty() || tiles_blob_path.empty()) {
        std::fprintf(stderr,
            "usage: bake-sprites --roms <dir> --blob <out.bin> --index <out.c>"
            " --sprites-blob <out.bin> --tiles-blob <out.bin> [--japan]\n");
        return 2;
    }

    // Mirror runtime ROM load path.
    if (!roms_dir.empty() && roms_dir.back() != '/') roms_dir += '/';
    config.data.rom_path = roms_dir;
    if (!roms.load_revb_roms(false)) {
        std::fprintf(stderr, "bake-sprites: ROM load failed (path=%s)\n", roms_dir.c_str());
        return 1;
    }
    // --japan: swap rom0/rom1 in place to the Japanese chip data (the runtime
    // does the same via outrun.select_course(jap=true)). Used for the audit
    // that verifies the World-baked atlas covers Japan too — see
    // tools/bake-sprites/audit_japan.sh.
    if (japan_region) {
        if (!roms.load_japanese_roms()) {
            std::fprintf(stderr, "bake-sprites: Japanese ROM load failed\n");
            return 1;
        }
        std::fprintf(stderr, "bake-sprites: walking JAPAN rom0/rom1\n");
    }
    // load_revb_roms() leaves rom0p/rom1p NULL — the runtime selects via
    // outrun.select_course(). For the bake we always use the W (revb) set.
    roms.rom0p = &roms.rom0;
    roms.rom1p = &roms.rom1;

    // Sprite and tile ROMs aren't loaded by load_revb_roms anymore (the N64
    // runtime gets them as pre-decoded blobs from this very tool's output).
    // Load them here so the walker / bake can read them.
    {
        int status = 0;
        roms.sprites.init(0x100000);
        status += roms.sprites.load_rom("mpr-10371.9",  0x000000, 0x20000, 0x7cc86208, RomLoader::INTERLEAVE4, true);
        status += roms.sprites.load_rom("mpr-10373.10", 0x000001, 0x20000, 0xb0d26ac9, RomLoader::INTERLEAVE4, true);
        status += roms.sprites.load_rom("mpr-10375.11", 0x000002, 0x20000, 0x59b60bd7, RomLoader::INTERLEAVE4, true);
        status += roms.sprites.load_rom("mpr-10377.12", 0x000003, 0x20000, 0x17a1b04a, RomLoader::INTERLEAVE4, true);
        status += roms.sprites.load_rom("mpr-10372.13", 0x080000, 0x20000, 0xb557078c, RomLoader::INTERLEAVE4, true);
        status += roms.sprites.load_rom("mpr-10374.14", 0x080001, 0x20000, 0x8051e517, RomLoader::INTERLEAVE4, true);
        status += roms.sprites.load_rom("mpr-10376.15", 0x080002, 0x20000, 0xf3b8f318, RomLoader::INTERLEAVE4, true);
        status += roms.sprites.load_rom("mpr-10378.16", 0x080003, 0x20000, 0xa1062984, RomLoader::INTERLEAVE4, true);

        roms.tiles.init(0x30000);
        status += roms.tiles.load_rom("opr-10268.99",  0x00000, 0x08000, 0x95344b04, RomLoader::NORMAL, true);
        status += roms.tiles.load_rom("opr-10232.102", 0x08000, 0x08000, 0x776ba1eb, RomLoader::NORMAL, true);
        status += roms.tiles.load_rom("opr-10267.100", 0x10000, 0x08000, 0xa85bb823, RomLoader::NORMAL, true);
        status += roms.tiles.load_rom("opr-10231.103", 0x18000, 0x08000, 0x8908bcbf, RomLoader::NORMAL, true);
        status += roms.tiles.load_rom("opr-10266.101", 0x20000, 0x08000, 0x9f6f1a74, RomLoader::NORMAL, true);
        status += roms.tiles.load_rom("opr-10230.104", 0x28000, 0x08000, 0x686f5e50, RomLoader::NORMAL, true);

        if (status != 0) {
            std::fprintf(stderr, "bake-sprites: sprite/tile ROM load failed\n");
            return 1;
        }
    }

    // Byte-swap into the same uint32_t layout the runtime uses (matches
    // hwsprites::init exactly so spritedata[i] reads are bit-identical).
    std::vector<uint32_t> sprites_words(SPRITES_LENGTH, 0);
    {
        const uint8_t* spr = roms.sprites.rom;
        for (uint32_t i = 0; i < SPRITES_LENGTH; i++) {
            uint8_t d3 = *spr++;
            uint8_t d2 = *spr++;
            uint8_t d1 = *spr++;
            uint8_t d0 = *spr++;
            sprites_words[i] = (uint32_t(d0) << 24) | (uint32_t(d1) << 16)
                             | (uint32_t(d2) <<  8) |  uint32_t(d3);
        }
    }

    // Emit sprites_native.bin — the byte-swapped sprite ROM as raw N64-side
    // bytes. We write each uint32_t big-endian so the disk byte order matches
    // what PI-DMA will land in big-endian N64 RAM; that way the runtime can
    // dma_read(scratch, pi_addr + bank*64KiB*4, BANK_BYTES) and reinterpret
    // scratch as the same uint32_t array hwsprites::init() used to build.
    {
        std::ofstream f(sprites_blob_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "bake-sprites: cannot open %s\n",
                         sprites_blob_path.c_str());
            return 1;
        }
        std::vector<uint8_t> be(SPRITES_LENGTH * 4);
        for (uint32_t i = 0; i < SPRITES_LENGTH; i++) {
            uint32_t w = sprites_words[i];
            be[i*4 + 0] = (uint8_t)((w >> 24) & 0xff);
            be[i*4 + 1] = (uint8_t)((w >> 16) & 0xff);
            be[i*4 + 2] = (uint8_t)((w >>  8) & 0xff);
            be[i*4 + 3] = (uint8_t)( w        & 0xff);
        }
        f.write((const char*)be.data(), (std::streamsize)be.size());
        if (!f) {
            std::fprintf(stderr, "bake-sprites: write failed %s\n",
                         sprites_blob_path.c_str());
            return 1;
        }
    }

    // Emit tiles_native.bin — the 256 KiB pre-decoded CI4 tile blob the N64
    // runtime DMAs on demand instead of holding hwtiles::tiles[] resident.
    // Each uint32_t is written big-endian on disk so PI-DMA into BE N64 RAM
    // reproduces the array bit-for-bit (matches hwtiles::init()'s output
    // exactly).
    {
        std::vector<uint32_t> tiles_words;
        bake_tiles_words(roms.tiles.rom, tiles_words);

        std::ofstream f(tiles_blob_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "bake-sprites: cannot open %s\n",
                         tiles_blob_path.c_str());
            return 1;
        }
        std::vector<uint8_t> be(tiles_words.size() * 4);
        for (size_t i = 0; i < tiles_words.size(); i++) {
            const uint32_t w = tiles_words[i];
            be[i*4 + 0] = (uint8_t)((w >> 24) & 0xff);
            be[i*4 + 1] = (uint8_t)((w >> 16) & 0xff);
            be[i*4 + 2] = (uint8_t)((w >>  8) & 0xff);
            be[i*4 + 3] = (uint8_t)( w        & 0xff);
        }
        f.write((const char*)be.data(), (std::streamsize)be.size());
        if (!f) {
            std::fprintf(stderr, "bake-sprites: write failed %s\n",
                         tiles_blob_path.c_str());
            return 1;
        }
    }

    // Walk every (bank, addr, pitch, max_h) tuple statically from ROM.
    //
    // Production (default) baked World only, which missed ~109 Japan-only
    // sprite descriptors (a Japan-region toggle at runtime would fall through
    // to the slow EOR-walk path for those). Now we walk BOTH regions and
    // union the tuple sets so the resulting atlas covers either CPU ROM
    // load. Sprite pixel data (mpr-1037x.bin) is region-agnostic, so each
    // tuple's blob_offset points into the same data either way — the union
    // costs only the extra unique entries (~109 × 2 flips ≈ 350 KB).
    //
    // --japan flag still walks only Japan (used by the original audit). The
    // --phase1-only flag still skips brute-force scans for tight audits.
    std::vector<StaticTuple> tuples;
    walk_static_addrs(roms, sprites_words, tuples,
                      /*japan=*/japan_region, phase1_only);
    if (!japan_region) {
        // Also walk Japan tables: swap rom0/rom1 in place, walk with
        // kTablesJ[], merge results. We run Japan in phase1-only mode —
        // the brute-force scans for World already catch valid sprite
        // descriptors that EOR-terminate properly in the sprite ROM
        // (region-agnostic), so re-running brute force on Japan rom0
        // mostly just adds false-positive duplicates from different
        // random bytes. The strict-need add is Japan's table-derived
        // (phase 1) tuples — they're the ones the engine code actually
        // references when Japan is loaded.
        if (!roms.load_japanese_roms()) {
            std::fprintf(stderr, "bake-sprites: Japan ROM swap failed for combined walk\n");
            return 1;
        }
        std::vector<StaticTuple> tuples_jap;
        walk_static_addrs(roms, sprites_words, tuples_jap,
                          /*japan=*/true, /*phase1_only=*/true);

        // Merge: tuple identity is (bank, addr, pitch); keep max h.
        struct K { uint16_t bank; uint16_t addr; int16_t pitch;
                   bool operator<(const K& o) const {
                       if (bank != o.bank)   return bank   < o.bank;
                       if (addr != o.addr)   return addr   < o.addr;
                       return pitch < o.pitch; } };
        std::map<K, uint16_t> merged;
        for (const auto& t : tuples)
            merged[K{t.bank, t.addr, t.pitch}] = t.h;
        size_t before = merged.size();
        for (const auto& t : tuples_jap) {
            auto it = merged.find(K{t.bank, t.addr, t.pitch});
            if (it == merged.end()) merged[K{t.bank, t.addr, t.pitch}] = t.h;
            else it->second = std::max(it->second, t.h);
        }
        std::fprintf(stderr, "bake-sprites: combined walk: %zu W + %zu Japan-only "
                             "= %zu unique tuples\n",
                     before, merged.size() - before, merged.size());

        tuples.clear();
        tuples.reserve(merged.size());
        for (const auto& kv : merged) {
            StaticTuple st;
            st.bank  = kv.first.bank;
            st.addr  = kv.first.addr;
            st.pitch = kv.first.pitch;
            st.h     = kv.second;
            tuples.push_back(st);
        }
    }
    if (tuples.empty()) {
        std::fprintf(stderr, "bake-sprites: walker produced no tuples\n");
        return 1;
    }

    std::vector<BakedEntry> entries;
    std::vector<uint8_t>    blob;
    blob.reserve(4 * 1024 * 1024);

    // Bake each (bank, addr, pitch) tuple at both flip variants. flip is set
    // dynamically by the engine consumer sites, so we cover both directions
    // unconditionally.
    int decode_fails = 0;
    for (const StaticTuple& t : tuples) {
        for (int flip_v = 0; flip_v < 2; flip_v++) {
            std::vector<uint8_t> ci4;
            BakedEntry e{};
            if (decode_one(sprites_words.data(),
                           t.bank, t.addr, t.pitch, flip_v != 0,
                           t.h, ci4, e) != 0) {
                decode_fails++;
                continue;
            }
            // 8-byte align the blob position (rdpq DMA + LOAD_BLOCK want it).
            while (blob.size() % 8) blob.push_back(0);
            e.blob_offset = (uint32_t)blob.size();
            blob.insert(blob.end(), ci4.begin(), ci4.end());
            entries.push_back(e);
        }
    }
    if (decode_fails) {
        std::fprintf(stderr, "bake-sprites: %d decode failures (ignored)\n", decode_fails);
    }

    // Sort by (bank, flip, pitch, addr) so the runtime can binary-search.
    std::sort(entries.begin(), entries.end(),
        [](const BakedEntry& a, const BakedEntry& b) {
            if (a.bank  != b.bank)  return a.bank  < b.bank;
            if (a.flip  != b.flip)  return a.flip  < b.flip;
            if (a.pitch != b.pitch) return a.pitch < b.pitch;
            return a.addr < b.addr;
        });

    if (!write_blob(blob_path, blob))          return 1;
    if (!write_index(index_path, entries, (uint32_t)blob.size())) return 1;

    std::printf("bake-sprites: %zu entries, blob=%zu bytes (%.2f MiB)\n",
        entries.size(), blob.size(), blob.size() / 1048576.0);
    return 0;
}
