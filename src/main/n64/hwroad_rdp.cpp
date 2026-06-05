/***************************************************************************
    RDP path for HWRoad foreground rasteriser — all 4 road_control cases.

    Two phases per frame:

      1. CPU pass (called from rendersurface::finalize_frame, owns rspq).
         For each scanline:
           - Replay the same color_offset / data0 / hpos / color_table
             math as the CPU render_foreground_lores.
           - Compute the in-bounds spans s0 / s1.
           - Build one CI4 mask for the active span — 1 byte per 2 pixels,
             high nibble first. Each nibble is a 4-bit TLUT index encoding
             which colour the merged road priority decision picked.
           - Build one 16-entry RGBA16 TLUT (slots 0..7 = road0 c[0..7];
             slots 8..15 = road1 c[0..7]).
         Both arrays live in *cached* RAM. The merged-index decision uses
         a per-frame 64-entry LUT keyed by (pix0, pix1) — replaces the
         priority_map walk that the CPU rasteriser does per pixel.

      2. RDP emit (still under the same finalize_frame call).
         a) Fill mode: one rdpq_fill_rectangle per active line covering
            the whole row with the merged "(pix0=3, pix1=3)" colour:
              control 0:    c0[3]   (road0 bg)
              control 1:    c0[3]   (priority_map[0][3] bit3 = 0)
              control 2:    c1[3]   (priority_map[1][3] bit3 = 1)
              control 3:    c1[3]   (road1 bg)
            That sweeps both OOB regions (outside the active span) and
            doubles as a backstop wherever the CI4 rect would resample
            the same colour.
         b) Standard mode + CI4 + TLUT_RGBA16 + alphacompare(1).
            One textured rect per active line over [s_start, s_end). The
            RDP samples the CI4 nibble → indexes the per-line TLUT →
            writes the road colour into the framebuffer.

    The CI4 4-bit-per-pixel load goes through an I8 view of the same TMEM
    (same trick libdragon's hwtiles uses) — see TILE0 vs TILE1 below.
***************************************************************************/

#include "n64/hwroad_rdp.hpp"
#include "n64/hwroad_rdp_internal.hpp"
#include "n64/platform.hpp"
#include "hwvideo/hwroad.hpp"
#include "frontend/config.hpp"

#include <libdragon.h>
#include <malloc.h>
#include <cstddef>
#include <cstring>

namespace n64
{
namespace hwroad_rdp
{

// All-cases RDP overlay. Toggle to A/B against the CPU rasterizer; runtime
// gate goes through should_skip_cpu().
bool     enabled = true;
uint32_t last_us = 0;
static uint32_t s_frame = 0;

// Shared with hwroad_rdp_rsp.cpp via the internal header.
namespace detail
{
    uint8_t*  mask_buf = nullptr;
    uint16_t* tlut_buf = nullptr;
    LineState line[MAX_LINES];
}

using detail::MAX_LINES;
using detail::MAX_SPAN_PX;
using detail::MASK_BYTES;
using detail::TLUT_ENTRIES;
using detail::LineState;
using detail::SKIP;
using detail::OOB_ONLY;
using detail::DRAW;
using detail::mask_buf;
using detail::tlut_buf;
using detail::line;
using detail::mask_ptr;
using detail::tlut_ptr;

namespace
{
    // Per-line CI4 masks. 224 * 160 = ~35 KB.
    constexpr size_t MASK_BUF_BYTES = (size_t)MAX_LINES * MASK_BYTES;
    // Per-line RGBA16 TLUT. 224 * 16 * 2 = ~7 KB.
    constexpr size_t TLUT_BUF_BYTES = (size_t)MAX_LINES * TLUT_ENTRIES * 2;

    // Cached owner pointers (for free()). Hot path writes only via the
    // uncached aliases; sequential byte/word stores go through the R4300
    // store buffer and coalesce into 32-byte RDRAM bursts — same trick
    // the CPU rasteriser uses for its KSEG1 scratch surface. Avoids the
    // ~140 KB/frame of cache pollution the cached-write + writeback path
    // was paying for.
    void*      mask_buf_cached = nullptr;
    void*      tlut_buf_cached = nullptr;

