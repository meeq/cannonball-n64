#include <cstring> // memcpy
#include <libdragon.h>
#include "hwvideo/hwroad.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"

namespace n64_profile { extern uint32_t prim_count; }

/***************************************************************************
    Video Emulation: OutRun Road Rendering Hardware.
    Based on MAME source code.

    Copyright Aaron Giles.
    All rights reserved.
***************************************************************************/

/*******************************************************************************************
 *
 *  Out Run/X-Board-style road chip
 *
 *  Road control register:
 *      Bits               Usage
 *      -------- -----d--  (X-board only) Direct scanline mode (1) or indirect mode (0)
 *      -------- ------pp  Road enable/priorities:
 *                            0 = road 0 only visible
 *                            1 = both roads visible, road 0 has priority
 *                            2 = both roads visible, road 1 has priority
 *                            3 = road 1 only visible
 *
 *  Road RAM:
 *      Offset   Bits               Usage
 *      000-1FF  ----s--- --------  Road 0: Solid fill (1) or ROM fill
 *               -------- -ccccccc  Road 0: Solid color (if solid fill)
 *               -------i iiiiiiii  Road 0: Index for other tables (if in indirect mode)
 *               -------r rrrrrrr-  Road 0: Road ROM line select
 *      200-3FF  ----s--- --------  Road 1: Solid fill (1) or ROM fill
 *               -------- -ccccccc  Road 1: Solid color (if solid fill)
 *               -------i iiiiiiii  Road 1: Index for other tables (if in indirect mode)
 *               -------r rrrrrrr-  Road 1: Road ROM line select
 *      400-7FF  ----hhhh hhhhhhhh  Road 0: horizontal scroll
 *      800-BFF  ----hhhh hhhhhhhh  Road 1: horizontal scroll
 *      C00-FFF  ----bbbb --------  Background color index
 *               -------- s-------  Road 1: stripe color index
 *               -------- -a------  Road 1: pixel value 2 color index
 *               -------- --b-----  Road 1: pixel value 1 color index
 *               -------- ---c----  Road 1: pixel value 0 color index
 *               -------- ----s---  Road 0: stripe color index
 *               -------- -----a--  Road 0: pixel value 2 color index
 *               -------- ------b-  Road 0: pixel value 1 color index
 *               -------- -------c  Road 0: pixel value 0 color index
 *
 *  Logic:
 *      First, the scanline is used to index into the tables at 000-1FF/200-3FF
 *          - if solid fill, the background is filled with the specified color index
 *          - otherwise, the remaining tables are used
 *
 *      If indirect mode is selected, the index is taken from the low 9 bits of the
 *          table value from 000-1FF/200-3FF
 *      If direct scanline mode is selected, the index is set equal to the scanline
 *          for road 0, or the scanline + 256 for road 1
 *
 *      The horizontal scroll value is looked up using the index in the tables at
 *          400-7FF/800-BFF
 *
 *      The color information is looked up using the index in the table at C00-FFF. Note
 *          that the same table is used for both roads.
 *
 *
 *  Out Run road priorities are controlled by a PAL that maps as indicated below.
 *  This was used to generate the priority_map. It is assumed that X-board is the
 *  same, though this logic is locked inside a Sega custom.
 *
 *  RRC0 =  CENTA & (RDA == 3) & !RRC2
 *      | CENTB & (RDB == 3) & RRC2
 *      | (RDA == 1) & !RRC2
 *      | (RDB == 1) & RRC2
 *
 *  RRC1 =  CENTA & (RDA == 3) & !RRC2
 *      | CENTB & (RDB == 3) & RRC2
 *      | (RDA == 2) & !RRC2
 *      | (RDB == 2) & RRC2
 *
 *  RRC2 = !/HSYNC & IIQ
 *      | (CTRL == 3)
 *      | !CENTA & (RDA == 3) & !CENTB & (RDB == 3) & (CTRL == 2)
 *      | CENTB & (RDB == 3) & (CTRL == 2)
 *      | !CENTA & (RDA == 3) & !M2 & (CTRL == 2)
 *      | !CENTA & (RDA == 3) & !M3 & (CTRL == 2)
 *      | !M0 & (RDB == 0) & (CTRL == 2)
 *      | !M1 & (RDB == 0) & (CTRL == 2)
 *      | !CENTA & (RDA == 3) & CENTB & (RDB == 3) & (CTRL == 1)
 *      | !M0 & CENTB & (RDB == 3) & (CTRL == 1)
 *      | !M1 & CENTB & (RDB == 3) & (CTRL == 1)
 *      | !CENTA & M0 & (RDB == 0) & (CTRL == 1)
 *      | !CENTA & M1 & (RDB == 0) & (CTRL == 1)
 *      | !CENTA & (RDA == 3) & (RDB == 1) & (CTRL == 1)
 *      | !CENTA & (RDA == 3) & (RDB == 2) & (CTRL == 1)
 *
 *  RRC3 =  VA11 & VB11
 *      | VA11 & (CTRL == 0)
 *      | (CTRL == 3) & VB11
 *
 *  RRC4 =  !CENTA & (RDA == 3) & !CENTB & (RDB == 3)
 *      | VA11 & VB11
 *      | VA11 & (CTRL == 0)
 *      | (CTRL == 3) & VB11
 *      | !CENTB & (RDB == 3) & (CTRL == 3)
 *      | !CENTA & (RDA == 3) & (CTRL == 0)
 *
 *******************************************************************************************/

