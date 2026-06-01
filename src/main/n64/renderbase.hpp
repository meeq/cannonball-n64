/***************************************************************************
    N64 / libdragon abstract RenderBase.

    Mirrors src/main/sdl2/renderbase.hpp's public interface but with no SDL
    types. The protected member layout is N64-specific: the engine pixel
    buffer is RGB565 already, the palette table holds RGB565 colors, and
    the surface is owned by libdragon's display subsystem.
***************************************************************************/

#pragma once

#include "../stdint.hpp"
#include "../globals.hpp"

class RenderBase
{
public:
    RenderBase();
    virtual ~RenderBase() = default;

    virtual bool init(int src_width, int src_height,
                      int scale,
                      int video_mode,
                      int scanlines)          = 0;
    virtual void disable()                    = 0;
    virtual bool start_frame()                = 0;
    virtual bool finalize_frame()             = 0;
    virtual void draw_frame(uint16_t* pixels) = 0;

    void convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1);
    void set_shadow_intensity(float f);

    virtual bool supports_window() { return false; }
    virtual bool supports_vsync()  { return true;  }

protected:
    // Palette Lookup — held as RGB565 (libdragon's DEPTH_16_BPP framebuffer
    // format). Extended slots hold shadow colors at +S16_PALETTE_ENTRIES.
    uint16_t rgb[S16_PALETTE_ENTRIES * 2];

    // Source S16 buffer dimensions (eg. 320x224).
    int src_width, src_height;
    int video_mode;
    int scanlines;
    int scale;

    // Shadow intensity multiplier (0..255).
    int shadow_multi;
};
