#include <cstring> // memcpy
#include <libdragon.h>
#include "globals.hpp"
#include "romloader.hpp"
#include "hwvideo/hwtiles.hpp"
#include "frontend/config.hpp"
#include <cstring>

/***************************************************************************
    Video Emulation: OutRun Tilemap Hardware.
    Based on MAME source code.

    Copyright Aaron Giles.
    All rights reserved.
***************************************************************************/

/*******************************************************************************************
 *
 *  System 16B-style tilemaps
 *
 *  16 total pages
 *  Column/rowscroll enabled via bits in text layer
 *  Alternate tilemap support
 *
 *  Tile format:
 *      Bits               Usage
 *      p------- --------  Tile priority versus sprites
 *      -??----- --------  Unknown
 *      ---ccccc cc------  Tile color palette
 *      ---nnnnn nnnnnnnn  Tile index
 *
 *  Text format:
 *      Bits               Usage
 *      p------- --------  Tile priority versus sprites
 *      -???---- --------  Unknown
 *      ----ccc- --------  Tile color palette
 *      -------n nnnnnnnn  Tile index
 *
 *  Alternate tile format:
 *      Bits               Usage
 *      p------- --------  Tile priority versus sprites
 *      -??----- --------  Unknown
 *      ----cccc ccc-----  Tile color palette
 *      ---nnnnn nnnnnnnn  Tile index
 *
 *  Alternate text format:
 *      Bits               Usage
 *      p------- --------  Tile priority versus sprites
 *      -???---- --------  Unknown
 *      -----ccc --------  Tile color palette
 *      -------- nnnnnnnn  Tile index
 *
 *  Text RAM:
 *      Offset   Bits               Usage
 *      E80      aaaabbbb ccccdddd  Foreground tilemap page select
 *      E82      aaaabbbb ccccdddd  Background tilemap page select
 *      E84      aaaabbbb ccccdddd  Alternate foreground tilemap page select
 *      E86      aaaabbbb ccccdddd  Alternate background tilemap page select
 *      E90      c------- --------  Foreground tilemap column scroll enable
 *               -------v vvvvvvvv  Foreground tilemap vertical scroll
 *      E92      c------- --------  Background tilemap column scroll enable
 *               -------v vvvvvvvv  Background tilemap vertical scroll
 *      E94      -------v vvvvvvvv  Alternate foreground tilemap vertical scroll
 *      E96      -------v vvvvvvvv  Alternate background tilemap vertical scroll
 *      E98      r------- --------  Foreground tilemap row scroll enable
 *               ------hh hhhhhhhh  Foreground tilemap horizontal scroll
 *      E9A      r------- --------  Background tilemap row scroll enable
 *               ------hh hhhhhhhh  Background tilemap horizontal scroll
 *      E9C      ------hh hhhhhhhh  Alternate foreground tilemap horizontal scroll
 *      E9E      ------hh hhhhhhhh  Alternate background tilemap horizontal scroll
 *      F16-F3F  -------- vvvvvvvv  Foreground tilemap per-16-pixel-column vertical scroll
 *      F56-F7F  -------- vvvvvvvv  Background tilemap per-16-pixel-column vertical scroll
 *      F80-FB7  a------- --------  Foreground tilemap per-8-pixel-row alternate tilemap enable
 *               -------h hhhhhhhh  Foreground tilemap per-8-pixel-row horizontal scroll
 *      FC0-FF7  a------- --------  Background tilemap per-8-pixel-row alternate tilemap enable
 *               -------h hhhhhhhh  Background tilemap per-8-pixel-row horizontal scroll
 *
 *******************************************************************************************/

hwtiles::hwtiles(void)
{
    for (int i = 0; i < 2; i++)
        tile_banks[i] = i;

    set_x_clamp(CENTRE);
}

hwtiles::~hwtiles(void)
{

}

