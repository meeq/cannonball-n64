/***************************************************************************
    Load OutRun ROM Set.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#pragma once

#include "romloader.hpp"

class Roms
{
public:
    // Master / Slave CPU ROMs. Hold either World (Rev B) or Japanese chip
    // data; load_japanese_roms swaps the contents in place so only one
    // region is resident at a time. Sub-CPU sound (z80), road, and PCM are
    // region-agnostic and live in their own RomLoaders below.
    RomLoader rom0;
    RomLoader rom1;
    RomLoader tiles;
    RomLoader sprites;
    RomLoader road;
    RomLoader z80;
    RomLoader pcm;

    // Paged ROM aliases. Always point at rom0 / rom1; the indirection is
    // kept so engine code that reads region-aware data still goes through
    // the rom0p / rom1p path the rest of the codebase expects.
    RomLoader* rom0p;
    RomLoader* rom1p;

    Roms();
    ~Roms();
    bool load_revb_roms(bool);
    // Overwrites rom0 / rom1 with the Japanese chip data, leaving the
    // region-agnostic ROMs (road / z80 / pcm) untouched. Idempotent — a
    // second call (eg. boot-menu re-entry) reloads the same files.
    bool load_japanese_roms();
    int load_pcm_rom(bool);
    bool load_ym_data(const char* filename);

private:
    const static bool VERBOSE = true;
};

extern Roms roms;