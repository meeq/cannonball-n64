/***************************************************************************
    Internal-use header shared by hwroad_rdp.cpp (CPU build path) and
    hwroad_rdp_rsp.cpp (RSP build path). Exposes the storage and types
    that both builders populate and emit_foreground_lores_rdp consumes.
    Not part of the public n64/hwroad_rdp.hpp API.
***************************************************************************/

#pragma once

#include "n64/platform.hpp"

namespace n64
{
namespace hwroad_rdp
{
namespace detail
{
    constexpr int MAX_LINES    = S16_HEIGHT;    // 224
    constexpr int MAX_SPAN_PX  = 320;           // s16 active width
    constexpr int MASK_BYTES   = (MAX_SPAN_PX + 1) / 2;
    constexpr int TLUT_ENTRIES = 16;

    enum LineKind : uint8_t { SKIP = 0, OOB_ONLY = 1, DRAW = 2 };

    struct LineState
    {
        uint8_t  kind;
        uint16_t s_start, s_end;
        uint16_t c_oob;
    };

    // Defined in hwroad_rdp.cpp. mask_buf / tlut_buf are uncached aliases
    // (KSEG1) — writes go through the R4300 store buffer with no cache
    // traffic; RDP DMAs read fresh data without an explicit writeback.
    extern uint8_t*  mask_buf;
    extern uint16_t* tlut_buf;
    extern LineState line[MAX_LINES];

    inline uint8_t*  mask_ptr(int y) { return mask_buf + (size_t)y * MASK_BYTES; }
    inline uint16_t* tlut_ptr(int y) { return tlut_buf + (size_t)y * TLUT_ENTRIES; }
}
}
}
