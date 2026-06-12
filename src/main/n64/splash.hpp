/***************************************************************************
    N64 pre-boot SEGA splash.

    Renders the SEGA wordmark from assets/sega.png as a CI8 sprite with a
    per-frame TLUT swap that drives a five-phase animation: a horizontal
    rainbow cycle (~2 s), a white→cyan→white wave sweep through the palette
    indices, an all-white hold, and a four-frame fade-in to solid blue
    (~1 s hold). Plays once on cold boot before n64::boot_menu::run().

    Requires Video::boot_display() to have run (display + rdpq up) and the
    DFS payload to be mounted. Cleans up its sprite + drains the RDP queue
    before returning so the boot menu's font load lands on a clean queue.
***************************************************************************/

#pragma once

namespace n64 { namespace splash
{
    // Block until the intro animation completes (~3.5 s at 60 fps).
    void run();
}}