// Convert S16 tiles to a more useable format
void hwtiles::init(uint8_t* src_tiles, const bool hires)
{
    if (src_tiles)
    {
        for (int i = 0; i < TILES_LENGTH; i++)
        {
            uint8_t p0 = src_tiles[i];
            uint8_t p1 = src_tiles[i + 0x10000];
            uint8_t p2 = src_tiles[i + 0x20000];

            uint32_t val = 0;

            for (int ii = 0; ii < 8; ii++) 
            {
                uint8_t bit = 7 - ii;
                uint8_t pix = ((((p0 >> bit)) & 1) | (((p1 >> bit) << 1) & 2) | (((p2 >> bit) << 2) & 4));
                val = (val << 4) | pix;
            }
            tiles[i] = val; // Store converted value
        }
        memcpy(tiles_backup, tiles, TILES_LENGTH * sizeof(uint32_t));
    }
    
    if (hires)
    {
        s16_width_noscale = config.s16_width >> 1;
        render8x8_tile_mask      = &hwtiles::render8x8_tile_mask_hires;
        render8x8_tile_mask_clip = &hwtiles::render8x8_tile_mask_clip_hires;
    }
    else
    {
        s16_width_noscale = config.s16_width;
        render8x8_tile_mask      = &hwtiles::render8x8_tile_mask_lores;
        render8x8_tile_mask_clip = &hwtiles::render8x8_tile_mask_clip_lores;
    }
}

// Patch Tileset with new data
void hwtiles::patch_tiles(RomLoader* patch)
{
    memcpy(tiles_backup, tiles, TILES_LENGTH * sizeof(uint32_t));

    for (uint32_t i = 0; i < patch->length;)
    {
        uint32_t tile_index = patch->read16(&i) << 3;
        tiles[tile_index++] = patch->read32(&i);
        tiles[tile_index++] = patch->read32(&i);
        tiles[tile_index++] = patch->read32(&i);
        tiles[tile_index++] = patch->read32(&i);
        tiles[tile_index++] = patch->read32(&i);
        tiles[tile_index++] = patch->read32(&i);
        tiles[tile_index++] = patch->read32(&i);
        tiles[tile_index++] = patch->read32(&i);
    }
}

void hwtiles::restore_tiles()
{
    memcpy(tiles, tiles_backup, TILES_LENGTH * sizeof(uint32_t));
}

// Set Tilemap X Clamp
//
// This is used for the widescreen mode, in order to clamp the tilemap to
// a location of the screen. 
//
// In-Game we must clamp right to avoid page scrolling issues.
//
// The clamp will always be 192 for the non-widescreen mode.
void hwtiles::set_x_clamp(const uint16_t props)
{
    if (props == LEFT)
    {
        x_clamp = 192;
    }
    else if (props == RIGHT)
    {
        x_clamp = (512 - s16_width_noscale);
    }
    else if (props == CENTRE)
    {
        x_clamp = 192 - config.s16_x_off;
    }
}

void hwtiles::update_tile_values()
{
    for (int i = 0; i < 4; i++)
    {
        page[i] = ((text_ram[0xe80 + (i * 2) + 0] << 8) | text_ram[0xe80 + (i * 2) + 1]);

        scroll_x[i] = ((text_ram[0xe98 + (i * 2) + 0] << 8) | text_ram[0xe98 + (i * 2) + 1]);
        scroll_y[i] = ((text_ram[0xe90 + (i * 2) + 0] << 8) | text_ram[0xe90 + (i * 2) + 1]);
    }
}

// A quick and dirty debug function to display the contents of tile memory.
void hwtiles::render_all_tiles(uint16_t* buf)
{
    uint32_t Code = 0, Colour = 5, x, y;
    for (y = 0; y < 224; y += 8) 
    {
        for (x = 0; x < 320; x += 8) 
        {
            (this->*render8x8_tile_mask)(buf, Code, x, y, Colour, 3, 0, TILEMAP_COLOUR_OFFSET);
            Code++;
        }
    }
}

