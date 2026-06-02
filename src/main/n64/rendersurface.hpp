/***************************************************************************
    N64 / libdragon Render.

    Produces a scratch RGBA5551 surface from the engine's palette-indexed
    pixel buffer, then rdpq_tex_blit's it into the display framebuffer with
    a vertical letterbox.
***************************************************************************/

#pragma once

#include "../stdint.hpp"
#include "../globals.hpp"
#include <libdragon.h>
#include <rdpq_font.h>

// Per-phase timings (microseconds, EMA-smoothed). Updated by n64main /
// video.cpp and consumed by the FPS overlay.
namespace n64_profile
{
    extern uint32_t prepare_us;   // total prepare_frame
    extern uint32_t render_us;    // palette expand + RDP blit + present
    extern uint32_t tick_us;      // engine tick (excludes rasterize)
    extern uint32_t palette_us;   // just the palette-expand loop + writeback
    extern uint32_t wait_us;      // display_get() vsync block

    enum {
        SUB_ROAD_BG,
        SUB_TILE_BG,
        SUB_TILE_FG,
        SUB_ROAD_FG,
        SUB_SPRITE,
        SUB_TEXT,
        SUB_COUNT
    };
    extern uint32_t sub_us[SUB_COUNT];
}

class Render
{
public:
    Render();
    ~Render();

    bool init(int src_width, int src_height,
              int scale, int video_mode, int scanlines);
    void disable();
    bool start_frame();
    bool finalize_frame();
    void draw_frame(uint16_t* pixels);

    // Palette plumbing — Video::refresh_palette() calls convert_palette()
    // when S16 palette RAM changes; set_shadow_intensity() scales the shadow
    // half of the LUT at init.
    void convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1);
    void set_shadow_intensity(float f);

private:
    // Palette Lookup — held as RGBA5551 (libdragon's DEPTH_16_BPP framebuffer
    // format). Extended slots hold shadow colors at +S16_PALETTE_ENTRIES.
    uint16_t rgb[S16_PALETTE_ENTRIES * 2];

    // Tile-palette TLUT cache: 128 contiguous 16-entry RGBA5551 TLUTs that
    // mirror the engine tile palette layout. Engine palette entry E is read
    // by the tile renderer as (nTilePalette<<3) + c where c in [0..15], so
    // each slot i covers entries [i*8 .. i*8+15] (overlapping slots share 8
    // entries). Slot i is a contiguous 16-entry RGBA5551 block at offset
    // i*16, with entry 0 forced to 0 (alpha LSB clear) — the RDP alpha-
    // compare composite then drops CI4 pixval=0 to transparent, matching
    // the CPU mask path's `if (c0) buf[0] = nPalette + c0;`.
    // 8-byte aligned so rdpq_tex_upload_tlut can DMA from any slot directly.
    static constexpr int TILE_TLUT_SLOTS = 128;
    static constexpr int TILE_TLUT_SLOT_SIZE = 16;
    alignas(8) uint16_t tile_tlut[TILE_TLUT_SLOTS * TILE_TLUT_SLOT_SIZE];

    // Source S16 buffer dimensions (eg. 320x224).
    int src_width, src_height;
    int video_mode;
    int scanlines;
    int scale;

    // Shadow intensity multiplier (0..255).
    int shadow_multi;

    // Scratch RGBA5551 surface that draw_frame() populates and
    // finalize_frame() blits.
    uint16_t*  scratch_pixels;
    surface_t  scratch_surface;
    int        y_offset;          // letterbox top (pixels)

    // RDP-drawn FPS overlay (builtin libdragon debug font).
    rdpq_font_t* fps_font;
    static constexpr uint8_t FPS_FONT_ID = 1;

    bool       initialized;
};
