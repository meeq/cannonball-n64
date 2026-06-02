/***************************************************************************
    N64 Render — software palette expand + RDP blit.

    The S16 software rasterizers (hwroad/hwtiles/hwsprites) write palette
    indices into a uint16_t[] buffer. draw_frame() expands those indices into
    a scratch RGBA5551 (FMT_RGBA16) surface via the rgb[] LUT populated by
    convert_palette(). finalize_frame() rdpq_tex_blit's the scratch surface
    into the active display framebuffer using copy mode (4x faster than
    standard, RGBA16-only, no scaling/rotation).
***************************************************************************/

#include "rendersurface.hpp"
#include "platform.hpp"
#include "hwvideo/hwroad.hpp"
#include <cmath>
#include <cstring>
#include <malloc.h>

namespace n64_profile
{
    uint32_t prepare_us = 0;
    uint32_t render_us  = 0;
    uint32_t tick_us    = 0;
    uint32_t palette_us = 0;
    uint32_t wait_us    = 0;
    uint32_t sub_us[SUB_COUNT] = {0};
}

Render::Render()
    : rgb{}, src_width(0), src_height(0), video_mode(0),
      scanlines(0), scale(1), shadow_multi(0),
      scratch_pixels(nullptr), scratch_surface{}, y_offset(0),
      fps_font(nullptr), initialized(false)
{
}

Render::~Render()
{
    disable();
}

// Packs the S16 5-bit RGB channels into libdragon's FMT_RGBA16 framebuffer
// format: RRRRR GGGGG BBBBB A (5/5/5/1). Alpha LSB is 1 for opaque texels —
// finalize_frame composites with alpha compare so the LUT's zero-alpha
// entries (palette index 0, see convert_palette) reveal the road background.
static inline uint16_t pack_rgba5551(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0x1F) << 11) |
                      ((g & 0x1F) << 6)  |
                      ((b & 0x1F) << 1)  |
                      0x1);
}

void Render::convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1)
{
    adr >>= 1;

    // Palette index 0 is the engine's transparency sentinel: tiles, sprites
    // and text skip writing it, and prepare_frame() clears pixels[] to 0
    // each frame. Encode it as zero (alpha LSB = 0) so finalize_frame's
    // alpha-compare composite drops these pixels and the underlying RDP
    // road-background fill shows through.
    if (adr == 0)
    {
        rgb[0] = 0;
        rgb[S16_PALETTE_ENTRIES] = 0;
        return;
    }

    rgb[adr] = pack_rgba5551(r1, g1, b1);

    // Shadow color: scale by shadow_multi/255. Stored in the upper half of
    // the table to match the original SDL backend's layout (osprites looks
    // up shadow indices at +S16_PALETTE_ENTRIES).
    uint32_t sr = (r1 * shadow_multi) / 255;
    uint32_t sg = (g1 * shadow_multi) / 255;
    uint32_t sb = (b1 * shadow_multi) / 255;
    rgb[adr + S16_PALETTE_ENTRIES] = pack_rgba5551(sr, sg, sb);
}

