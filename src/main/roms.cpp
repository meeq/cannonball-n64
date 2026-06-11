/***************************************************************************
    Load OutRun ROM Set.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include <iostream>
#include <cstring>
#include "stdint.hpp"
#include "roms.hpp"

Roms roms;

Roms::Roms()
{
    jap_rom_status = -1;
    rom0p = NULL;
    rom1p = NULL;
}

Roms::~Roms(void)
{
}

#define LOAD(rom, args) rom.load_rom args

bool Roms::load_revb_roms(bool fixed_rom)
{
    // If incremented, a rom has failed to load.
    int status = 0;

    // Load Master CPU ROMs
    rom0.init(0x40000);
    status += LOAD(rom0, ("epr-10380b.133", 0x00000, 0x10000, 0x1f6cadad, RomLoader::INTERLEAVE2, VERBOSE));
    status += LOAD(rom0, ("epr-10382b.118", 0x00001, 0x10000, 0xc4c3fa1a, RomLoader::INTERLEAVE2, VERBOSE));
    status += LOAD(rom0, ("epr-10381b.132", 0x20000, 0x10000, 0xbe8c412b, RomLoader::INTERLEAVE2, VERBOSE));
    status += LOAD(rom0, ("epr-10383b.117", 0x20001, 0x10000, 0x10a2014a, RomLoader::INTERLEAVE2, VERBOSE));

    // Load Slave CPU ROMs
    rom1.init(0x40000);
    status += LOAD(rom1, ("epr-10327a.76", 0x00000, 0x10000, 0xe28a5baf, RomLoader::INTERLEAVE2, VERBOSE));
    status += LOAD(rom1, ("epr-10329a.58", 0x00001, 0x10000, 0xda131c81, RomLoader::INTERLEAVE2, VERBOSE));
    status += LOAD(rom1, ("epr-10328a.75", 0x20000, 0x10000, 0xd5ec5e5d, RomLoader::INTERLEAVE2, VERBOSE));
    status += LOAD(rom1, ("epr-10330a.57", 0x20001, 0x10000, 0xba9ec82a, RomLoader::INTERLEAVE2, VERBOSE));

    // Tile ROMs are not loaded on N64. The bake-sprites tool pre-decodes
    // them into /tiles/tiles_native.bin (DFS), and hwtiles::init no longer
    // reads src_tiles. Keeping the 192 KiB blob out of RAM frees that much
    // from the load-time peak (was OOM on the 4 MiB base console).

    // Load Non-Interleaved Road ROMs (2 identical roms, 1 for each road)
    road.init(0x10000);
    status += LOAD(road, ("opr-10185.11", 0x000000, 0x08000, 0x22794426, RomLoader::NORMAL, VERBOSE));
    status += LOAD(road, ("opr-10186.47", 0x008000, 0x08000, 0x22794426, RomLoader::NORMAL, VERBOSE));

    // Sprite ROMs are not loaded on N64. The bake-sprites tool emits both
    // a pre-decoded atlas (sprite_atlas.bin) and a native byte-swapped blob
    // (sprites_native.bin) for the EOR-walk spillover path; hwsprites::init
    // no longer reads src_sprites. Skipping the 1 MiB blob is the single
    // biggest reclaim on the 4 MiB base console's load-time peak.

    // Load Z80 Sound ROM
    // Note: This is a deliberate decision to double the Z80 ROM Space to accomodate extra FM based music
    z80.init(0x10000);
    status += LOAD(z80, ("epr-10187.88", 0x0000, 0x08000, 0xa10abaa9, RomLoader::NORMAL, VERBOSE));

    // Load Sega PCM Chip Samples
    pcm.init(0x60000);
    status += LOAD(pcm, ("opr-10193.66", 0x00000, 0x08000, 0xbcd10dde, RomLoader::NORMAL, VERBOSE));
    status += LOAD(pcm, ("opr-10192.67", 0x10000, 0x08000, 0x770f1270, RomLoader::NORMAL, VERBOSE));
    status += LOAD(pcm, ("opr-10191.68", 0x20000, 0x08000, 0x20a284ab, RomLoader::NORMAL, VERBOSE));
    status += LOAD(pcm, ("opr-10190.69", 0x30000, 0x08000, 0x7cab70e2, RomLoader::NORMAL, VERBOSE));
    status += LOAD(pcm, ("opr-10189.70", 0x40000, 0x08000, 0x01366b54, RomLoader::NORMAL, VERBOSE));
    status += LOAD(pcm, ("opr-10188.71", 0x50000, 0x08000, 0xbad30ad9, RomLoader::NORMAL, VERBOSE));
    status += load_pcm_rom(fixed_rom);

    // If status has been incremented, a rom has failed to load.
    return status == 0;
}

bool Roms::load_japanese_roms()
{
    // Only attempt to initalize the arrays once.
    if (jap_rom_status == -1)
    {
        j_rom0.init(0x40000);
        j_rom1.init(0x40000);
    }

    // If incremented, a rom has failed to load.
    jap_rom_status = 0;

    // Load Master CPU ROMs     
    jap_rom_status += LOAD(j_rom0, ("epr-10380.133", 0x00000, 0x10000, 0xe339e87a, RomLoader::INTERLEAVE2, VERBOSE));
    jap_rom_status += LOAD(j_rom0, ("epr-10382.118", 0x00001, 0x10000, 0x65248dd5, RomLoader::INTERLEAVE2, VERBOSE));
    jap_rom_status += LOAD(j_rom0, ("epr-10381.132", 0x20000, 0x10000, 0xbe8c412b, RomLoader::INTERLEAVE2, VERBOSE));
    jap_rom_status += LOAD(j_rom0, ("epr-10383.117", 0x20001, 0x10000, 0xdcc586e7, RomLoader::INTERLEAVE2, VERBOSE));

    // Load Slave CPU ROMs        
    jap_rom_status += LOAD(j_rom1, ("epr-10327.76", 0x00000, 0x10000, 0xda99d855, RomLoader::INTERLEAVE2, VERBOSE));
    jap_rom_status += LOAD(j_rom1, ("epr-10329.58", 0x00001, 0x10000, 0xfe0fa5e2, RomLoader::INTERLEAVE2, VERBOSE));
    jap_rom_status += LOAD(j_rom1, ("epr-10328.75", 0x20000, 0x10000, 0x3c0e9a7f, RomLoader::INTERLEAVE2, VERBOSE));
    jap_rom_status += LOAD(j_rom1, ("epr-10330.57", 0x20001, 0x10000, 0x59786e99, RomLoader::INTERLEAVE2, VERBOSE));
    // If status has been incremented, a rom has failed to load.
    return jap_rom_status == 0;
}

int Roms::load_pcm_rom(bool fixed_rom)
{
    int status = 0;
    if (fixed_rom)
    {
        status = LOAD(pcm, ("opr-10188.71f", 0x50000, 0x08000, 0x37598616, RomLoader::NORMAL, false));
        if (status == 1)
            status = LOAD(pcm, ("opr-10188.71f", 0x50000, 0x08000, 0xC2DE09B2, RomLoader::NORMAL, VERBOSE));
    }
    else
    {
        status = LOAD(pcm, ("opr-10188.71", 0x50000, 0x08000, 0xbad30ad9, RomLoader::NORMAL, VERBOSE));
    }

    //return LOAD(pcm, (fixed_rom ? "opr-10188.71f" : "opr-10188.71", 0x50000, 0x08000, fixed_rom ? 0x37598616 : 0xbad30ad9, RomLoader::NORMAL)) == 0;
    return status;
}

bool Roms::load_ym_data(const char* filename)
{
    RomLoader data;
    if (data.load_binary(filename) == 0)
    {
        if (data.length < 0x8000)
        {
            memcpy(z80.rom + 0x8000, data.rom, data.length);
            data.unload();
            return true;
        }
        else
        {
            std::cout << "YM Data is too large: " << filename << std::endl;
        }
    }
    return false;
}