HWRoad hwroad;

HWRoad::HWRoad()
{
}

HWRoad::~HWRoad()
{
}

// Convert road to a more useable format
void HWRoad::init(const uint8_t* src_road, const bool hires)
{
    road_control = 0;
    color_offset1 = 0x400;
    color_offset2 = 0x420;
    color_offset3 = 0x780;
    x_offset = 0;

    if (src_road)
        decode_road(src_road);

    // Road background is drawn via RDP fill rects (see render_rdp_background);
    // only the per-pixel road foreground texture path stays on the CPU.
    render_foreground = hires ? &HWRoad::render_foreground_hires
                              : &HWRoad::render_foreground_lores;
}

/*
    There are TWO (identical) roads we need to decode.
    Each of these roads is represented using a 512x256 map.
    See: http://www.extentofthejam.com/pseudo/
      
    512 x 256 x 2bpp map 
    0x8000 bytes of data. 
    2 Bits Per Pixel.
       
    Per Road:
    Bit 0 of each pixel is stored at offset 0x0000 - 0x3FFF
    Bit 1 of each pixel is stored at offset 0x4000 - 0x7FFF

    This means: 80 bytes per X Row [2 x 0x40 Bytes from the two separate locations]

    Decoded Format:  
    0 = Road Colour
    1 = Road Inner Stripe
    2 = Road Outer Stripe
    3 = Road Exterior
    7 = Central Stripe
*/

void HWRoad::decode_road(const uint8_t* src_road)
{
    for (int y = 0; y < 256 * 2; y++) 
    {
        const int src = ((y & 0xff) * 0x40 + (y >> 8) * 0x8000) % rom_size; // tempGfx
        const int dst = y * 512; // System16Roads

        // loop over columns
        for (int x = 0; x < 512; x++) 
        {
            roads[dst + x] = (((src_road[src + (x / 8)] >> (~x & 7)) & 1) << 0) | (((src_road[src + (x / 8 + 0x4000)] >> (~x & 7)) & 1) << 1);

            // pre-mark road data in the "stripe" area with a high bit
            if (x >= 256 - 8 && x < 256 && roads[dst + x] == 3)
                roads[dst + x] |= 4;
        }
    }

    // set up a dummy road in the last entry
    for (int i = 0; i < 512; i++) 
    {
        roads[256 * 2 * 512 + i] = 3;
    }
}

// Writes go to RAM, but we read from the RAM Buffer.
void HWRoad::write16(uint32_t adr, const uint16_t data)
{
    ram[(adr >> 1) & 0x7FF] = data;
}

void HWRoad::write16(uint32_t* adr, const uint16_t data)
{
    uint32_t a = *adr;
    ram[(a >> 1) & 0x7FF] = data;
    *adr += 2;
}

void HWRoad::write32(uint32_t* adr, const uint32_t data)
{
    uint32_t a = *adr;
    ram[(a >> 1) & 0x7FF] = data >> 16;
    ram[((a >> 1) + 1) & 0x7FF] = data & 0xFFFF;
    *adr += 4;
}

