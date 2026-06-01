/***************************************************************************
    N64 / libdragon Render.

    Concrete RenderBase subclass. Produces a scratch RGB565 surface from the
    engine's palette-indexed pixel buffer, then rdpq_tex_blit's it into the
    display framebuffer with a vertical letterbox.
***************************************************************************/

#pragma once

#include "renderbase.hpp"
#include <libdragon.h>

class Render : public RenderBase
{
public:
    Render();
    ~Render();

    bool init(int src_width, int src_height,
              int scale, int video_mode, int scanlines) override;
    void disable() override;
    bool start_frame() override;
    bool finalize_frame() override;
    void draw_frame(uint16_t* pixels) override;

private:
    // Scratch RGB565 surface that draw_frame() populates and finalize_frame()
    // blits. Held as a flat buffer with a surface_t wrapper.
    uint16_t*  scratch_pixels;
    surface_t  scratch_surface;
    int        y_offset;          // letterbox top (pixels)

    bool       initialized;
};
