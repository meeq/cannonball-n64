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

* A libdragon toolchain (`mips64-elf-gcc`, `n64tool`, `mkdfs`, etc.). Install
  via [libdragon's docker image](https://github.com/DragonMinded/libdragon/wiki/Installing-libdragon)
  or build from source.
* `N64_INST` exported to the toolchain root (the directory containing
  `bin/mips64-elf-gcc`).
* CMake 3.13 or newer.
* A host C++ compiler (used only to build the offline audio renderer below).
* OutRun revision B romset copied to `roms/` at the project root. The CMake
  step stages the entire directory into the DFS image.


Build
-----

### 1. Render the audio assets (one-time)

The N64 build streams `.wav64` clips produced offline by replaying the OutRun
Z80 sound program against the YM2151 + SegaPCM emulation. Build the renderer
with your host compiler and run it once to populate `audio/wav64/`:

    cmake -S tools/audio-render -B build-host/audio-render
    cmake --build build-host/audio-render

    tools/audio-render/render-all.sh
    tools/audio-render/wav2wav64.sh

This reads from `roms/` and writes `audio/raw/*.wav` then
`audio/wav64/*.wav64`. You only need to re-run it if the romset or the
renderer changes.

### 2. Build the ROM

Configure and build from the project root:

    export N64_INST=$HOME/Projects/n64/toolchain   # adjust to your install

    cmake -S cmake -B build \
          -DCMAKE_TOOLCHAIN_FILE="$(pwd)/cmake/n64-toolchain.cmake"

    cmake --build build -j

Outputs land in `build/`:

* `cannonball.z64` — the ROM to load on an emulator (ares, cen64) or flash
  cart (EverDrive-64, 64drive).
* `cannonball.elf` — the unstripped ELF, useful with `mips64-elf-gdb`.
* `cannonball.dfs` — the DFS filesystem image embedded in the ROM.


Status
------

* The ROM boots straight into attract mode; the in-game menu and cabinet
  diagnostics are still excluded from the build.
* EEPROM save is stubbed.
* Frame-rate target is 30 FPS.
