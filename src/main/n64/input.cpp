/***************************************************************************
    N64 Input — libdragon joypad backing.

    Mapping (P1, JOYPAD_PORT_1):
      A       → ACCEL
      B       → BRAKE
      Z       → BRAKE
      L       → BRAKE
      R       → GEAR (toggle)
      C-Down  → GEAR (toggle)
      C-Up    → VIEWPOINT
      C-Left  → MUSIC_PREV (in-game music cycle backward)
      C-Right → MUSIC_NEXT (in-game music cycle forward)
      START   → START (also credits up in freeplay mode; opens pause menu)
      D-pad   → directional UP/DOWN/LEFT/RIGHT
      Stick   → analog steering / accelerator when analog enabled
***************************************************************************/

#include "input.hpp"
#include "platform.hpp"
#include <cstring>

Input::Input()
    : keys{}, keys_old{}, motor_limits{}, gamepad(false),
      rumble_supported(0), analog(0), key_press(-1), joy_button(-1),
      wheel(CENTRE), a_wheel(CENTRE), a_accel(0), a_brake(0), a_motor(0)
{
}

Input::~Input() = default;

void Input::init(int /*pad_id*/,
                 int* /*key_config*/, int* /*pad_config*/,
                 const int analog_in,
                 int* /*axis*/, bool* /*invert*/, int* /*asettings*/)
{
    joypad_init();
    analog  = analog_in;
    gamepad = true;

    // Rumble Pak detection — refreshed on hotplug
    joypad_poll();
    rumble_supported = joypad_get_rumble_supported(JOYPAD_PORT_1) ? 1 : 0;

    std::memset(keys, 0, sizeof(keys));
    std::memset(keys_old, 0, sizeof(keys_old));
    std::memset(motor_limits, 0, sizeof(motor_limits));
    wheel = a_wheel = CENTRE;
    a_accel = a_brake = a_motor = 0;
}

void Input::close_joy()
{
    joypad_close();
    gamepad = false;
}

void Input::poll()
{
    joypad_poll();
    joypad_buttons_t held    = joypad_get_buttons(JOYPAD_PORT_1);
    joypad_inputs_t  inputs  = joypad_get_inputs(JOYPAD_PORT_1);

    const bool brake = held.b || held.z || held.l;
    const bool gear  = held.r || held.c_down;

    keys[LEFT]      = held.d_left  || inputs.stick_x < -40;
    keys[RIGHT]     = held.d_right || inputs.stick_x >  40;
    keys[UP]        = held.d_up    || inputs.stick_y >  40;
    keys[DOWN]      = held.d_down  || inputs.stick_y < -40;
    keys[ACCEL]     = held.a;
    keys[BRAKE]     = brake;
    keys[GEAR1]     = gear;
    keys[GEAR2]     = false;
    keys[START]     = held.start;
    keys[COIN]      = false;
    keys[VIEWPOINT] = held.c_up;
    keys[PAUSE]     = false;
    keys[STEP]      = false;
    keys[TIMER]     = false;
    keys[MENU]      = false;
    keys[MUSIC_PREV] = held.c_left;
    keys[MUSIC_NEXT] = held.c_right;

    // Analog steering: stick_x range roughly [-80, +80] → wheel 0..0xFF
    if (analog)
    {
        int x = inputs.stick_x;
        if (x < -80) x = -80;
        if (x >  80) x =  80;
        wheel   = CENTRE + (x * (CENTRE - 1) / 80);
        a_wheel = wheel;

        a_accel = held.a ? 0xFF : 0;
        a_brake = brake   ? 0xFF : 0;
    }
}

void Input::frame_done()
{
    std::memcpy(keys_old, keys, sizeof(keys));
}

bool Input::is_pressed(presses p)       { return keys[p]; }
bool Input::is_pressed_clear(presses p) { bool r = keys[p]; keys[p] = false; return r; }
bool Input::has_pressed(presses p)      { return keys[p] && !keys_old[p]; }

void Input::reset_axis_config() {}
int  Input::get_axis_config()   { return 0; }

void Input::set_rumble(bool enable, float /*strength*/)
{
    if (rumble_supported)
        joypad_set_rumble_active(JOYPAD_PORT_1, enable);
}
