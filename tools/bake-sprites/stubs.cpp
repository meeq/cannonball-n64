// Minimal globals to satisfy the linker when roms.cpp + romloader.cpp are
// compiled standalone (no game loop, no XML config).
//
// romloader.cpp only reads config.data.rom_path; the driver writes it
// directly after Config is default-constructed.

#include "frontend/config.hpp"

Config  config;

Config::Config(void)  {}
Config::~Config(void) {}