// RDP path: emit one textured-rectangle per visible tile straight into the
// attached framebuffer at (x_offset, y_offset). Mirrors render_tile_layer's
// page/scroll/priority/code decode.
//
// Bypasses rdpq_tex_blit (which emits ~5 RDP commands per tile via the
// tex_loader strip machinery). Instead we set up two tile descriptors once
// per layer and emit only 3 commands per tile:
//   TILE0 (draw): CI4 view of TMEM[0..63], pitch=8, palette slot 0
//   TILE1 (load): I8 view of the same TMEM (RDP's 4bpp loads must go
//                 through an 8bpp view — same trick libdragon's
//                 texload_tile_4bpp uses internally)
// Per-tile sequence: set_texture_image_raw + load_tile(TILE1) +
// texture_rectangle(TILE0). TLUT uploads (which rebind TILE7) don't touch
// TILE0/TILE1, so the per-layer setup survives the whole loop.
void hwtiles::render_rdp_tile_layer(const uint16_t* tile_tlut,
                                    uint8_t page_index, uint8_t priority_draw,
                                    int x_offset, int y_offset)
{
    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);
    rdpq_set_tile(TILE0, FMT_CI4, 0, 8, NULL);
    rdpq_set_tile_size(TILE0, 0, 0, 8, 8);
    rdpq_set_tile(TILE1, FMT_I8,  0, 8, NULL);

    const uint16_t EffPage = page[page_index];
    uint16_t xScroll = scroll_x[page_index];
    uint16_t yScroll = scroll_y[page_index];

    if ((xScroll & 0x8000) != 0)
        xScroll = (text_ram[0xf80 + (0x40 * page_index) + 0] << 8)
                | text_ram[0xf80 + (0x40 * page_index) + 1];
    if ((yScroll & 0x8000) != 0)
        yScroll = (text_ram[0xf16 + (0x40 * page_index) + 0] << 8)
                | text_ram[0xf16 + (0x40 * page_index) + 1];

    // Precompute the visible mx/my window. The CPU path tested every tile
    // in the 64x128 grid; only ~40x28 are actually on screen, so we narrow
    // the outer loops to just the visible tiles. The x/y wrap math below
    // mirrors what the per-tile path does inside the loop.
    const int ox = (x_clamp - xScroll) & 0x3ff; // 0..1023
    const int oy = yScroll & 0x1ff;             // 0..511

    // y(my) = 8*my - oy, with `+= 512` if < -288. Visible range is
    // -7..S16_HEIGHT-1. Each my produces exactly one y; just walk all 64
    // rows but bail fast — y math is cheap and 64 iters is negligible.
    // (Doing a full closed-form solve here is fragile for the wrap edge.)

    // x(mx) similarly. We loop all 128 mx values inside the row loop but
    // *early-exit* on tile_ram priority/code misses cheaply, and skip the
    // expensive rdpq calls on out-of-window tiles.

    // TLUT-upload cache: adjacent tiles often share a palette (sky bands,
    // text strips). Skipping redundant 3-cmd uploads is the main win here.
    int last_colour = -1;

    for (int my = 0; my < 64; my++)
    {
        int y = 8 * my - oy;
        if (y < -288) y += 512;
        if (y <= -8 || y >= S16_HEIGHT) continue;

        for (int mx = 0; mx < 128; mx++)
        {
            uint16_t ActPage = 0;
            if (my < 32 && mx < 64)    ActPage = (EffPage >>  0) & 0x0f;
            if (my < 32 && mx >= 64)   ActPage = (EffPage >>  4) & 0x0f;
            if (my >= 32 && mx < 64)   ActPage = (EffPage >>  8) & 0x0f;
            if (my >= 32 && mx >= 64)  ActPage = (EffPage >> 12) & 0x0f;

            const uint32_t TileIndex =
                64 * 32 * 2 * ActPage + ((2 * 64 * my) & 0xfff) + ((2 * mx) & 0x7f);
            const uint16_t Data =
                (tile_ram[TileIndex + 0] << 8) | tile_ram[TileIndex + 1];

            if (((Data >> 15) & 1) != priority_draw)
                continue;

            uint32_t Code = Data & 0x1fff;
            Code = tile_banks[Code / 0x1000] * 0x1000 + Code % 0x1000;
            Code &= (NUM_TILES - 1);
            if (Code == 0)
                continue;

            int x = 8 * mx - ox;
            if (x < -x_clamp) x += 1024;
            if (x <= -8 || x >= s16_width_noscale) continue;

            const int Colour = (Data >> 6) & 0x7f;

            // Refresh the 16-entry TLUT for this tile palette into TMEM
            // before drawing — only when the palette actually changes from
            // the prior tile. The frame-level cache writeback that gates
            // tile_tlut[] is done once per frame by the renderer.
            if (Colour != last_colour)
            {
                rdpq_tex_upload_tlut((uint16_t*)&tile_tlut[Colour * 16], 0, 16);
                last_colour = Colour;
            }

            // Raw 3-command tile blit. 8x8 CI4 = 32 bytes contiguous in
            // tiles[Code*8]; we view it as 4x8 I8 to satisfy RDP's 4bpp
            // load constraint, then draw via the CI4 tile descriptor.
            rdpq_set_texture_image_raw(
                0, PhysicalAddr(&tiles[Code * 8]), FMT_I8, 4, 8);
            rdpq_load_tile(TILE1, 0, 0, 4, 8);
            rdpq_texture_rectangle(TILE0,
                x + x_offset,     y + y_offset,
                x + x_offset + 8, y + y_offset + 8,
                0, 0);
        }
    }
}

