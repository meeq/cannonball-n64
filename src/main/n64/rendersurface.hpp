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
    extern uint32_t render_us;    // RDP blit + present
    extern uint32_t tick_us;      // engine tick (excludes rasterize)
    extern uint32_t wait_us;      // display_get() vsync block
    extern uint32_t audio_us;     // audio.tick (Z80 catchup + mixer_poll)

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

    // Palette plumbing — Video::refresh_palette() calls convert_palette()
    // when S16 palette RAM changes; set_shadow_intensity() scales the shadow
    // half of the LUT at init.
    void convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1);
    void set_shadow_intensity(float f);

    // Engine scratch surface — RGBA5551, sized src_width × src_height. Held
    // through KSEG1 so CPU writes go straight to RDRAM via the store buffer
    // (no cache-line allocate, no writeback), then DMA-blit'd onto the
    // framebuffer in finalize_frame. hwroad::render_foreground writes here.
    uint16_t* scratch_uc() const { return scratch_uc_ptr; }

    // RGBA5551 engine palette LUT (S16_PALETTE_ENTRIES * 2 entries). Lower
    // half is normal colors, upper half is shadow-darkened. Indexed by engine
    // palette entry; entry 0 is encoded as zero so the alpha-compare composite
    // treats it as transparent.
    const uint16_t* rgb_lut() const { return rgb; }

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

    // Sprite-palette TLUT cache. Sprite renderer reads engine palette as
    // 0x800 + (color<<4) + pix, color in [0..127], pix in [0..15] — so slots
    // are non-overlapping (stride 16, slot size 16) covering engine indices
    // [0x800 .. 0x1800). Entry 0 of each slot is forced to 0 so the RDP
    // alpha-compare composite drops CI4 pixval=0 to transparent (the atlas
    // extractor pads EOR-terminated short rows with 0 as well).
    static constexpr int SPRITE_TLUT_SLOTS = 128;
    static constexpr int SPRITE_TLUT_SLOT_SIZE = 16;
    static constexpr uint32_t SPRITE_PAL_BASE = 0x800;
    alignas(8) uint16_t sprite_tlut[SPRITE_TLUT_SLOTS * SPRITE_TLUT_SLOT_SIZE];

    // Source S16 buffer dimensions (eg. 320x224).
    int src_width, src_height;
    int video_mode;
    int scanlines;
    int scale;

    // Shadow intensity multiplier (0..255).
    int shadow_multi;

    // Scratch RGBA5551 surface populated by hwroad foreground (and possibly
    // future CPU layers) and blit'd onto the framebuffer in finalize_frame.
    // scratch_pixels is the cached allocation; scratch_uc_ptr is its KSEG1
    // alias used by all CPU writers — see scratch_uc() accessor above.
    uint16_t*  scratch_pixels;
    uint16_t*  scratch_uc_ptr;
    surface_t  scratch_surface;
    int        y_offset;          // letterbox top (pixels)

    // RDP-drawn FPS overlay (builtin libdragon debug font).
    rdpq_font_t* fps_font;
    static constexpr uint8_t FPS_FONT_ID = 1;

    bool       initialized;
};
