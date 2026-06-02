/***************************************************************************
    Shared engine-state declarations.
    Definitions live in src/main/n64/n64main.cpp.
***************************************************************************/

#pragma once

#include "globals.hpp"
#include "audio.hpp"

namespace cannonball
{
    extern Audio audio;

    extern int    frame;
    extern bool   tick_frame;
    extern double frame_ms;
    extern int    fps_counter;
    extern int    state;

    enum
    {
        STATE_BOOT,
        STATE_INIT_MENU,
        STATE_MENU,
        STATE_INIT_GAME,
        STATE_GAME,
        STATE_QUIT
    };
}
