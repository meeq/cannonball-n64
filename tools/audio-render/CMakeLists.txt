# -----------------------------------------------------------------------------
# audio-render — host-side asset pipeline tool.
#
# Compiles the OutRun audio simulation (engine/audio + hwaudio + romloader)
# natively for the host and produces a CLI that dumps WAV files for any
# Z80 sound command. cmake/audio-payload.cmake builds it at ROM-build time
# and drives it through render-sound.py to generate the wav64 payload from
# the sounds.txt manifest.
#
# Invoke from the repo root:
#   cmake -S tools/audio-render -B build-host/audio-render
#   cmake --build build-host/audio-render
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.13)
project(audio-render CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Resolve repo paths (this file lives at tools/audio-render/CMakeLists.txt).
set(repo_root  "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(main_root  "${repo_root}/src/main")

# This dir precedes ${main_root} so "hwaudio/ym2151.hpp" resolves to the
# tool-local FULL emulator below, not the inert N64 stub in src/main.
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${main_root})

# Engine + chip emulator sources we need to drive the simulation.
set(engine_audio
    "${main_root}/engine/audio/osound.cpp"
    "${main_root}/engine/audio/osoundint.cpp"
)
# The renderer needs the REAL YM2151 — src/main/hwaudio/ym2151.* is an inert
# stub on the N64 target (runtime playback is pre-rendered wav64s, so the
# stub's get_buffer() returns NULL and would segfault any render). The full
# Burczynski/MAME emulator lives under tools/audio-render/hwaudio/ for host
# builds only.
set(hwaudio
    "${main_root}/hwaudio/segapcm.cpp"
    "${main_root}/hwaudio/soundchip.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hwaudio/ym2151.cpp"
)
set(romloader
    "${main_root}/roms.cpp"
    "${main_root}/romloader.cpp"
    "${main_root}/utils_crc32.cpp"
)
set(driver
    "${CMAKE_CURRENT_SOURCE_DIR}/render.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/wav_writer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs.cpp"
)

add_executable(audio-render
    ${engine_audio}
    ${hwaudio}
    ${romloader}
    ${driver}
)

target_compile_options(audio-render PRIVATE -Wall -Wno-deprecated-declarations)
