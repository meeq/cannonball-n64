#pragma once

#include "stdint.hpp"

class RomLoader;

class hwtiles
{
public:
    enum
    {
        LEFT,
        RIGHT,
        CENTRE,
    };

    uint8_t text_ram[0x1000]; // Text RAM
    uint8_t tile_ram[0x10000]; // Tile RAM

    hwtiles(void);
    ~hwtiles(void);

    void init(uint8_t* src_tiles, const bool hires);
    void patch_tiles(RomLoader* patch);
    void restore_tiles();
    void set_x_clamp(const uint16_t);
    void update_tile_values();
    void render_tile_layer(uint16_t*, uint8_t, uint8_t);
    void render_text_layer(uint16_t*, uint8_t);
    void render_all_tiles(uint16_t*);

    // RDP tile-layer renderer: walks both BG (page=1) and FG (page=0) tilemaps
    // in a single pass that shares the atlas ring, unique-code map, and chunk
    // metadata. Equivalent to two render_rdp_tile_layer calls in BG→FG order
    // but with one decode pass and atlas chunks that can pack BG+FG uniques
    // together (so a chunk that ends mid-BG keeps filling with early FG tiles
    // instead of starting a new chunk per page). Within any chunk that spans
    // both pages, visibles are emitted in collection order (BG before FG) so
    // FG correctly draws over BG. Requires the renderer to have attached the
    // display and to pass its per-palette TLUT cache.
    void render_rdp_tile_layers(const uint16_t* tile_tlut,
                                uint8_t priority_draw,
                                int x_offset, int y_offset);

    // RDP text-layer renderer: walks the 32x64 text_ram grid (no scrolling,
    // no page select) and emits one textured-rectangle per visible tile.
    // Same atlas + TLUT cache as render_rdp_tile_layers; only the first 8
    // TLUT slots are touched (text Colour is 3-bit).
    void render_rdp_text_layer(const uint16_t* tile_tlut,
                               uint8_t priority_draw,
                               int x_offset, int y_offset);

private:
    int16_t x_clamp;
    
    // S16 Width, ignoring widescreen related scaling.
    uint16_t s16_width_noscale;

    static const int TILES_LENGTH = 0x10000;
    // Aligned to 8 bytes so rdpq DMAs can read tile rows directly from this
    // array as a CI4 atlas — each tile_idx*8 uint32_t row holds 8 nibbles
    // (leftmost pixel in the high nibble of byte 0), which is the exact
    // CI4 byte layout the RDP expects on a big-endian N64.
    alignas(8) uint32_t tiles[TILES_LENGTH];        // Converted tiles
    alignas(8) uint32_t tiles_backup[TILES_LENGTH]; // Converted tiles (backup without patch)

    uint16_t page[4];
    uint16_t scroll_x[4];
    uint16_t scroll_y[4];

    uint8_t tile_banks[2];

    static const uint16_t NUM_TILES = 0x2000; // Length of graphic rom / 24
    static const uint16_t TILEMAP_COLOUR_OFFSET = 0x1c00;
    
    void (hwtiles::*render8x8_tile_mask)(
        uint16_t *buf,
        uint16_t nTileNumber, 
        uint16_t StartX, 
        uint16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 
        
    void (hwtiles::*render8x8_tile_mask_clip)(
        uint16_t *buf,
        uint16_t nTileNumber, 
        int16_t StartX, 
        int16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 
        
    void render8x8_tile_mask_lores(
        uint16_t *buf,
        uint16_t nTileNumber, 
        uint16_t StartX, 
        uint16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 

    void render8x8_tile_mask_clip_lores(
        uint16_t *buf,
        uint16_t nTileNumber, 
        int16_t StartX, 
        int16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset);
        
    void render8x8_tile_mask_hires(
        uint16_t *buf,
        uint16_t nTileNumber, 
        uint16_t StartX, 
        uint16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 
        
    void render8x8_tile_mask_clip_hires(
        uint16_t *buf,
        uint16_t nTileNumber, 
        int16_t StartX, 
        int16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset);
        
    inline void set_pixel_x4(uint16_t *buf, uint32_t data);
};