// RDP path for the text layer. Same atlas + TLUT cache as the tile layer, but:
//   * walks the 32x64 text_ram grid instead of tile_ram
//   * no scrolling; tile origin is shifted by -192 (matches the CPU path)
//   * Colour is 3-bit (0..7) — only the first 8 TLUT slots are touched
//   * tile bank is always tile_banks[0] (text codes are 9-bit)
void hwtiles::render_rdp_text_layer(const uint16_t* tile_tlut,
                                    uint8_t priority_draw,
                                    int x_offset, int y_offset)
{
    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);
    rdpq_set_tile(TILE0, FMT_CI4, 0, 8, NULL);
    rdpq_set_tile_size(TILE0, 0, 0, 8, 8);
    rdpq_set_tile(TILE1, FMT_I8,  0, 8, NULL);

    // TLUT-upload cache: text rows often share a palette across long runs
    // (HUD strings are typically one or two colors).
    int last_colour = -1;

    uint32_t TileIndex = 0;
    for (int my = 0; my < 32; my++)
    {
        for (int mx = 0; mx < 64; mx++, TileIndex += 2)
        {
            const uint16_t Code_raw =
                (text_ram[TileIndex + 0] << 8) | text_ram[TileIndex + 1];

            if (((Code_raw >> 15) & 1) != priority_draw)
                continue;

            const int Colour = (Code_raw >> 9) & 0x07;
            uint32_t Code = Code_raw & 0x1ff;
            Code += tile_banks[0] * 0x1000;
            Code &= (NUM_TILES - 1);
            if (Code == 0)
                continue;

            int x = 8 * mx - 192;
            int y = 8 * my;

            // CPU path's outer visibility test (clip path also excluded the
            // 0..7 left strip via `x > -8 && y >= 0`). RDP scissor handles
            // partial-edge tiles, so the same gate is sufficient.
            if (x <= -8 || x >= s16_width_noscale) continue;
            if (y < 0  || y >= S16_HEIGHT)         continue;

            if (Colour != last_colour)
            {
                rdpq_tex_upload_tlut((uint16_t*)&tile_tlut[Colour * 16], 0, 16);
                last_colour = Colour;
            }

            const int dx = x + x_offset + config.s16_x_off;
            const int dy = y + y_offset;
            rdpq_set_texture_image_raw(
                0, PhysicalAddr(&tiles[Code * 8]), FMT_I8, 4, 8);
            rdpq_load_tile(TILE1, 0, 0, 4, 8);
            rdpq_texture_rectangle(TILE0, dx, dy, dx + 8, dy + 8, 0, 0);
        }
    }
}

