/***************************************************************************
    Binary File Loader.

    Handles loading an individual binary file to memory.
    Supports reading bytes, words and longs from this area of memory.

    Copyright Chris White.
    See license.txt for more details.

    N64 port: replaced <fstream>/<iostream> with C stdio. The newlib + DFS
    integration in libdragon routes fopen("rom:/...") through the DFS
    filesystem driver, so the cart-side asset semantics are preserved.
    Dropping iostream alone reclaims ~80 KiB of .text (std::time_get /
    std::money_get / locale / strftime machinery that <fstream> drags in).
***************************************************************************/

#include <cstdio>
#include <cstring>
#include <libdragon.h>

#include "stdint.hpp"
#include "romloader.hpp"
#include "frontend/config.hpp"
#include "utils_crc32.hpp"


RomLoader::RomLoader()
{
    rom = NULL;
    loaded = false;
}

RomLoader::~RomLoader()
{
    if (rom != NULL)
        delete[] rom;
}

void RomLoader::init(const uint32_t length)
{
    this->length = length;
    rom = new uint8_t[length];
}

void RomLoader::unload(void)
{
    delete[] rom;
    rom = NULL;
}

// ------------------------------------------------------------------------------------------------
// Filename based ROM loader
// Advantage: Simpler. Does not require <dirent.h>
// ------------------------------------------------------------------------------------------------

int RomLoader::load_rom(const char* filename, const int offset, const int length, const int expected_crc, const uint8_t interleave, const bool verbose)
{
    std::string path = config.data.rom_path;
    path += std::string(filename);

    FILE* src = std::fopen(path.c_str(), "rb");
    if (!src)
    {
        if (verbose) debugf("cannot open rom: %s\n", path.c_str());
        loaded = false;
        return 1; // fail
    }

    char* buffer = new char[length];
    const size_t bytes_read = std::fread(buffer, 1, length, src);

    Crc32 result;
    result.process_bytes(buffer, bytes_read);

    if (expected_crc != (int)result.checksum())
    {
        if (verbose)
            debugf("%s has incorrect checksum.\nExpected: %x Found: %x\n",
                   filename, expected_crc, result.checksum());
        delete[] buffer;
        std::fclose(src);
        return 1;
    }

    for (int i = 0; i < length; i++)
        rom[(i * interleave) + offset] = buffer[i];

    delete[] buffer;
    std::fclose(src);
    loaded = true;
    return 0; // success
}

// --------------------------------------------------------------------------------------------
// Load Binary File (LayOut Levels, Tilemap Data etc.)
// --------------------------------------------------------------------------------------------

int RomLoader::load_binary(const char* filename)
{
    FILE* src = std::fopen(filename, "rb");
    if (!src)
    {
        debugf("cannot open file: %s\n", filename);
        loaded = false;
        return 1; // fail
    }

    length = filesize(filename);

    char* buffer = new char[length];
    std::fread(buffer, 1, length, src);
    rom = (uint8_t*) buffer;

    std::fclose(src);

    loaded = true;
    return 0; // success
}

int RomLoader::filesize(const char* filename)
{
    FILE* in = std::fopen(filename, "rb");
    if (!in) return 0;
    std::fseek(in, 0, SEEK_END);
    const int size = (int) std::ftell(in);
    std::fclose(in);
    return size;
}
