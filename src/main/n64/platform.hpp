/***************************************************************************
    N64 / libdragon platform helpers.

    Shared helpers and constants used across the N64 platform layer. Pulls in
    <libdragon.h> for the rest of n64 sources.
***************************************************************************/

#pragma once

#include <libdragon.h>

#include "../stdint.hpp"
#include "../globals.hpp"

namespace n64
{
    // libdragon declares RESOLUTION_320x240 / DEPTH_16_BPP as runtime `const`,
    // not `constexpr`, so these need const-not-constexpr too.
    const resolution_t FB_RES       = RESOLUTION_320x240;
    const bitdepth_t   FB_DEPTH     = DEPTH_16_BPP;
    constexpr int      FB_COUNT     = 2;   // double-buffered
    constexpr int      FB_WIDTH     = 320;
    constexpr int      FB_HEIGHT    = 240;

    // Engine pixel area is centred vertically: S16_HEIGHT (224) into 240.
    constexpr int      ENGINE_Y_OFFSET = (FB_HEIGHT - S16_HEIGHT) / 2;
}