void hwtiles::render_tile_layer(uint16_t* buf, uint8_t page_index, uint8_t priority_draw)
{
    int16_t Colour, x, y, Priority = 0;

    uint16_t ActPage = 0;
    uint16_t EffPage = page[page_index];
    uint16_t xScroll = scroll_x[page_index];
    uint16_t yScroll = scroll_y[page_index];

    // Need to support this at each row/column
    if ((xScroll & 0x8000) != 0)
        xScroll = (text_ram[0xf80 + (0x40 * page_index) + 0] << 8) | text_ram[0xf80 + (0x40 * page_index) + 1];
    if ((yScroll & 0x8000) != 0)
        yScroll = (text_ram[0xf16 + (0x40 * page_index) + 0] << 8) | text_ram[0xf16 + (0x40 * page_index) + 1];

    for (int my = 0; my < 64; my++) 
    {
        for (int mx = 0; mx < 128; mx++) 
        {
            if (my < 32 && mx < 64)                    // top left
                ActPage = (EffPage >> 0) & 0x0f;
            if (my < 32 && mx >= 64)                   // top right
                ActPage = (EffPage >> 4) & 0x0f;
            if (my >= 32 && mx < 64)                   // bottom left
                ActPage = (EffPage >> 8) & 0x0f;
            if (my >= 32 && mx >= 64)                  // bottom right page
                ActPage = (EffPage >> 12) & 0x0f;

            uint32_t TileIndex = 64 * 32 * 2 * ActPage + ((2 * 64 * my) & 0xfff) + ((2 * mx) & 0x7f);

            uint16_t Data = (tile_ram[TileIndex + 0] << 8) | tile_ram[TileIndex + 1];

            Priority = (Data >> 15) & 1;

            if (Priority == priority_draw) 
            {
                uint32_t Code = Data & 0x1fff;
                Code = tile_banks[Code / 0x1000] * 0x1000 + Code % 0x1000;
                Code &= (NUM_TILES - 1);

                if (Code == 0) continue;

                Colour = (Data >> 6) & 0x7f;

                x = 8 * mx;
                y = 8 * my;

                // We take into account the internal screen resolution here
                // to account for widescreen mode.
                x -= (x_clamp - xScroll) & 0x3ff;

                if (x < -x_clamp)
                    x += 1024;

                y -= yScroll & 0x1ff;

                if (y < -288)
                    y += 512;

                uint16_t ColourOff = TILEMAP_COLOUR_OFFSET;
                if (Colour >= 0x20)
					ColourOff = 0x100 | TILEMAP_COLOUR_OFFSET;
                if (Colour >= 0x40)
					ColourOff = 0x200 | TILEMAP_COLOUR_OFFSET;
                if (Colour >= 0x60)
					ColourOff = 0x300 | TILEMAP_COLOUR_OFFSET;

                if (x > 7 && x < (s16_width_noscale - 8) && y > 7 && y <= (S16_HEIGHT - 8))
                    (this->*render8x8_tile_mask)(buf, Code, x, y, Colour, 3, 0, ColourOff);
                else if (x > -8 && x < s16_width_noscale && y > -8 && y < S16_HEIGHT)
					(this->*render8x8_tile_mask_clip)(buf, Code, x, y, Colour, 3, 0, ColourOff);
            } // end priority check
        }
    } // end for loop
}

void hwtiles::render_text_layer(uint16_t* buf, uint8_t priority_draw)
{
    uint16_t mx, my, Code, Colour, x, y, Priority, TileIndex = 0;

    for (my = 0; my < 32; my++) 
    {
        for (mx = 0; mx < 64; mx++) 
        {
            Code = (text_ram[TileIndex + 0] << 8) | text_ram[TileIndex + 1];
            Priority = (Code >> 15) & 1;

            if (Priority == priority_draw) 
            {
                Colour = (Code >> 9) & 0x07;
                Code &= 0x1ff;
                Code += tile_banks[0] * 0x1000;
                Code &= (NUM_TILES - 1);

                if (Code != 0) 
                {
                    x = 8 * mx;
                    y = 8 * my;

                    x -= 192;

                    // We also adjust the text layer for wide-screen below. But don't allow painting in the 
                    // wide-screen areas to avoid graphical glitches.
                    if (x > 7 && x < (s16_width_noscale - 8) && y > 7 && y <= (S16_HEIGHT - 8))
                        (this->*render8x8_tile_mask)(buf, Code, x + config.s16_x_off, y, Colour, 3, 0, TILEMAP_COLOUR_OFFSET);
                    else if (x > -8 && x < s16_width_noscale && y >= 0 && y < S16_HEIGHT) 
                        (this->*render8x8_tile_mask_clip)(buf, Code, x + config.s16_x_off, y, Colour, 3, 0, TILEMAP_COLOUR_OFFSET);
                }
            }
            TileIndex += 2;
        }
    }
}