    inline color_t rgba32_from_5551(uint16_t p)
    {
        uint8_t r = ((p >> 11) & 0x1F) << 3;
        uint8_t g = ((p >>  6) & 0x1F) << 3;
        uint8_t b = ((p >>  1) & 0x1F) << 3;
        return RGBA32(r, g, b, 0xFF);
    }

    // Inline copy of HWRoad::compute_road_span — kept private to hwroad.cpp.
    inline void compute_span(int32_t hpos, int W,
                             int& s_start, int& s_end, int& t_base)
    {
        if (hpos < 0x200) {
            s_start = 0;
            s_end   = (0x200 - hpos < W) ? (0x200 - hpos) : W;
            t_base  = hpos;
        } else {
            const int wrap = 0x1000 - hpos;
            if (wrap >= W) { s_start = W; s_end = W; t_base = 0; }
            else           { s_start = wrap;
                             s_end   = (wrap + 0x200 < W) ? (wrap + 0x200) : W;
                             t_base  = 0; }
        }
    }
}

void init()
{
    if (!mask_buf_cached) {
        mask_buf_cached = memalign(16, MASK_BUF_BYTES);
        mask_buf        = (uint8_t*)UncachedAddr(mask_buf_cached);
    }
    if (!tlut_buf_cached) {
        tlut_buf_cached = memalign(16, TLUT_BUF_BYTES);
        tlut_buf        = (uint16_t*)UncachedAddr(tlut_buf_cached);
    }
    if (mask_buf) std::memset(mask_buf, 0, MASK_BUF_BYTES);
    if (tlut_buf) std::memset(tlut_buf, 0, TLUT_BUF_BYTES);
    for (int y = 0; y < MAX_LINES; y++) line[y].kind = SKIP;
}

void shutdown()
{
    if (mask_buf_cached) { free(mask_buf_cached); mask_buf_cached = nullptr; mask_buf = nullptr; }
    if (tlut_buf_cached) { free(tlut_buf_cached); tlut_buf_cached = nullptr; tlut_buf = nullptr; }
}

bool should_skip_cpu(uint8_t /*road_control*/)
{
    return enabled;
}

} // namespace hwroad_rdp
} // namespace n64

