// bake-sprites — host-side asset pipeline tool.
//
// Reads the OutRun sprite + tile ROM set and emits two N64-ready native
// blobs that the runtime PI-DMAs at decode-on-miss time:
//
//   sprites_native.bin (1 MiB) — the byte-swapped uint32_t sprite ROM
//     (matches hwsprites::init exactly). Big-endian on disk so a PI-DMA
//     into BE N64 RAM reproduces hwsprites::sprites[] bit-for-bit.
//   tiles_native.bin   (256 KiB) — the bitplane → CI4 pre-decoded tile
//     ROM (matches hwtiles::init() exactly). Same BE-on-disk convention.
//
// The pre-decoded sprite atlas (sprite_atlas.bin + sprite_atlas_index.c)
// was removed: the runtime atlas hash cache decodes on first miss and
// amortizes the ~500us EOR walk across subsequent frames. See
// hwsprites::atlas_get_or_extract.
//
// Usage:
//   bake-sprites --roms <roms_dir>
//                --sprites-blob <out.bin> --tiles-blob <out.bin>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "frontend/config.hpp"
#include "roms.hpp"

static const uint32_t SPRITES_LENGTH = 0x100000u >> 2;   // 256 K words

// Mirrors hwtiles::init() — three 64 KiB bitplanes at offsets 0x00000 /
// 0x10000 / 0x20000 of the 0x30000-byte tile ROM, decoded into a 256 KiB
// uint32_t array indexed by [i], i ∈ [0, 0x10000). Each output word
// packs 8 CI4 pixels (leftmost in the high nibble of byte 0).
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

static bool write_be_words(const std::string& path,
                           const uint32_t* words, size_t count)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "bake-sprites: cannot open %s\n", path.c_str());
        return false;
    }
    std::vector<uint8_t> be(count * 4);
    for (size_t i = 0; i < count; i++) {
        const uint32_t w = words[i];
        be[i*4 + 0] = (uint8_t)((w >> 24) & 0xff);
        be[i*4 + 1] = (uint8_t)((w >> 16) & 0xff);
        be[i*4 + 2] = (uint8_t)((w >>  8) & 0xff);
        be[i*4 + 3] = (uint8_t)( w        & 0xff);
    }
    f.write((const char*)be.data(), (std::streamsize)be.size());
    if (!f) {
        std::fprintf(stderr, "bake-sprites: write failed %s\n", path.c_str());
        return false;
    }
    return true;
}

int main(int argc, char** argv)
{
    std::string roms_dir, sprites_blob_path, tiles_blob_path;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* who) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "bake-sprites: %s missing arg\n", who); std::exit(2); }
            return std::string(argv[++i]);
        };
        if      (a == "--roms")          roms_dir          = next("--roms");
        else if (a == "--sprites-blob")  sprites_blob_path = next("--sprites-blob");
        else if (a == "--tiles-blob")    tiles_blob_path   = next("--tiles-blob");
        else { std::fprintf(stderr, "bake-sprites: unknown arg %s\n", a.c_str()); return 2; }
    }
    if (roms_dir.empty() || sprites_blob_path.empty() || tiles_blob_path.empty()) {
        std::fprintf(stderr,
            "usage: bake-sprites --roms <dir>"
            " --sprites-blob <out.bin> --tiles-blob <out.bin>\n");
        return 2;
    }

    if (!roms_dir.empty() && roms_dir.back() != '/') roms_dir += '/';
    config.data.rom_path = roms_dir;
    if (!roms.load_revb_roms(false)) {
        std::fprintf(stderr, "bake-sprites: ROM load failed (path=%s)\n", roms_dir.c_str());
        return 1;
    }

    // load_revb_roms() does not load sprite/tile ROMs — the N64 runtime
    // consumes them as DFS blobs emitted by this tool — so load them here.
    roms.sprites.init(0x100000);
    int status = 0;
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

    // Byte-swap sprite ROM into the runtime hwsprites::sprites[] layout.
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
    if (!write_be_words(sprites_blob_path, sprites_words.data(), SPRITES_LENGTH))
        return 1;

    std::vector<uint32_t> tiles_words;
    bake_tiles_words(roms.tiles.rom, tiles_words);
    if (!write_be_words(tiles_blob_path, tiles_words.data(), tiles_words.size()))
        return 1;

    std::printf("bake-sprites: sprites_native.bin (%zu bytes) + tiles_native.bin (%zu bytes)\n",
                SPRITES_LENGTH * 4, tiles_words.size() * 4);
    return 0;
}
