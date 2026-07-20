Cannonball N64 - OutRun Engine
==============================

CannonBall is a souped up game engine for the OutRun arcade game. The original
68000 and Z80 assembler code was rewritten in C++ by Chris White. This fork is
a Nintendo 64 port: it drops the original cross-platform SDL/Boost host layer
and runs natively on N64 hardware via [libdragon](https://github.com/DragonMinded/libdragon),
producing a self-contained `cannonball.z64` ROM with the OutRun romset and
prerendered audio baked into a DFS payload.

For an overview of the original engine and its features, see the upstream
[manual](https://github.com/djyt/cannonball/wiki). Credit for the engine
itself belongs to Chris White (project creator) and the upstream contributors.

The N64 port requires the original OutRun ROMs, as they contain the graphics
and audio data the engine drives.


Getting Started
---------------

Prerequisites:

* A libdragon toolchain (`mips64-elf-gcc`, `n64tool`, `mkdfs`, `mksprite`,
  `audioconv64`, etc.). Install via
  [libdragon's docker image](https://github.com/DragonMinded/libdragon/wiki/Installing-libdragon)
  or build from source.
* `N64_INST` exported to the toolchain root (the directory containing
  `bin/mips64-elf-gcc`).
* CMake 3.13 or newer.
* A host C++ compiler, Python 3, and `ffmpeg` — the build compiles and runs
  the asset pipeline (audio renderer, sprite/tile baker) on the host machine.
* OutRun revision B romset copied to `roms/` at the project root. The CMake
  step stages the entire directory into the DFS image. The Japanese CPU ROMs
  (`epr-10380`..`epr-10383`) are optional; if present, the in-game OPTIONS
  menu can switch the region to Japan.


Build
-----

Configure and build from the project root:

    export N64_INST=$HOME/Projects/n64/toolchain   # adjust to your install

    cmake -S cmake -B build \
          -DCMAKE_TOOLCHAIN_FILE="$(pwd)/cmake/n64-toolchain.cmake"

    cmake --build build -j

The first build also renders the audio payload: it compiles the host-side
renderer (`tools/audio-render`), replays the OutRun Z80 sound program against
the YM2151 + SegaPCM emulation for every track and sound effect listed in
`tools/audio-render/sounds.txt`, and converts the results to VADPCM `.wav64`.
This adds a few minutes the first time; incremental builds only re-render
when the romset or the renderer changes.

Outputs land in `build/`:

* `cannonball.z64` — the ROM to load on an emulator (ares, cen64) or flash
  cart (EverDrive-64, 64drive, SummerCart64).
* `cannonball.elf` — the unstripped ELF, useful with `mips64-elf-gdb`.
* `cannonball.dfs` — the DFS filesystem image embedded in the ROM.

The audio tooling can also be built standalone (music-loop analysis tools,
one-off renders):

    cmake -S tools/audio-render -B build-host/audio-render
    cmake --build build-host/audio-render


Status
------

* Boot flow: disclaimer + SEGA splash, then a native boot menu offering
  ARCADE, CONTINUOUS, and TIME TRIALS modes plus an OPTIONS screen for the
  persisted settings (difficulty, region, controls, audio, cheats).
* EEPROM (16 Kbit) persistence: high-score tables per mode and region,
  Time Trial best times, and all settings — checksummed with backup copies
  via libdragon's eepfs.
* In-game: pause menu (Continue / Retry / Quit), mid-race music switching on
  C-Left/C-Right (plus a "RADIO OFF" slot), Rumble Pak support, and a
  Japan-region mode that swaps the CPU ROMs in place.
* Performance: engine logic runs at the arcade's 30 Hz; rendering is
  RDP-driven (road, tiles, sprites, text) and floats between 30 and 60 fps
  depending on scene load. Works on baseline 4 MiB consoles; an Expansion
  Pak enlarges the sprite-atlas cache.
* The upstream SDL frontend menu, cabinet diagnostics, and layout editor are
  excluded from the build.