uint16_t HWRoad::read_road_control()
{
    uint32_t *src = (uint32_t *)ram;
    uint32_t *dst = (uint32_t *)ramBuff;

    // swap the halves of the road RAM
    for (uint16_t i = 0; i < ROAD_RAM_SIZE/4; i++)
    {
        uint32_t temp = *src;
        *src++ = *dst;
        *dst++ = temp;
    }

    return 0xffff;
}

void HWRoad::write_road_control(const uint8_t road_control)
{
    this->road_control = road_control;
}

// ------------------------------------------------------------------------------------------------
// Road Rendering: Lores Version
// ------------------------------------------------------------------------------------------------

// Background: emit RDP fill rectangles for solid-fill scanline bands.
//
// The original CPU path filled pixels[y * s16_width + x] for each solid-fill
// scanline. Here we resolve the per-scanline palette index using the same
// road_control/road RAM rules, then batch consecutive same-color scanlines
// into a single rdpq_fill_rectangle. Caller must have attached the display
// and *not* set a render mode yet — we configure fill mode here.
void HWRoad::render_rdp_background(const uint16_t* rgb_lut, int x_offset, int y_offset, int s16_width)
{
    uint16_t* roadram = ramBuff;

    int prev_color = -1;
    int band_start = 0;

    rdpq_set_mode_fill(RGBA32(0, 0, 0, 0));

    for (int y = 0; y <= S16_HEIGHT; y++)
    {
        int color = -1;

        if (y < S16_HEIGHT)
        {
            int data0 = roadram[0x000 + y];
            int data1 = roadram[0x100 + y];

            switch (road_control & 3)
            {
                case 0:
                    if (data0 & 0x800)
                        color = data0 & 0x7f;
                    break;
                case 1:
                    if (data0 & 0x800)
                        color = data0 & 0x7f;
                    else if (data1 & 0x800)
                        color = data1 & 0x7f;
                    break;
                case 2:
                    if (data1 & 0x800)
                        color = data1 & 0x7f;
                    else if (data0 & 0x800)
                        color = data0 & 0x7f;
                    break;
                case 3:
                    if (data1 & 0x800)
                        color = data1 & 0x7f;
                    break;
            }
        }

        if (color != prev_color)
        {
            if (prev_color != -1)
            {
                uint16_t pixel = rgb_lut[prev_color | color_offset3];
                uint8_t r = ((pixel >> 11) & 0x1F) << 3;
                uint8_t g = ((pixel >>  6) & 0x1F) << 3;
                uint8_t b = ((pixel >>  1) & 0x1F) << 3;
                rdpq_set_fill_color(RGBA32(r, g, b, 0xFF));
                rdpq_fill_rectangle(x_offset,
                                    y_offset + band_start,
                                    x_offset + s16_width,
                                    y_offset + y);
                n64_profile::prim_count++;
            }
            band_start = y;
            prev_color = color;
        }
    }
}

namespace
{
    // Bounds-elimination helpers for render_foreground_lores. hpos increments
    // by 1 per pixel, wrapping mod 0x1000. "In-bounds" iff hpos < 0x200; over
    // a 320-pixel scanline the trajectory crosses at most one wrap, so each
    // road has at most one [s_start, s_end) in-bounds span and a constant
    // texture base offset at that span's start. Outside the span the texel is
    // sentinel 3.
    struct LoresSpan { int s_start, s_end, t_base; };

    inline LoresSpan compute_road_span(int32_t H, int W)
    {
        LoresSpan s;
        if (H < 0x200) {
            s.s_start = 0;
            s.s_end   = (0x200 - H < W) ? (0x200 - H) : W;
            s.t_base  = H;
        } else {
            const int wrap = 0x1000 - H;
            if (wrap >= W) { s.s_start = W; s.s_end = W; }
            else           { s.s_start = wrap;
                             s.s_end   = (wrap + 0x200 < W) ? (wrap + 0x200) : W; }
            s.t_base = 0;
        }
        return s;
    }

    // Segment painters. p is 16-byte aligned (engine scratch surface), so
    // p+a is 32-bit aligned iff a is even — odd-a starts with a single-
    // pixel lead store before u32 packed pairs take over. Big-endian:
    // (ca<<16)|cb stores ca at the lower address.

