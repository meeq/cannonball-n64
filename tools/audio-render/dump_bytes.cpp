// Tiny one-off helper: dump N bytes of z80 ROM at a given address.
//   dump-bytes <addr-hex> <count> [--rom-path PATH]
#include <cstdio>
#include <cstdlib>
#include <string>
#include "frontend/config.hpp"
#include "roms.hpp"

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: dump-bytes <addr> <count> [--rom-path P]\n"); return 1; }
    uint16_t addr = (uint16_t)std::strtoul(argv[1], nullptr, 0);
    int count = (int)std::strtoul(argv[2], nullptr, 0);
    std::string rom_path = "roms/";
    for (int i = 3; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--rom-path" && i + 1 < argc) rom_path = argv[++i];
    }
    config.data.rom_path = rom_path;
    if (!roms.load_revb_roms(false)) { std::fprintf(stderr, "ROM load failed\n"); return 2; }
    for (int i = 0; i < count; ++i)
    {
        if (i % 16 == 0) std::printf("\n%04X:", (unsigned)(addr + i));
        std::printf(" %02X", roms.z80.read8((uint16_t)(addr + i)));
    }
    std::printf("\n");
    return 0;
}
