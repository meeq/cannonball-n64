/***************************************************************************
    N64 EEPROM-backed save layer.

    Phase 5 wires this into ohiscore.cpp + ttrial.cpp via small platform
    hooks. Phase 1 ships stubs so the link line resolves.
***************************************************************************/

#pragma once

#include "../stdint.hpp"

namespace n64save
{
    bool init();
    bool load_high_scores();
    bool save_high_scores();
    bool load_ttrial_scores();
    bool save_ttrial_scores();
}