    inline void paint_oob_span(uint16_t* p, int a, int n,
                               uint16_t c, uint32_t c32)
    {
        if (n <= 0) return;
        int i = 0;
        if ((a & 1) != 0) { p[a] = c; i = 1; }
        uint32_t* p32 = (uint32_t*)(p + a + i);
        const int pairs = (n - i) >> 1;
        for (int k = 0; k < pairs; k++) p32[k] = c32;
        i += pairs * 2;
        if (i < n) p[a + i] = c;
    }

    inline void paint_single_in_span(uint16_t* p, int a, int n,
                                     const uint8_t* src, const uint16_t* clut)
    {
        if (n <= 0) return;
        int i = 0;
        if ((a & 1) != 0) { p[a] = clut[src[0]]; i = 1; }
        uint32_t* p32 = (uint32_t*)(p + a + i);
        const int pairs = (n - i) >> 1;
        for (int k = 0; k < pairs; k++) {
            uint32_t ca = clut[src[i  ]];
            uint32_t cb = clut[src[i+1]];
            p32[k] = (ca << 16) | cb;
            i += 2;
        }
        if (i < n) p[a + i] = clut[src[i]];
    }

    inline void paint_two_in_span(uint16_t* p, int a, int n,
                                  const uint8_t* s0, const uint8_t* s1,
                                  const uint16_t* merged)
    {
        if (n <= 0) return;
        int i = 0;
        if ((a & 1) != 0) {
            p[a] = merged[(s0[0] << 3) | s1[0]];
            i = 1;
        }
        uint32_t* p32 = (uint32_t*)(p + a + i);
        const int pairs = (n - i) >> 1;
        for (int k = 0; k < pairs; k++) {
            uint32_t ca = merged[(s0[i  ] << 3) | s1[i  ]];
            uint32_t cb = merged[(s0[i+1] << 3) | s1[i+1]];
            p32[k] = (ca << 16) | cb;
            i += 2;
        }
        if (i < n) p[a + i] = merged[(s0[i] << 3) | s1[i]];
    }
} // anonymous namespace

