# -----------------------------------------------------------------------------
# audio-render — host-side asset pipeline tool.
#
# Compiles the OutRun audio simulation (engine/audio + hwaudio + romloader)
# natively for the host and produces a CLI that dumps WAV files for any
# Z80 sound command. Reused at build time to generate the wav64 payload
# that ships with the N64 ROM (see render-all.sh).
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

include_directories(${main_root})

# Engine + chip emulator sources we need to drive the simulation.
set(engine_audio
    "${main_root}/engine/audio/osound.cpp"
    "${main_root}/engine/audio/osoundint.cpp"
)
set(hwaudio
    "${main_root}/hwaudio/segapcm.cpp"
    "${main_root}/hwaudio/soundchip.cpp"
    "${main_root}/hwaudio/ym2151.cpp"
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

# Companion tool: reads OSound::loop_fires across ticks to detect each
# music track's loop period directly from the source data. Output drives
# the per-track render durations in sounds.txt.
add_executable(detect-loop
    ${engine_audio}
    ${hwaudio}
    ${romloader}
    "${CMAKE_CURRENT_SOURCE_DIR}/detect_loop.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs.cpp"
)

# Companion tool: finds the TRUE song loop period by exhaustively
# fingerprinting the music engine state (chan_ram + pcm_ram) every tick
# and reporting when it first exactly repeats. Independent of any
# assumption about per-channel LOOP_FOREVER structure.
add_executable(find-song-loop
    ${engine_audio}
    ${hwaudio}
    ${romloader}
    "${CMAKE_CURRENT_SOURCE_DIR}/find_song_loop.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs.cpp"
)

# Companion tool: static MML interpreter. Walks the Z80 music data for a
# given music id without running the simulator, so it can answer the
# question "what is the structural loop period of this track?" even when
# the simulator's LOOP_FOREVER never fires twice.
add_executable(analyze-music
    ${romloader}
    "${CMAKE_CURRENT_SOURCE_DIR}/analyze_music.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs.cpp"
)

# Companion tool: combined structural+acoustic loop finder. Require the
# MML walker state (chan_ram + pcm_ram + aux) to match exactly across the
# splice (no mid-phrase wrap), then minimise the YM2151+SegaPCM internal-
# state delta across all such candidate (intro, period) pairs.
add_executable(find-music-loop
    ${engine_audio}
    ${hwaudio}
    ${romloader}
    "${CMAKE_CURRENT_SOURCE_DIR}/find_music_loop.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs.cpp"
)

# Diagnostic: dumps SEQ_POS + ancillary chan_ram bytes per frame so we can
# see whether the Z80 MML walker truly cycles and at what period.
add_executable(diagnose-chan-ram
    ${engine_audio}
    ${hwaudio}
    ${romloader}
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnose_chan_ram.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs.cpp"
)

# The engine code is older and a bit warning-noisy under strict modes; keep
# warnings on but don't fail the build over them.
add_executable(dump-bytes
    ${romloader}
    "${CMAKE_CURRENT_SOURCE_DIR}/dump_bytes.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs.cpp"
)

target_compile_options(audio-render   PRIVATE -Wall -Wno-deprecated-declarations)
target_compile_options(detect-loop    PRIVATE -Wall -Wno-deprecated-declarations)
target_compile_options(find-song-loop PRIVATE -Wall -Wno-deprecated-declarations)
target_compile_options(analyze-music   PRIVATE -Wall -Wno-deprecated-declarations)
target_compile_options(find-music-loop PRIVATE -Wall -Wno-deprecated-declarations)
target_compile_options(dump-bytes      PRIVATE -Wall -Wno-deprecated-declarations)
