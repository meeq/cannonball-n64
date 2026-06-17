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

    // Engine hook — call from any path that writes tile_ram. The BG/FG
    // tile_cache holds a CI4-rendered snapshot of the tilemap keyed only
    // by (EffPage, tile_banks); writes that modify tile_ram WITHOUT
    // changing those keys (e.g. Time Trials' music-select state filling
    // in animated cells over a static page) would silently leave the
    // cache showing the pre-write tilemap. This sets a dirty flag that
    // update_and_blit checks alongside the shadow keys, forcing a rebuild
    // on the next render frame. Cheap (one bool write) so it's fine to
    // call from every per-cell tile_ram write site.
    static void mark_tile_cache_dirty();

private:
    int16_t x_clamp;
    
    // S16 Width, ignoring widescreen related scaling.
    uint16_t s16_width_noscale;

    static const int TILES_LENGTH = 0x10000;
    // The legacy SDL build holds a 256 KiB resident CI4 tile array (built
    // by bitplane-decoding the 192 KiB raw tile ROM at init time). On N64
    // the conversion is done offline by the bake-sprites tool, which emits
    // /tiles/tiles_native.bin into DFS — a 256 KiB CI4 blob laid out as
    // uint32_t [TILES_LENGTH], big-endian on disk so PI-DMA reproduces the
    // array bit-for-bit in BE N64 RAM. render_rdp_tile_layers and
    // render_rdp_text_layer PI-DMA 32 bytes (8 rows × 4 B) per unique tile
    // from this blob into a per-call scratch instead of indexing tiles[].
    //
    // tiles_pi_addr is resolved at init() via dfs_rom_addr; 0 = unresolved
    // / missing payload. patch_tiles / restore_tiles are stubbed on N64
    // (widescreen is hardcoded off), so the blob is truly read-only.
    uint32_t tiles_pi_addr;

    uint16_t page[4];
    uint16_t scroll_x[4];
    uint16_t scroll_y[4];

    uint8_t tile_banks[2];

    static const uint16_t NUM_TILES = 0x2000; // Length of graphic rom / 24
    static const uint16_t TILEMAP_COLOUR_OFFSET = 0x1c00;
};