// ---------------------------------------------------------------------------
// HWRoad::build_foreground_lores_rdp
//
// CPU-only phase. Replays the same color_offset / data0 / hpos / color_table
// math the CPU rasteriser uses, but emits its result into the uncached mask
// + per-line TLUT buffers instead of writing RGBA5551 into a scratch surface.
// No RDP commands are issued here. Called from prepare_frame so the previous
// frame's RDP work has time to drain; an rspq_wait() at entry guarantees we
// never overwrite a mask/TLUT the RDP is still reading. Empirically, running
// concurrently with RDP DMA inflates per-pixel cost 3-4× via RDRAM bus
// contention.
// ---------------------------------------------------------------------------
void HWRoad::build_foreground_lores_rdp(const uint16_t* rgb_lut)
{
    using namespace n64::hwroad_rdp;

    if (!mask_buf || !tlut_buf) return;

    const uint8_t ctrl = road_control & 3;
    s_frame++;
    if ((s_frame % 60) == 0)
        debugf("hwroad_rdp: ctrl=%u last_us=%lu\n",
               (unsigned)ctrl, (unsigned long)last_us);

    uint64_t t_wait0 = get_ticks_us();
    // Block on the previous frame's RDP work so this frame's mask/TLUT writes
    // don't race the RDP reading last frame's. Also keeps the RDRAM bus
    // empty for the upcoming uncached stores.
    rspq_wait();
    uint64_t t0 = get_ticks_us();
    const uint32_t sub_wait = (uint32_t)(t0 - t_wait0);

    // Sub-phase counters inside the per-row loop:
    //   sub_tlut: TLUT entry writes (14 rgb_lut indexed loads per row)
    //   sub_spans: span computation, color_idx + hpos lookups
    //   sub_fill : pre-fill + segment direct-pack into uncached mask
    //   sub_rows : count of DRAW rows
    uint32_t sub_tlut = 0, sub_spans = 0, sub_fill = 0;
    uint32_t sub_rows = 0;

    uint16_t* roadram = ramBuff;
    const int W      = config.s16_width;
    const uint16_t s16_x = 0x5f8 + config.s16_x_off;

    static const uint8_t priority_map[2][8] =
    {
        { 0x80, 0x81, 0x81, 0x87, 0, 0, 0, 0x00 },
        { 0x81, 0x81, 0x81, 0x8f, 0, 0, 0, 0x80 }
    };

    // Per-frame merged index LUT. merged_idx[(p0 << 3) | p1] gives the
    // 4-bit TLUT slot for the (pix0=p0, pix1=p1) decision:
    //   0..7  -> road0 colour table entry p0
    //   8..15 -> road1 colour table entry p1
    // For single-road controls (0/3) the table is independent of the
    // unused road's pixel value.
    uint8_t merged_idx[64];
    if (ctrl == 0) {
        for (int p0 = 0; p0 < 8; p0++)
            for (int p1 = 0; p1 < 8; p1++)
                merged_idx[(p0 << 3) | p1] = (uint8_t)p0;
    } else if (ctrl == 3) {
        for (int p0 = 0; p0 < 8; p0++)
            for (int p1 = 0; p1 < 8; p1++)
                merged_idx[(p0 << 3) | p1] = (uint8_t)(8 + p1);
    } else {
        const uint8_t* pmap_row = priority_map[ctrl - 1];
        for (int p0 = 0; p0 < 8; p0++) {
            const uint8_t row = pmap_row[p0];
            for (int p1 = 0; p1 < 8; p1++)
                merged_idx[(p0 << 3) | p1] =
                    (uint8_t)(((row >> p1) & 1) ? (8 + p1) : p0);
        }
    }

    // Collapsed slot LUTs used by segment-based fills below. slot8r0[p]
    // == merged_idx[(p,3)] (in-road0, road1 OOB); slot8r1[p] ==
    // merged_idx[(3,p)] (in-road1, road0 OOB). oob_slot == merged_idx[(3,3)],
    // the OOB / both-bg colour — matches the per-row fill rect.
    uint8_t slot8r0[8], slot8r1[8];
    for (int p = 0; p < 8; p++) {
        slot8r0[p] = merged_idx[(p << 3) | 3];
        slot8r1[p] = merged_idx[(3 << 3) | p];
    }
    const uint8_t oob_slot = merged_idx[(3 << 3) | 3];

    // ---- Phase 1: CPU pass — replicate render_foreground_lores math
    // and build CI4 masks + per-line TLUTs. -------------------------------

    for (int y = 0; y < MAX_LINES; y++)
    {
        const uint32_t data0 = roadram[0x000 + y];
        const uint32_t data1 = roadram[0x100 + y];

        // Same skip conditions as the CPU path
        if (((data0 & 0x800) != 0) && ((data1 & 0x800) != 0))
        { line[y].kind = SKIP; continue; }
        if (ctrl == 0 && ((data0 & 0x800) != 0))
        { line[y].kind = SKIP; continue; }
        if (ctrl == 3 && ((data1 & 0x800) != 0))
        { line[y].kind = SKIP; continue; }

        uint64_t r_t0 = get_ticks_us();

        const int32_t color0_idx = ((road_control & 4) != 0)
                                      ? y : (int32_t)(data0 & 0x1ff);
        const int32_t color1_idx = ((road_control & 4) != 0)
                                      ? (0x100 + y) : (int32_t)(data1 & 0x1ff);
        const int32_t color0 = roadram[0x600 + color0_idx];
        const int32_t color1 = roadram[0x600 + color1_idx];
        int32_t hpos0 = roadram[0x200 + color0_idx] & 0xfff;
        int32_t hpos1 = roadram[0x400 + color1_idx] & 0xfff;

        uint8_t* src0 = ((data0 & 0x800) != 0)
                          ? roads + 256 * 2 * 512
                          : roads + (0x000 + ((data0 >> 1) & 0xff)) * 512;
        uint8_t* src1 = ((data1 & 0x800) != 0)
                          ? roads + 256 * 2 * 512
                          : roads + (0x100 + ((data1 >> 1) & 0xff)) * 512;

        uint64_t r_t1 = get_ticks_us();
        sub_spans += (uint32_t)(r_t1 - r_t0);

        // TLUT entries. Slots 0..7 = road0 c[0..7]; 8..15 = road1 c[0..7].
        // Slots 4/5/6 in each half are unused but cheap to leave zeroed.
        uint16_t* tlut = tlut_ptr(y);
        tlut[0] = rgb_lut[color_offset1 ^ 0x00 ^ ((color0 >> 0) & 1)];
        tlut[1] = rgb_lut[color_offset1 ^ 0x02 ^ ((color0 >> 1) & 1)];
        tlut[2] = rgb_lut[color_offset1 ^ 0x04 ^ ((color0 >> 2) & 1)];
        tlut[7] = rgb_lut[color_offset1 ^ 0x06 ^ ((color0 >> 3) & 1)];
        {
            int32_t bg = (color0 >> 8) & 0xf;
            tlut[3] = ((data0 & 0x200) != 0)
                         ? tlut[0]
                         : rgb_lut[color_offset2 ^ 0x00 ^ bg];
        }
        tlut[8]  = rgb_lut[color_offset1 ^ 0x08 ^ ((color1 >> 4) & 1)];
        tlut[9]  = rgb_lut[color_offset1 ^ 0x0a ^ ((color1 >> 5) & 1)];
        tlut[10] = rgb_lut[color_offset1 ^ 0x0c ^ ((color1 >> 6) & 1)];
        tlut[15] = rgb_lut[color_offset1 ^ 0x0e ^ ((color1 >> 7) & 1)];
        {
            int32_t bg = (color1 >> 8) & 0xf;
            tlut[11] = ((data1 & 0x200) != 0)
                          ? tlut[8]
                          : rgb_lut[color_offset2 ^ 0x10 ^ bg];
        }
        tlut[4] = 0; tlut[5] = 0; tlut[6] = 0;
        tlut[12] = 0; tlut[13] = 0; tlut[14] = 0;

        uint64_t r_t2 = get_ticks_us();
        sub_tlut += (uint32_t)(r_t2 - r_t1);

        hpos0 = (hpos0 - (s16_x + x_offset)) & 0xfff;
        hpos1 = (hpos1 - (s16_x + x_offset)) & 0xfff;

        int s0s, s0e, t0b, s1s, s1e, t1b;
        compute_span(hpos0, W, s0s, s0e, t0b);
        compute_span(hpos1, W, s1s, s1e, t1b);

        // c_oob = TLUT[merged_idx[(3,3)]]. Single-road controls collapse
        // to that road's c[3].
        const uint8_t oob_slot = merged_idx[(3 << 3) | 3];
        const uint16_t c_oob   = tlut[oob_slot];

        // Active span used to size the mask + textured rect.
        int span_start, span_end;
        if (ctrl == 0)      { span_start = s0s; span_end = s0e; }
        else if (ctrl == 3) { span_start = s1s; span_end = s1e; }
        else {
            span_start = (s0s < s1s) ? s0s : s1s;
            span_end   = (s0e > s1e) ? s0e : s1e;
        }

        line[y].s_start = (uint16_t)span_start;
        line[y].s_end   = (uint16_t)span_end;
        line[y].c_oob   = c_oob;

        if (span_end <= span_start) { line[y].kind = OOB_ONLY; continue; }

        const int span       = span_end - span_start;
        const int byte_count = (span + 1) >> 1;
        uint8_t* mask        = mask_ptr(y);   // uncached alias

        uint64_t r_t3 = get_ticks_us();
        sub_spans += (uint32_t)(r_t3 - r_t2);

        // Direct-pack into the uncached mask: each segment computes pairs
        // of slots and stores one mask byte (high nibble = first pixel).
        // Uncached, sequential — store buffer coalesces into RDRAM bursts.
        // Segment boundaries that land on an odd nibble RMW just the
        // affected nibble (~30 cycles each, a handful per row).
        //
        // The mask is pre-filled with (oob_slot:oob_slot) so gap regions
        // (e.g. the in-between span when roads don't overlap in ctrl=1/2)
        // pick up the right colour with no extra writes.

        // --- Pre-fill mask with oob_pair via 4-byte uncached stores -----
        const uint8_t oob_pair = (uint8_t)((oob_slot << 4) | oob_slot);
        {
            const uint32_t v32 = (uint32_t)oob_pair * 0x01010101u;
            uint32_t* m32 = (uint32_t*)mask;
            const int n32 = byte_count >> 2;
            for (int j = 0; j < n32; j++) m32[j] = v32;
            for (int j = n32 << 2; j < byte_count; j++) mask[j] = oob_pair;
        }

        const uint8_t* p0 = src0 + (t0b - s0s);
        const uint8_t* p1 = src1 + (t1b - s1s);

        // Segment writer: walks [lo, hi) by 2 pixels, packs nibble pairs
        // into mask starting at offset (lo - span_start). Boundaries that
        // hit an odd nibble RMW only the affected nibble.
        #define PACK_SEGMENT(LO, HI, GET_SLOT)                              \
            do {                                                            \
                int _lo = (LO);                                             \
                const int _hi = (HI);                                       \
                if (_lo < _hi) {                                            \
                    int _n   = _lo - span_start;                            \
                    int _idx = _n >> 1;                                     \
                    if (_n & 1) {                                           \
                        const uint8_t _s = (uint8_t)(GET_SLOT(_lo));        \
                        mask[_idx] = (uint8_t)((mask[_idx] & 0xF0)          \
                                              | (_s & 0x0F));               \
                        _lo++; _idx++;                                      \
                    }                                                       \
                    while (_lo + 1 < _hi) {                                 \
                        const uint8_t _a = (uint8_t)(GET_SLOT(_lo));        \
                        const uint8_t _b = (uint8_t)(GET_SLOT(_lo + 1));    \
                        mask[_idx++] = (uint8_t)((_a << 4) | _b);           \
                        _lo += 2;                                           \
                    }                                                       \
                    if (_lo < _hi) {                                        \
                        const uint8_t _s = (uint8_t)(GET_SLOT(_lo));        \
                        mask[_idx] = (uint8_t)((_s << 4)                    \
                                              | (mask[_idx] & 0x0F));       \
                    }                                                       \
                }                                                           \
            } while (0)

        #define SLOT_R0(X)   (slot8r0[p0[(X)]])
        #define SLOT_R1(X)   (slot8r1[p1[(X)]])
        #define SLOT_BOTH(X) (merged_idx[(p0[(X)] << 3) | p1[(X)]])

        if (ctrl == 0) {
            PACK_SEGMENT(s0s, s0e, SLOT_R0);
        } else if (ctrl == 3) {
            PACK_SEGMENT(s1s, s1e, SLOT_R1);
        } else {
            const int ol = (s0s > s1s) ? s0s : s1s;
            const int oh = (s0e < s1e) ? s0e : s1e;
            if (ol < oh) {
                PACK_SEGMENT(ol, oh, SLOT_BOTH);
                if (s0s < ol) PACK_SEGMENT(s0s, ol, SLOT_R0);
                if (s1s < ol) PACK_SEGMENT(s1s, ol, SLOT_R1);
                if (s0e > oh) PACK_SEGMENT(oh, s0e, SLOT_R0);
                if (s1e > oh) PACK_SEGMENT(oh, s1e, SLOT_R1);
            } else {
                if (s0s < s0e) PACK_SEGMENT(s0s, s0e, SLOT_R0);
                if (s1s < s1e) PACK_SEGMENT(s1s, s1e, SLOT_R1);
            }
        }

        #undef SLOT_R0
        #undef SLOT_R1
        #undef SLOT_BOTH
        #undef PACK_SEGMENT

        uint64_t r_t4 = get_ticks_us();
        sub_fill += (uint32_t)(r_t4 - r_t3);
        sub_rows++;

        line[y].kind = DRAW;
    }

    uint64_t tA = get_ticks_us();

    // mask_buf and tlut_buf are accessed via uncached aliases — every byte
    // written above already hit RDRAM via the store buffer, so the RDP
    // DMAs see fresh data without an explicit writeback.

    last_us = (last_us * 7 + (uint32_t)(tA - t0)) >> 3;

    if ((s_frame % 60) == 0) {
        debugf("hwroad_rdp build: wait=%lu tlut=%lu spans=%lu fill=%lu rows=%lu total=%lu\n",
               (unsigned long)sub_wait,
               (unsigned long)sub_tlut,
               (unsigned long)sub_spans,
               (unsigned long)sub_fill,
               (unsigned long)sub_rows,
               (unsigned long)(tA - t0));
    }
}