void hwtiles::render8x8_tile_mask_lores(
    uint16_t *buf,
    uint16_t nTileNumber, 
    uint16_t StartX, 
    uint16_t StartY, 
    uint16_t nTilePalette, 
    uint16_t nColourDepth, 
    uint16_t nMaskColour, 
    uint16_t nPaletteOffset) 
{
    uint32_t nPalette = (nTilePalette << nColourDepth) | nMaskColour;
    uint32_t* pTileData = tiles + (nTileNumber << 3);
    buf += (StartY * config.s16_width) + StartX;

    for (int y = 0; y < 8; y++) 
    {
        uint32_t p0 = *pTileData;

        if (p0 != nMaskColour) 
        {
            uint32_t c7 = p0 & 0xf;
            uint32_t c6 = (p0 >> 4) & 0xf;
            uint32_t c5 = (p0 >> 8) & 0xf;
            uint32_t c4 = (p0 >> 12) & 0xf;
            uint32_t c3 = (p0 >> 16) & 0xf;
            uint32_t c2 = (p0 >> 20) & 0xf;
            uint32_t c1 = (p0 >> 24) & 0xf;
            uint32_t c0 = (p0 >> 28);

            if (c0) buf[0] = nPalette + c0;
            if (c1) buf[1] = nPalette + c1;
            if (c2) buf[2] = nPalette + c2;
            if (c3) buf[3] = nPalette + c3;
            if (c4) buf[4] = nPalette + c4;
            if (c5) buf[5] = nPalette + c5;
            if (c6) buf[6] = nPalette + c6;
            if (c7) buf[7] = nPalette + c7;
        }
        buf += config.s16_width;
        pTileData++;
    }
}

void hwtiles::render8x8_tile_mask_clip_lores(
    uint16_t *buf,
    uint16_t nTileNumber, 
    int16_t StartX, 
    int16_t StartY, 
    uint16_t nTilePalette, 
    uint16_t nColourDepth, 
    uint16_t nMaskColour, 
    uint16_t nPaletteOffset) 
{
    uint32_t nPalette = (nTilePalette << nColourDepth) | nMaskColour;
    uint32_t* pTileData = tiles + (nTileNumber << 3);
    buf += (StartY * config.s16_width) + StartX;

    for (int y = 0; y < 8; y++) 
    {
        if ((StartY + y) >= 0 && (StartY + y) < S16_HEIGHT) 
        {
            uint32_t p0 = *pTileData;

            if (p0 != nMaskColour) 
            {
                uint32_t c7 = p0 & 0xf;
                uint32_t c6 = (p0 >> 4) & 0xf;
                uint32_t c5 = (p0 >> 8) & 0xf;
                uint32_t c4 = (p0 >> 12) & 0xf;
                uint32_t c3 = (p0 >> 16) & 0xf;
                uint32_t c2 = (p0 >> 20) & 0xf;
                uint32_t c1 = (p0 >> 24) & 0xf;
                uint32_t c0 = (p0 >> 28);

                if (c0 && 0 + StartX >= 0 && 0 + StartX < config.s16_width) buf[0] = nPalette + c0;
                if (c1 && 1 + StartX >= 0 && 1 + StartX < config.s16_width) buf[1] = nPalette + c1;
                if (c2 && 2 + StartX >= 0 && 2 + StartX < config.s16_width) buf[2] = nPalette + c2;
                if (c3 && 3 + StartX >= 0 && 3 + StartX < config.s16_width) buf[3] = nPalette + c3;
                if (c4 && 4 + StartX >= 0 && 4 + StartX < config.s16_width) buf[4] = nPalette + c4;
                if (c5 && 5 + StartX >= 0 && 5 + StartX < config.s16_width) buf[5] = nPalette + c5;
                if (c6 && 6 + StartX >= 0 && 6 + StartX < config.s16_width) buf[6] = nPalette + c6;
                if (c7 && 7 + StartX >= 0 && 7 + StartX < config.s16_width) buf[7] = nPalette + c7;
            }
        }
        buf += config.s16_width;
        pTileData++;
    }
}

