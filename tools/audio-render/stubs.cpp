// Minimal globals to satisfy the linker when the audio engine code is
// compiled standalone (no game loop, no XML config, no rendering).
//
// The audio path only reads:
//   - config.sound.{rate,advertise,enabled}, config.fps
//   - outrun.game_state (used to gate music in attract mode)
//
// We default-construct Config and Outrun, then the render driver writes
// the few fields it actually needs.

#include "frontend/config.hpp"
#include "engine/outrun.hpp"

Config  config;
Outrun  outrun;

Config::Config(void)  {}
Config::~Config(void) {}

// Outrun's real ctor wires up `outputs` and other game subsystems; here
// we just need an empty shell. game_state is set to GS_MUSIC by the
// driver so the attract-mode music filter in osoundint never trips.
Outrun::Outrun()  { outputs = nullptr; game_state = 0; }
Outrun::~Outrun() {}