// ---------------------------------------------------------------------------
// HWRoad::emit_foreground_lores_rdp
//
// RDP-only phase. Reads the per-line state, mask_buf, and tlut_buf produced
// by build_foreground_lores_rdp (this frame) and emits a per-line fill
// rectangle plus a CI4 + TLUT textured rectangle. Caller must have a target
// rdpq_attach'd; coordinates are framebuffer-space.
// ---------------------------------------------------------------------------
void HWRoad::emit_foreground_lores_rdp(int x_off, int y_off)
{
    using namespace n64::hwroad_rdp;

    if (!mask_buf || !tlut_buf) return;

    uint64_t t0 = get_ticks_us();
    const int W = config.s16_width;

    // ---- Phase 2a: per-line OOB fill --------------------------------------

    rdpq_set_mode_fill(RGBA32(0, 0, 0, 0));
    for (int y = 0; y < MAX_LINES; y++)
    {
        if (line[y].kind == SKIP) continue;
        rdpq_set_fill_color(rgba32_from_5551(line[y].c_oob));
        rdpq_fill_rectangle(x_off, y_off + y,
                            x_off + W, y_off + y + 1);
    }

    uint64_t tB = get_ticks_us();

    // ---- Phase 2b: CI4 textured rect per active line ----------------------

    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);

    // TILE0 = CI4 draw view at TMEM[0]. TILE1 = I8 load view (RDP's 4bpp
    // load constraint). MASK_BYTES is the max row pitch we ever load.
    rdpq_set_tile(TILE0, FMT_CI4, 0, MASK_BYTES, NULL);
    rdpq_set_tile_size(TILE0, 0, 0, MAX_SPAN_PX, 1);
    rdpq_set_tile(TILE1, FMT_I8,  0, MASK_BYTES, NULL);

    for (int y = 0; y < MAX_LINES; y++)
    {
        if (line[y].kind != DRAW) continue;

        const int span       = line[y].s_end - line[y].s_start;
        const int span_bytes = (span + 1) >> 1;
        const int x0 = x_off + line[y].s_start;
        const int x1 = x_off + line[y].s_end;

        rdpq_tex_upload_tlut(tlut_ptr(y), 0, TLUT_ENTRIES);

        rdpq_set_texture_image_raw(0, PhysicalAddr(mask_ptr(y)),
                                   FMT_I8, span_bytes, 1);
        rdpq_load_tile(TILE1, 0, 0, span_bytes, 1);

        rdpq_texture_rectangle(TILE0,
                               x0, y_off + y,
                               x1, y_off + y + 1,
                               0, 0);
    }

    uint64_t tC = get_ticks_us();

    if ((s_frame % 60) == 0) {
        debugf("hwroad_rdp emit: fill=%lu tex=%lu total=%lu\n",
               (unsigned long)(tB - t0),
               (unsigned long)(tC - tB),
               (unsigned long)(tC - t0));
    }
}
