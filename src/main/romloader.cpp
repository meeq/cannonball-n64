/***************************************************************************
    Binary File Loader. 
    
    Handles loading an individual binary file to memory.
    Supports reading bytes, words and longs from this area of memory.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include <iostream>
#include <fstream>
#include <cstddef>       // for std::size_t

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

    // Open rom file
    std::ifstream src(path.c_str(), std::ios::in | std::ios::binary);
    if (!src)
    {
        if (verbose) std::cout << "cannot open rom: " << path << std::endl;
        loaded = false;
        return 1; // fail
    }

    // Read file
    char* buffer = new char[length];
    src.read(buffer, length);

    // Check CRC on file
    Crc32 result;
    result.process_bytes(buffer, (size_t) src.gcount());

    if (expected_crc != result.checksum())
    {
        if (verbose) 
        std::cout << std::hex << 
            filename << " has incorrect checksum.\nExpected: " << expected_crc << " Found: " << result.checksum() << std::endl;

        return 1;
    }

    // Interleave file as necessary
    for (int i = 0; i < length; i++)
    {
        rom[(i * interleave) + offset] = buffer[i];
    }

    // Clean Up
    delete[] buffer;
    src.close();
    loaded = true;
    return 0; // success
}

// --------------------------------------------------------------------------------------------
// Load Binary File (LayOut Levels, Tilemap Data etc.)
// --------------------------------------------------------------------------------------------

int RomLoader::load_binary(const char* filename)
{
    std::ifstream src(filename, std::ios::in | std::ios::binary);
    if (!src)
    {
        std::cout << "cannot open file: " << filename << std::endl;
        loaded = false;
        return 1; // fail
    }

    length = filesize(filename);

    // Read file
    char* buffer = new char[length];
    src.read(buffer, length);
    rom = (uint8_t*) buffer;

    // Clean Up
    src.close();

    loaded = true;
    return 0; // success
}

int RomLoader::filesize(const char* filename)
{
    std::ifstream in(filename, std::ifstream::in | std::ifstream::binary);
    in.seekg(0, std::ifstream::end);
    int size = (int) in.tellg();
    in.close();
    return size; 
}