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

int main(int argc, char** argv)
{
    std::string roms_dir, blob_path, index_path, sprites_blob_path;

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
        else { std::fprintf(stderr, "bake-sprites: unknown arg %s\n", a.c_str()); return 2; }
    }
    if (roms_dir.empty() || blob_path.empty() || index_path.empty()
        || sprites_blob_path.empty()) {
        std::fprintf(stderr,
            "usage: bake-sprites --roms <dir> --blob <out.bin> --index <out.c>"
            " --sprites-blob <out.bin>\n");
        return 2;
    }

    // Mirror runtime ROM load path.
    if (!roms_dir.empty() && roms_dir.back() != '/') roms_dir += '/';
    config.data.rom_path = roms_dir;
    if (!roms.load_revb_roms(false)) {
        std::fprintf(stderr, "bake-sprites: ROM load failed (path=%s)\n", roms_dir.c_str());
        return 1;
    }
    // load_revb_roms() leaves rom0p/rom1p NULL — the runtime selects via
    // outrun.select_course(). For the bake we always use the W (revb) set.
    roms.rom0p = &roms.rom0;
    roms.rom1p = &roms.rom1;

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

    // Walk every (bank, addr, pitch, max_h) tuple statically from ROM.
    std::vector<StaticTuple> tuples;
    walk_static_addrs(roms, sprites_words, tuples);
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