// ------------------------------------------------------------------------------------------------
// Additional routines for Hi-Res Mode.
// Note that the tilemaps are displayed at the same resolution, we just want everything to be
// proportional.
// ------------------------------------------------------------------------------------------------
void hwtiles::render8x8_tile_mask_hires(
    uint16_t *buf,
    uint16_t nTileNumber, 
    uint16_t StartX, 
    uint16_t StartY, 
    uint16_t nTilePalette, 
    uint16_t nColourDepth, 
    uint16_t nMaskColour, 
    uint16_t nPaletteOffset) 
{
    uint32_t nPalette = (nTilePalette << nColourDepth) | nMaskColour;
    uint32_t* pTileData = tiles + (nTileNumber << 3);
    buf += ((StartY << 1) * config.s16_width) + (StartX << 1);

    for (int y = 0; y < 8; y++) 
    {
        uint32_t p0 = *pTileData;

        if (p0 != nMaskColour) 
        {
            uint32_t c7 = p0 & 0xf;
            uint32_t c6 = (p0 >> 4) & 0xf;
            uint32_t c5 = (p0 >> 8) & 0xf;
            uint32_t c4 = (p0 >> 12) & 0xf;
            uint32_t c3 = (p0 >> 16) & 0xf;
            uint32_t c2 = (p0 >> 20) & 0xf;
            uint32_t c1 = (p0 >> 24) & 0xf;
            uint32_t c0 = (p0 >> 28);

            if (c0) set_pixel_x4(&buf[0],  nPalette + c0);
            if (c1) set_pixel_x4(&buf[2],  nPalette + c1);
            if (c2) set_pixel_x4(&buf[4],  nPalette + c2);
            if (c3) set_pixel_x4(&buf[6],  nPalette + c3);
            if (c4) set_pixel_x4(&buf[8],  nPalette + c4);
            if (c5) set_pixel_x4(&buf[10], nPalette + c5);
            if (c6) set_pixel_x4(&buf[12], nPalette + c6);
            if (c7) set_pixel_x4(&buf[14], nPalette + c7);
        }
        buf += (config.s16_width << 1);
        pTileData++;
    }
}

void hwtiles::render8x8_tile_mask_clip_hires(
    uint16_t *buf,
    uint16_t nTileNumber, 
    int16_t StartX, 
    int16_t StartY, 
    uint16_t nTilePalette, 
    uint16_t nColourDepth, 
    uint16_t nMaskColour, 
    uint16_t nPaletteOffset) 
{
    uint32_t nPalette = (nTilePalette << nColourDepth) | nMaskColour;
    uint32_t* pTileData = tiles + (nTileNumber << 3);
    buf += ((StartY << 1) * config.s16_width) + (StartX << 1);

    for (int y = 0; y < 8; y++) 
    {
        if ((StartY + y) >= 0 && (StartY + y) < S16_HEIGHT) 
        {
            uint32_t p0 = *pTileData;

            if (p0 != nMaskColour) 
            {
                uint32_t c7 = p0 & 0xf;
                uint32_t c6 = (p0 >> 4) & 0xf;
                uint32_t c5 = (p0 >> 8) & 0xf;
                uint32_t c4 = (p0 >> 12) & 0xf;
                uint32_t c3 = (p0 >> 16) & 0xf;
                uint32_t c2 = (p0 >> 20) & 0xf;
                uint32_t c1 = (p0 >> 24) & 0xf;
                uint32_t c0 = (p0 >> 28);

                if (c0 && 0 + StartX >= 0 && 0 + StartX < s16_width_noscale) set_pixel_x4(&buf[0],  nPalette + c0);
                if (c1 && 1 + StartX >= 0 && 1 + StartX < s16_width_noscale) set_pixel_x4(&buf[2],  nPalette + c1);
                if (c2 && 2 + StartX >= 0 && 2 + StartX < s16_width_noscale) set_pixel_x4(&buf[4],  nPalette + c2);
                if (c3 && 3 + StartX >= 0 && 3 + StartX < s16_width_noscale) set_pixel_x4(&buf[6],  nPalette + c3);
                if (c4 && 4 + StartX >= 0 && 4 + StartX < s16_width_noscale) set_pixel_x4(&buf[8],  nPalette + c4);
                if (c5 && 5 + StartX >= 0 && 5 + StartX < s16_width_noscale) set_pixel_x4(&buf[10], nPalette + c5);
                if (c6 && 6 + StartX >= 0 && 6 + StartX < s16_width_noscale) set_pixel_x4(&buf[12], nPalette + c6);
                if (c7 && 7 + StartX >= 0 && 7 + StartX < s16_width_noscale) set_pixel_x4(&buf[14], nPalette + c7);
            }
        }
        buf += (config.s16_width << 1);
        pTileData++;
    }
}

// Hires Mode: Set 4 pixels instead of one.
void hwtiles::set_pixel_x4(uint16_t *buf, uint32_t data)
{
    buf[0] = buf[1] = buf[0  + config.s16_width] = buf[1 + config.s16_width] = data;
}
