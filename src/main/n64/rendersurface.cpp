/***************************************************************************
    N64 Render — software palette expand + RDP blit.

    The S16 software rasterizers (hwroad/hwtiles/hwsprites) write palette
    indices into a uint16_t[] buffer. draw_frame() expands those indices into
    a scratch RGBA5551 (FMT_RGBA16) surface via the rgb[] LUT populated by
    RenderBase::convert_palette(). finalize_frame() rdpq_tex_blit's the
    scratch surface into the active display framebuffer using copy mode
    (4x faster than standard, RGBA16-only, no scaling/rotation).
***************************************************************************/

#include "rendersurface.hpp"
#include "platform.hpp"
#include <cstring>
#include <malloc.h>

Render::Render()
    : scratch_pixels(nullptr), scratch_surface{}, y_offset(0), initialized(false)
{
}

Render::~Render()
{
    disable();
}

bool Render::init(int src_w, int src_h,
                  int /*scale_in*/, int video_mode_in, int scanlines_in)
{
    src_width  = src_w;
    src_height = src_h;
    scale      = 1;
    video_mode = video_mode_in;
    scanlines  = scanlines_in;

    if (!initialized)
    {
        display_init(n64::FB_RES, n64::FB_DEPTH, n64::FB_COUNT,
                     GAMMA_NONE, FILTERS_RESAMPLE);
        rdpq_init();
        initialized = true;
    }

    if (scratch_pixels)
        free(scratch_pixels);

    // 8-byte alignment is required for rdpq_tex_blit DMA reads.
    const int bytes = src_width * src_height * (int)sizeof(uint16_t);
    scratch_pixels = (uint16_t*)memalign(8, bytes);
    std::memset(scratch_pixels, 0, bytes);

    scratch_surface = surface_make_linear(scratch_pixels, FMT_RGBA16,
                                          src_width, src_height);

    // Vertical letterbox: engine is 224 tall, framebuffer 240.
    y_offset = (n64::FB_HEIGHT - src_height) / 2;
    if (y_offset < 0) y_offset = 0;

    return true;
}

void Render::disable()
{
    if (scratch_pixels)
    {
        free(scratch_pixels);
        scratch_pixels = nullptr;
    }
    if (initialized)
    {
        rdpq_close();
        display_close();
        initialized = false;
    }
}

bool Render::start_frame()
{
    return true;
}

void Render::draw_frame(uint16_t* pixels)
{
    // pixels[] holds palette indices into rgb[]. Expand to RGBA5551.
    uint16_t*       dst = scratch_pixels;
    const uint16_t* src = pixels;
    const int       n   = src_width * src_height;
    for (int i = 0; i < n; ++i)
        dst[i] = rgb[src[i]];

    // Ensure CPU writes land in RDRAM before RDP DMA reads them.
    data_cache_hit_writeback(scratch_pixels,
                             src_width * src_height * sizeof(uint16_t));
}

bool Render::finalize_frame()
{
    surface_t* disp = display_get();
    rdpq_attach_clear(disp, NULL);
    rdpq_set_mode_copy(false);

    const int x = (disp->width - src_width) / 2;
    rdpq_tex_blit(&scratch_surface, x, y_offset, NULL);

    rdpq_detach_show();
    return true;
}
