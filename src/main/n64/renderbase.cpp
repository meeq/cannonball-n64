/***************************************************************************
    N64 RenderBase implementation.

    convert_palette() packs the S16 5-bit RGB channels into libdragon's
    FMT_RGBA16 framebuffer format: RRRRR GGGGG BBBBB A (5/5/5/1). The alpha
    LSB is set to 1 for opaque pixels — the RDP needs it set to draw the
    palette-expanded scratch surface in COPY mode.
***************************************************************************/

#include "renderbase.hpp"
#include "platform.hpp"
#include <cmath>

RenderBase::RenderBase()
    : rgb{}, src_width(0), src_height(0), video_mode(0),
      scanlines(0), scale(1), shadow_multi(0)
{
}

static inline uint16_t pack_rgba5551(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0x1F) << 11) |
                      ((g & 0x1F) << 6)  |
                      ((b & 0x1F) << 1)  |
                      0x1);
}

void RenderBase::convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1)
{
    adr >>= 1;
    rgb[adr] = pack_rgba5551(r1, g1, b1);

    // Shadow color: scale by shadow_multi/255 (it's a 0..255 multiplier
    // derived from set_shadow_intensity). Stored in the upper half of the
    // table to match the SDL backend's layout.
    uint32_t sr = (r1 * shadow_multi) / 255;
    uint32_t sg = (g1 * shadow_multi) / 255;
    uint32_t sb = (b1 * shadow_multi) / 255;
    rgb[adr + S16_PALETTE_ENTRIES] = pack_rgba5551(sr, sg, sb);
}

void RenderBase::set_shadow_intensity(float f)
{
    shadow_multi = (int)std::round(255.0f * f);
}
