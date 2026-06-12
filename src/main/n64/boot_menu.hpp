/***************************************************************************
    N64 pre-engine title + options menu.

    Two screens rendered with rdpq_sprite_blit + a prim-colour combiner for
    runtime tinting (CI4 labels/pills get re-coloured, RGBA16 backgrounds pass
    through). Both screens run before the engine initializes — picking a mode
    from the title screen sets outrun.cannonball_mode and returns; entering
    Options recurses into the sub-screen and auto-saves to EEPROM on B.

    Title screen rows:
      ARCADE          → cannonball_mode = MODE_ORIGINAL
      CONTINUOUS      → cannonball_mode = MODE_CONT
      TIME TRIALS     → cannonball_mode = MODE_TTRIAL  (inline laps editor)
      OPTIONS         → sub-screen, returns to title on B

    Options screen rows:
      REGION             world / japan       → engine.jap
      TIME DIFFICULTY    easy..hardest       → engine.dip_time
      TRAFFIC DIFFICULTY easy..hardest       → engine.dip_traffic (synced to
                                                cont_traffic + ttrial.traffic)
      CAR COLOR          red..cyan           → engine.car_pal
      CAR TRANSMISSION   manual / automatic  → controls.gear (BUTTON / AUTO)
      CHEATS pills × 4   tires / bumper /    → engine.grippy_tyres / bumper /
                         turbo / off-road       turbo / offroad
***************************************************************************/

#pragma once

namespace n64 { namespace boot_menu
{
    // Overlay any persisted saved_settings_v1 record onto live `config`.
    // No-op when no record exists. Called before video.boot_display so the
    // platform defaults laid down by platform_overrides() can be overridden
    // ahead of any subsystem read.
    void apply_saved_settings();

    // Show the title screen, optionally recursing into Options. Returns once
    // the player has picked a game mode — outrun.cannonball_mode is set, the
    // saved record is written to EEPROM, and the caller can proceed with
    // engine init.
    //
    // Requires Video::boot_display() to have run (display + rdpq up) and the
    // joypad subsystem to have been initialized.
    void run();
}}