// Foreground: Render From ROM
//
// Writes RGBA5551 straight into the engine scratch surface. The original
// path emitted palette indices into a uint16_t[] buffer that a separate pass
// then expanded via the palette LUT — that expand pass cost ~7 ms/frame on
// R4300 and the streaming src→dst writes thrashed the 8 KB D-cache. Resolving
// the 12 used color_table slots into RGBA5551 once per scanline lets the
// inner loop do a single uncached store per pixel with no follow-up pass.
//
// Bounds elimination: hpos at pixel x is monotonic (H+x mod 0x1000) so each
// road's in-bounds span is one contiguous range. compute_road_span resolves
// it once, then the scanline is split into segments at the union of road
// boundaries (≤5 segments) and each segment dispatches to a straight-line
// painter — no per-pixel `hpos < 0x200` branch and no `(hpos+1) & 0xfff`.
void HWRoad::render_foreground_lores(uint16_t* dst_rgba, const uint16_t* rgb_lut)
{
    int x, y;
    uint16_t* roadram = ramBuff;

#define HWROAD_PROFILE_LORES 0
#if HWROAD_PROFILE_LORES
    uint64_t prof_t0 = get_ticks_us();
    uint32_t prof_scanlines = 0;
    uint32_t prof_skipped   = 0;
    uint32_t prof_pairskip  = 0;  // both-roads-low-priority early continue
#endif

    for (y = 0; y < S16_HEIGHT; y++)
    {
        uint16_t color_table[32];

        static const uint8_t priority_map[2][8] =
        {
            { 0x80,0x81,0x81,0x87,0,0,0,0x00 },
            { 0x81,0x81,0x81,0x8f,0,0,0,0x80 }
        };

        const uint32_t data0 = roadram[0x000 + y];
        const uint32_t data1 = roadram[0x100 + y];

        // if both roads are low priority, skip
        if (((data0 & 0x800) != 0) && ((data1 & 0x800) != 0))
        {
#if HWROAD_PROFILE_LORES
            prof_pairskip++;
#endif
            continue;
        }

        uint16_t* pPixel = dst_rgba + (y * config.s16_width);
        int32_t hpos0, hpos1, color0, color1;
        int32_t control = road_control & 3;

        uint8_t *src0, *src1;
        int32_t bgcolor; // 8 bits

        // get road 0 data
        src0   = ((data0 & 0x800) != 0) ? roads + 256 * 2 * 512 : (roads + (0x000 + ((data0 >> 1) & 0xff)) * 512);
        hpos0  = roadram[0x200 + (((road_control & 4) != 0) ? y : (data0 & 0x1ff))] & 0xfff;
        color0 = roadram[0x600 + (((road_control & 4) != 0) ? y : (data0 & 0x1ff))];

        // get road 1 data
        src1   = ((data1 & 0x800) != 0) ? roads + 256 * 2 * 512 : (roads + (0x100 + ((data1 >> 1) & 0xff)) * 512);
        hpos1  = roadram[0x400 + (((road_control & 4) != 0) ? (0x100 + y) : (data1 & 0x1ff))] & 0xfff;
        color1 = roadram[0x600 + (((road_control & 4) != 0) ? (0x100 + y) : (data1 & 0x1ff))];

        // determine the 5 colors for road 0 — resolve straight to RGBA5551
        color_table[0x00] = rgb_lut[color_offset1 ^ 0x00 ^ ((color0 >> 0) & 1)];
        color_table[0x01] = rgb_lut[color_offset1 ^ 0x02 ^ ((color0 >> 1) & 1)];
        color_table[0x02] = rgb_lut[color_offset1 ^ 0x04 ^ ((color0 >> 2) & 1)];
        bgcolor = (color0 >> 8) & 0xf;
        color_table[0x03] = ((data0 & 0x200) != 0) ? color_table[0x00] : rgb_lut[color_offset2 ^ 0x00 ^ bgcolor];
        color_table[0x07] = rgb_lut[color_offset1 ^ 0x06 ^ ((color0 >> 3) & 1)];

        // determine the 5 colors for road 1 — resolve straight to RGBA5551
        color_table[0x10] = rgb_lut[color_offset1 ^ 0x08 ^ ((color1 >> 4) & 1)];
        color_table[0x11] = rgb_lut[color_offset1 ^ 0x0a ^ ((color1 >> 5) & 1)];
        color_table[0x12] = rgb_lut[color_offset1 ^ 0x0c ^ ((color1 >> 6) & 1)];
        bgcolor = (color1 >> 8) & 0xf;
        color_table[0x13] = ((data1 & 0x200) != 0) ? color_table[0x10] : rgb_lut[color_offset2 ^ 0x10 ^ bgcolor];
        color_table[0x17] = rgb_lut[color_offset1 ^ 0x0e ^ ((color1 >> 7) & 1)];

        // Shift road dependent on whether we are in widescreen mode or not
        uint16_t s16_x = 0x5f8 + config.s16_x_off;

        // draw the road
        const int W = config.s16_width;
        switch (control)
        {
            case 0:
            {
                if (data0 & 0x800)
                {
#if HWROAD_PROFILE_LORES
                    prof_skipped++;
#endif
                    continue;
                }
                hpos0 = (hpos0 - (s16_x + x_offset)) & 0xfff;
                const LoresSpan s = compute_road_span(hpos0, W);
                const uint16_t* clut    = &color_table[0x00];
                const uint16_t  c_oob   = clut[3];
                const uint32_t  c_oob32 = ((uint32_t)c_oob << 16) | c_oob;
                paint_oob_span(pPixel, 0, s.s_start, c_oob, c_oob32);
                paint_single_in_span(pPixel, s.s_start, s.s_end - s.s_start,
                                     src0 + s.t_base, clut);
                paint_oob_span(pPixel, s.s_end, W - s.s_end, c_oob, c_oob32);
#if HWROAD_PROFILE_LORES
                prof_scanlines++;
#endif
                break;
            }

            case 1:
            case 2:
            {
                hpos0 = (hpos0 - (s16_x + x_offset)) & 0xfff;
                hpos1 = (hpos1 - (s16_x + x_offset)) & 0xfff;

                // Per-scanline merged color LUT. Folds priority_map[ctrl-1] +
                // the road0/road1 ternary into a single 16-bit indexed
                // lookup keyed by (pix0*8 + pix1). Slots for pix in {4,5,6}
                // hold whatever color_table did — the road data only ever
                // produces {0,1,2,3,7}, matching the SDL build.
                const uint8_t* pmap_row = priority_map[control - 1];
                uint16_t merged[64];
                for (int p0 = 0; p0 < 8; p0++)
                {
                    const uint8_t  mask = pmap_row[p0];
                    const uint16_t c0   = color_table[0x00 + p0];
                    uint16_t* row = &merged[p0 << 3];
                    for (int p1 = 0; p1 < 8; p1++)
                        row[p1] = ((mask >> p1) & 1)
                                  ? color_table[0x10 + p1]
                                  : c0;
                }

                const LoresSpan s0 = compute_road_span(hpos0, W);
                const LoresSpan s1 = compute_road_span(hpos1, W);

                const uint16_t c_oob   = merged[(3 << 3) | 3];
                const uint32_t c_oob32 = ((uint32_t)c_oob << 16) | c_oob;

                // Sub-LUTs for one-road-OOB segments.
                uint16_t sub0[8], sub1[8];
                for (int k = 0; k < 8; k++) {
                    sub0[k] = merged[(k << 3) | 3];   // road1 OOB
                    sub1[k] = merged[(3 << 3) | k];   // road0 OOB
                }

                // Cut points → sort → walk adjacent pairs as
                // constant-(in0,in1) segments. ≤5 segments per scanline.
                int cuts[6] = { 0, s0.s_start, s0.s_end, s1.s_start, s1.s_end, W };
                for (int i = 1; i < 6; i++) {
                    const int v = cuts[i]; int j = i;
                    while (j > 0 && cuts[j-1] > v) { cuts[j] = cuts[j-1]; j--; }
                    cuts[j] = v;
                }

                for (int seg = 0; seg < 5; seg++) {
                    const int a = cuts[seg];
                    const int b = cuts[seg+1];
                    if (a >= b) continue;
                    const bool in0 = (a >= s0.s_start && a < s0.s_end);
                    const bool in1 = (a >= s1.s_start && a < s1.s_end);
                    const int  n   = b - a;

                    if (in0 && in1) {
                        paint_two_in_span(pPixel, a, n,
                                          src0 + s0.t_base + (a - s0.s_start),
                                          src1 + s1.t_base + (a - s1.s_start),
                                          merged);
                    } else if (in0) {
                        paint_single_in_span(pPixel, a, n,
                                             src0 + s0.t_base + (a - s0.s_start),
                                             sub0);
                    } else if (in1) {
                        paint_single_in_span(pPixel, a, n,
                                             src1 + s1.t_base + (a - s1.s_start),
                                             sub1);
                    } else {
                        paint_oob_span(pPixel, a, n, c_oob, c_oob32);
                    }
                }
#if HWROAD_PROFILE_LORES
                prof_scanlines++;
#endif
                break;
            }

            case 3:
            {
                if (data1 & 0x800)
                {
#if HWROAD_PROFILE_LORES
                    prof_skipped++;
#endif
                    continue;
                }
                hpos1 = (hpos1 - (s16_x + x_offset)) & 0xfff;
                const LoresSpan s = compute_road_span(hpos1, W);
                const uint16_t* clut    = &color_table[0x10];
                const uint16_t  c_oob   = clut[3];
                const uint32_t  c_oob32 = ((uint32_t)c_oob << 16) | c_oob;
                paint_oob_span(pPixel, 0, s.s_start, c_oob, c_oob32);
                paint_single_in_span(pPixel, s.s_start, s.s_end - s.s_start,
                                     src1 + s.t_base, clut);
                paint_oob_span(pPixel, s.s_end, W - s.s_end, c_oob, c_oob32);
#if HWROAD_PROFILE_LORES
                prof_scanlines++;
#endif
                break;
            }
        } // end switch
    } // end for

#if HWROAD_PROFILE_LORES
    static uint32_t prof_frame = 0;
    uint64_t prof_t1 = get_ticks_us();
    if ((prof_frame++ % 60) == 0)
    {
        debugf("rfg[%5lu] ctrl=%ld total=%5lu lines=%3lu pairskip=%3lu skip=%3lu\n",
               (unsigned long)prof_frame,
               (long)(road_control & 3),
               (unsigned long)(prof_t1 - prof_t0),
               (unsigned long)prof_scanlines,
               (unsigned long)prof_pairskip,
               (unsigned long)prof_skipped);
    }
#endif
}

