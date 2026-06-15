/***************************************************************************
    N64 pre-boot SEGA splash.

    Renders the SEGA wordmark over a cleared white framebuffer in two
    centered 200x80 phases: a CI8 cyan sweep driven by a per-frame TLUT
    swap on sweep.sprite (assets/splash/sweep.png), then an IA4
    silhouette (fade.sprite, assets/splash/fade.png) tinted via the RDP
    colour combiner — PRIMITIVE colour LERPs across five keyframes
    (white → pale cyan → cyan → cyan → deep blue) for fade-in, holds
    deep blue for ~3 s while sega.wav64 plays, then reverses the LERP
    back to white. Holding START at any point skips the rest.

    Requires Video::boot_display() to have run (display + rdpq up) and the
    DFS payload to be mounted. Owns its own audio_init / mixer_init for
    the jingle and tears them down before returning so Audio::init can
    claim a clean subsystem. Cleans up its sprites + drains the RDP
    queue before returning so the boot menu's font load lands on a
    clean queue.
***************************************************************************/

#pragma once

namespace n64 { namespace splash
{
    // Block until the intro animation completes (~6 s at 60 fps).
    void run();
}}
