/***************************************************************************
    N64 / libdragon Input.

    Mirrors the public surface of src/main/sdl2/input.hpp so engine and
    frontend code (engine/oinputs.cpp, engine/outrun.cpp, frontend/ttrial.cpp)
    is platform-agnostic.

    The SDL backend was event-driven (handle_key_*, handle_joy_*). On N64 we
    poll libdragon's joypad once per frame from n64main.cpp via Input::poll().
    The SDL-event handler methods are intentionally absent here — main.cpp
    (the only caller) is excluded from the N64 build.
***************************************************************************/

#pragma once

#include "../stdint.hpp"

class Input
{
public:
    enum presses
    {
        LEFT  = 0,
        RIGHT = 1,
        UP    = 2,
        DOWN  = 3,
        ACCEL = 4,
        BRAKE = 5,
        GEAR1 = 6,
        GEAR2 = 7,

        START = 8,
        COIN  = 9,
        VIEWPOINT = 10,

        PAUSE = 11,
        STEP  = 12,
        TIMER = 13,
        MENU  = 14,

        // N64-only: C-Left / C-Right cycle the in-game music track. No
        // analog in the SDL backend, so the enum is platform-specific.
        MUSIC_PREV = 15,
        MUSIC_NEXT = 16,
    };

    bool keys[17];
    bool keys_old[17];

    enum limits
    {
        SW_LEFT   = 0,
        SW_CENTRE = 1,
        SW_RIGHT  = 2,
    };
    bool motor_limits[3];

    // Has gamepad been found? (always true under libdragon once init() runs
    // — joypad subsystem is always available.)
    bool gamepad;

    // Gamepad supports rumble?
    int rumble_supported;

    // Use analog controls
    int analog;

    // Analog Controls (signed 16-bit space, 0x80 centre matches SDL backend)
    int wheel, a_wheel;
    int a_accel;
    int a_brake;
    int a_motor;

    Input();
    ~Input();

    // Same signature as the SDL backend so frontend/config can call it
    // unconditionally. pad_config/key_config/axis/invert/asettings come from
    // Config — N64 ignores the keyboard-derived bits.
    void init(int pad_id,
              int* key_config, int* pad_config,
              const int analog,
              int* axis, bool* invert, int* asettings);
    void close_joy();

    void frame_done();
    bool is_pressed(presses p);
    bool is_pressed_clear(presses p);
    bool has_pressed(presses p);
    void set_rumble(bool enable, float strength = 1.0f);

    // N64-only: poll libdragon joypad and update keys[]. Called once per
    // frame from n64main.cpp before the engine tick.
    void poll();

private:
    static const int CENTRE = 0x80;
};

extern Input input;