// ------------------------------------------------------------------------------------------------
// Render Road Foreground - High Resolution Version
// Interpolates previous scanline with next.
// ------------------------------------------------------------------------------------------------
void HWRoad::render_foreground_hires(uint16_t* dst_rgba, const uint16_t* rgb_lut)
{
    int x, y, yy;
    uint16_t* roadram = ramBuff;
    
    uint16_t color_table[32];
    int32_t color0, color1;
    int32_t bgcolor; // 8 bits

    for (y = 0; y < config.s16_height; y++) 
    {
        yy = y >> 1;
       
        static const uint8_t priority_map[2][8] =
        {
            { 0x80,0x81,0x81,0x87,0,0,0,0x00 },
            { 0x81,0x81,0x81,0x8f,0,0,0,0x80 }
        };

        uint32_t data0 = roadram[0x000 + yy];
        uint32_t data1 = roadram[0x100 + yy];

        // if both roads are low priority, skip
        if (((data0 & 0x800) != 0) && ((data1 & 0x800) != 0))
        {
            y++; 
            continue;
        }

        uint8_t *src0 = NULL, *src1 = NULL;

        // get road 0 data
        int32_t hpos0  = roadram[0x200 + (((road_control & 4) != 0) ? yy : (data0 & 0x1ff))] & 0xfff;

        // get road 1 data       
        int32_t hpos1  = roadram[0x400 + (((road_control & 4) != 0) ? (0x100 + yy) : (data1 & 0x1ff))] & 0xfff;
        
        // ----------------------------------------------------------------------------------------
        // Interpolate Scanlines when in hi-resolution mode.
        // ----------------------------------------------------------------------------------------
        if (y & 1 && yy < S16_HEIGHT - 1)
        {
            uint32_t data0_next = roadram[0x000 + yy + 1];
            uint32_t data1_next = roadram[0x100 + yy + 1];

            int32_t  hpos0_next = roadram[0x200 + (((road_control & 4) != 0) ? yy + 1 : (data0_next & 0x1ff))] & 0xfff;
            int32_t  hpos1_next = roadram[0x400 + (((road_control & 4) != 0) ? yy + 1 : (data1_next & 0x1ff))] & 0xfff;

            // Interpolate road 1 position
            if (((data0 & 0x800) == 0) && (data0_next & 0x800) == 0)
            {
                data0      = (data0      >> 1) & 0xFF;
                data0_next = (data0_next >> 1) & 0xFF;
                int32_t diff = (data0 + ((data0_next - data0) >> 1)) & 0xFF;
                src0 = (roads + (0x000 + diff) * 512);
                hpos0 = (hpos0 + ((hpos0_next - hpos0) >> 1)) & 0xFFF;
            }
            // Interpolate road 2 source position
            if (((data1 & 0x800) == 0) && (data1_next & 0x800) == 0)
            {
                data1      = (data1      >> 1) & 0xFF;
                data1_next = (data1_next >> 1) & 0xFF;
                int32_t diff = (data1 + ((data1_next - data1) >> 1)) & 0xFF;
                src1 = (roads + (0x100 + diff) * 512);
                hpos1 = (hpos1 + ((hpos1_next - hpos1) >> 1)) & 0xFFF;
            }     
        }
        // ----------------------------------------------------------------------------------------
        // Recalculate for non-interpolated scanlines
        // ----------------------------------------------------------------------------------------
        else
        {            
            color0 = roadram[0x600 + (((road_control & 4) != 0) ? yy :           (data0 & 0x1ff))];
            color1 = roadram[0x600 + (((road_control & 4) != 0) ? (0x100 + yy) : (data1 & 0x1ff))];

            // determine the 5 colors for road 0 — resolve straight to RGBA5551
            color_table[0x00] = rgb_lut[color_offset1 ^ 0x00 ^ ((color0 >> 0) & 1)];
            color_table[0x01] = rgb_lut[color_offset1 ^ 0x02 ^ ((color0 >> 1) & 1)];
            color_table[0x02] = rgb_lut[color_offset1 ^ 0x04 ^ ((color0 >> 2) & 1)];
            bgcolor = (color0 >> 8) & 0xf;
            color_table[0x03] = ((data0 & 0x200) != 0) ? color_table[0x00] : rgb_lut[color_offset2 ^ 0x00 ^ bgcolor];
            color_table[0x07] = rgb_lut[color_offset1 ^ 0x06 ^ ((color0 >> 3) & 1)];

            // determine the 5 colors for road 1 — resolve straight to RGBA5551
            color_table[0x10] = rgb_lut[color_offset1 ^ 0x08 ^ ((color1 >> 4) & 1)];
            color_table[0x11] = rgb_lut[color_offset1 ^ 0x0a ^ ((color1 >> 5) & 1)];
            color_table[0x12] = rgb_lut[color_offset1 ^ 0x0c ^ ((color1 >> 6) & 1)];
            bgcolor = (color1 >> 8) & 0xf;
            color_table[0x13] = ((data1 & 0x200) != 0) ? color_table[0x10] : rgb_lut[color_offset2 ^ 0x10 ^ bgcolor];
            color_table[0x17] = rgb_lut[color_offset1 ^ 0x0e ^ ((color1 >> 7) & 1)];
        }
        
        if (src0 == NULL)
            src0 = ((data0 & 0x800) != 0) ? roads + 256 * 2 * 512 : (roads + (0x000 + ((data0 >> 1) & 0xff)) * 512);
        if (src1 == NULL)
            src1 = ((data1 & 0x800) != 0) ? roads + 256 * 2 * 512 : (roads + (0x100 + ((data1 >> 1) & 0xff)) * 512);

        // Shift road dependent on whether we are in widescreen mode or not
        uint16_t s16_x = 0x5f8 + config.s16_x_off;
        uint16_t* const pPixel = dst_rgba + (y * config.s16_width);

        // draw the road
        switch (road_control & 3)
        {
            case 0:
                if (data0 & 0x800)
                    continue;
                hpos0 = (hpos0 - (s16_x + x_offset)) & 0xfff;
                for (x = 0; x < config.s16_width; x++) 
                {
                    int pix0 = (hpos0 < 0x200) ? src0[hpos0] : 3;
                    pPixel[x] = color_table[0x00 + pix0];
                    if (x & 1)
                        hpos0 = (hpos0 + 1) & 0xfff;
                }
                break;

            case 1:
                hpos0 = (hpos0 - (s16_x + x_offset)) & 0xfff;
                hpos1 = (hpos1 - (s16_x + x_offset)) & 0xfff;
                for (x = 0; x < config.s16_width; x++) 
                {
                    int pix0 = (hpos0 < 0x200) ? src0[hpos0] : 3;
                    int pix1 = (hpos1 < 0x200) ? src1[hpos1] : 3;
                    if (((priority_map[0][pix0] >> pix1) & 1) != 0)
                        pPixel[x] = color_table[0x10 + pix1];
                    else
                        pPixel[x] = color_table[0x00 + pix0];

                    if (x & 1)
                    {
                        hpos0 = (hpos0 + 1) & 0xfff;
                        hpos1 = (hpos1 + 1) & 0xfff;
                    }
                }
                break;

            case 2:
                hpos0 = (hpos0 - (s16_x + x_offset)) & 0xfff;
                hpos1 = (hpos1 - (s16_x + x_offset)) & 0xfff;
                for (x = 0; x < config.s16_width; x++) 
                {
                    int pix0 = (hpos0 < 0x200) ? src0[hpos0] : 3;
                    int pix1 = (hpos1 < 0x200) ? src1[hpos1] : 3;
                    if (((priority_map[1][pix0] >> pix1) & 1) != 0)
                        pPixel[x] = color_table[0x10 + pix1];
                    else
                        pPixel[x] = color_table[0x00 + pix0];
                      
                    if (x & 1)
                    {
                        hpos0 = (hpos0 + 1) & 0xfff;
                        hpos1 = (hpos1 + 1) & 0xfff;
                    }
                }
                break;

            case 3:
                if (data1 & 0x800)
                    continue;
                hpos1 = (hpos1 - (s16_x + x_offset)) & 0xfff;
                for (x = 0; x < config.s16_width; x++) 
                {
                    int pix1 = (hpos1 < 0x200) ? src1[hpos1] : 3;
                    pPixel[x] = color_table[0x10 + pix1];                   
                    if (x & 1)
                        hpos1 = (hpos1 + 1) & 0xfff;
                }
                break;
            } // end switch
    } // end for
}