void Render::set_shadow_intensity(float f)
{
    shadow_multi = (int)std::round(255.0f * f);
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
        // FILTERS_DISABLED skips VI's AA + dedither + resample chain. VI
        // shares RDRAM bandwidth with CPU/RDP, and we're CPU-bandwidth bound,
        // so handing that back is a free win — the engine output is already
        // correctly sized for the framebuffer so there's nothing to filter.
        display_init(n64::FB_RES, n64::FB_DEPTH, n64::FB_COUNT,
                     GAMMA_NONE, FILTERS_DISABLED);
        rdpq_init();

        fps_font = rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO);
        rdpq_text_register_font(FPS_FONT_ID, fps_font);

        initialized = true;
    }

    if (scratch_pixels)
        free(scratch_pixels);

    // 8-byte alignment is required for rdpq_tex_blit DMA reads.
    const int bytes = src_width * src_height * (int)sizeof(uint16_t);
    scratch_pixels = (uint16_t*)memalign(8, bytes);
    std::memset(scratch_pixels, 0, bytes);

    // draw_frame writes scratch_pixels through an uncached pointer, so flush
    // and invalidate now — any cached lines left over from the memset above
    // would otherwise win on eviction and overwrite our uncached writes.
    data_cache_hit_writeback_invalidate(scratch_pixels, bytes);

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
        if (fps_font)
        {
            rdpq_text_unregister_font(FPS_FONT_ID);
            rdpq_font_free(fps_font);
            fps_font = nullptr;
        }
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
    uint64_t t0 = get_ticks_us();

    // pixels[] holds palette indices into rgb[]. Expand to RGBA5551.
    //
    // Two micro-opts vs the obvious loop:
    //  * Read/write 32 bits at a time — halves load/store transactions on
    //    the streaming src and dst buffers (each is 143 KB, way bigger than
    //    the 8 KB D-cache).
    //  * Write through an uncached dst pointer. The store buffer coalesces
    //    sequential writes to RDRAM, so we skip the cache-line allocate +
    //    the ~143 KB writeback pass that would otherwise run after the
    //    loop. The cached scratch_pixels is invalidated once at init().
    const uint32_t* src32 = (const uint32_t*)pixels;
    uint32_t*       dst32 = (uint32_t*)UncachedAddr(scratch_pixels);
    const int       n32   = (src_width * src_height) >> 1;

    for (int i = 0; i < n32; ++i)
    {
        uint32_t two = src32[i];
        uint32_t hi  = rgb[(uint16_t)(two >> 16)];
        uint32_t lo  = rgb[(uint16_t)(two & 0xFFFF)];
        dst32[i] = (hi << 16) | lo;
    }

    uint64_t t1 = get_ticks_us();
    n64_profile::palette_us =
        (n64_profile::palette_us * 7 + (uint32_t)(t1 - t0)) >> 3;
}

bool Render::finalize_frame()
{
    // display_get() blocks until a backbuffer frees — i.e. vsync wait. Time
    // it separately so we can tell vsync-bound from CPU-bound.
    uint64_t t0 = get_ticks_us();
    surface_t* disp = display_get();
    uint64_t t1 = get_ticks_us();
    n64_profile::wait_us =
        (n64_profile::wait_us * 7 + (uint32_t)(t1 - t0)) >> 3;

    rdpq_attach_clear(disp, NULL);

    const int x = (disp->width - src_width) / 2;

    // Road background: emit one rdpq_fill_rectangle per same-color band.
    // Configures fill mode internally; safe to call before the composite blit.
    uint64_t rbg_t0 = get_ticks_us();
    hwroad.render_rdp_background(rgb, x, y_offset, src_width);
    uint64_t rbg_t1 = get_ticks_us();
    n64_profile::sub_us[n64_profile::SUB_ROAD_BG] =
        (n64_profile::sub_us[n64_profile::SUB_ROAD_BG] * 7
         + (uint32_t)(rbg_t1 - rbg_t0)) >> 3;

    // Composite the palette-expanded engine surface on top. Standard mode +
    // alpha compare keeps RGBA5551 alpha=0 texels (palette index 0) from
    // overwriting the road background underneath.
    rdpq_set_mode_standard();
    rdpq_mode_alphacompare(1);
    rdpq_tex_blit(&scratch_surface, x, y_offset, NULL);

    // FPS + per-phase profile overlay via RDP. rdpq_text_printf submits its
    // own mode setup, so the preceding copy-mode blit is fine to leave as-is.
    rdpq_text_printf(NULL, FPS_FONT_ID, 4, 12,
                     "FPS %4.1f ras %5lu pal %4lu wait %5lu",
                     display_get_fps(),
                     (unsigned long)n64_profile::prepare_us,
                     (unsigned long)n64_profile::palette_us,
                     (unsigned long)n64_profile::wait_us);
    rdpq_text_printf(NULL, FPS_FONT_ID, 4, 22,
                     "rbg%5lu tbg%5lu tfg%5lu rfg%5lu spr%5lu txt%5lu",
                     (unsigned long)n64_profile::sub_us[n64_profile::SUB_ROAD_BG],
                     (unsigned long)n64_profile::sub_us[n64_profile::SUB_TILE_BG],
                     (unsigned long)n64_profile::sub_us[n64_profile::SUB_TILE_FG],
                     (unsigned long)n64_profile::sub_us[n64_profile::SUB_ROAD_FG],
                     (unsigned long)n64_profile::sub_us[n64_profile::SUB_SPRITE],
                     (unsigned long)n64_profile::sub_us[n64_profile::SUB_TEXT]);

    rdpq_detach_show();
    return true;
}
