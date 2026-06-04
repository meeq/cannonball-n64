# -----------------------------------------------------------------------------
# audio-payload — generate VADPCM wav64 files from the OutRun ROM and stage
# them into the DFS root for n64_create_rom.
#
# The parent build is cross-compiling to N64; this module configures and
# builds the host audio-render tool out-of-tree (via a nested cmake), reads
# tools/audio-render/sounds.txt, emits one .wav64 per row into the build
# tree, and copies each one into ${N64_DFS_ROOT}/audio/.
#
# Inputs from caller (set BEFORE include):
#   N64_DFS_ROOT     — DFS staging directory (the same one passed to
#                      n64_create_rom). The module appends /audio/*.wav64.
#
# Outputs:
#   AUDIO_PAYLOAD_STAGED_FILES  — list of staged DFS paths; pass to
#                                 n64_create_rom(... EXTRA_DFS_DEPS ...)
#   target  audio-payload       — ALL target that produces everything above
#
# Pipeline per row:
#   audio-render (host C++) --> raw WAV (44100 Hz)
#        |
#        v  music_loop only: ffmpeg trims to sample-exact (intro+loop)
#        |
#   audioconv64 --> VADPCM wav64 (22050 Hz, loop offset set for music_loop)
#        |
#        v
#   copy_if_different into ${N64_DFS_ROOT}/audio/
#
# Externals required at build time:
#   audioconv64 — ships with libdragon; located via N64_BINDIR
#   ffmpeg      — system; for sample-exact trim of looped tracks
#   python3     — orchestrates one pipeline invocation per row
# -----------------------------------------------------------------------------

if(NOT DEFINED N64_DFS_ROOT)
    message(FATAL_ERROR "audio-payload: caller must set N64_DFS_ROOT before include()")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

find_program(AUDIOCONV64_EXE audioconv64
    PATHS "${N64_BINDIR}"
    DOC   "libdragon audioconv64 (VADPCM encoder)"
    REQUIRED)

find_program(FFMPEG_EXE ffmpeg
    DOC   "ffmpeg (used to trim loop tracks to sample-exact length)"
    REQUIRED)

set(_AUDIO_TOOL_SRC   "${CMAKE_CURRENT_SOURCE_DIR}/../tools/audio-render")
set(_AUDIO_MANIFEST   "${_AUDIO_TOOL_SRC}/sounds.txt")
set(_RENDER_SOUND_PY  "${_AUDIO_TOOL_SRC}/render-sound.py")
set(_ROMS_DIR         "${CMAKE_CURRENT_SOURCE_DIR}/../roms/")
set(_AUDIO_RAW_DIR    "${CMAKE_BINARY_DIR}/audio/raw")
set(_AUDIO_WAV64_DIR  "${CMAKE_BINARY_DIR}/audio/wav64")
set(_AUDIO_STAGE_DIR  "${N64_DFS_ROOT}/audio")
file(MAKE_DIRECTORY "${_AUDIO_RAW_DIR}" "${_AUDIO_WAV64_DIR}" "${_AUDIO_STAGE_DIR}")

# -----------------------------------------------------------------------------
# Host audio-render tool — build via a child cmake invocation. The child
# process inherits no parent CMake state, so it auto-detects the host C++
# compiler instead of using this build's mips64-elf toolchain.
#
# The configure + build steps are fused into a single custom_command output
# so Make doesn't try to parallelise them; downstream rows depend on the
# resulting binary path.
# -----------------------------------------------------------------------------
set(_AUDIO_HOST_BLD "${CMAKE_BINARY_DIR}/audio-render-host")
set(_AUDIO_HOST_BIN "${_AUDIO_HOST_BLD}/audio-render")

file(GLOB _AUDIO_HOST_SRCS
    "${_AUDIO_TOOL_SRC}/*.cpp"
    "${_AUDIO_TOOL_SRC}/*.hpp"
    "${_AUDIO_TOOL_SRC}/CMakeLists.txt")

add_custom_command(
    OUTPUT  "${_AUDIO_HOST_BIN}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_AUDIO_HOST_BLD}"
    COMMAND ${CMAKE_COMMAND} -S "${_AUDIO_TOOL_SRC}"
                             -B "${_AUDIO_HOST_BLD}"
                             -DCMAKE_BUILD_TYPE=Release
                             -G "${CMAKE_GENERATOR}"
    COMMAND ${CMAKE_COMMAND} --build "${_AUDIO_HOST_BLD}"
                             --target audio-render
    DEPENDS ${_AUDIO_HOST_SRCS}
    COMMENT "[AUDIO] host audio-render"
    VERBATIM)

# -----------------------------------------------------------------------------
# Per-row pipeline. Each row emits two chained custom_commands:
#   1. render+encode  -> ${_AUDIO_WAV64_DIR}/$name.wav64
#   2. stage          -> ${_AUDIO_STAGE_DIR}/$name.wav64
# Both rules live under the single `audio-payload` target's build.make,
# avoiding the Make-parallelism gotcha where rules get duplicated into
# multiple .dir/build.make files.
# -----------------------------------------------------------------------------
function(_audio_payload_add KIND ID FILENAME)
    set(_raw_wav   "${_AUDIO_RAW_DIR}/${FILENAME}")
    string(REGEX REPLACE "\\.wav$" ".wav64" _wav64_name "${FILENAME}")
    set(_wav64_out "${_AUDIO_WAV64_DIR}/${_wav64_name}")
    set(_staged    "${_AUDIO_STAGE_DIR}/${_wav64_name}")

    set(_args
        --render-bin "${_AUDIO_HOST_BIN}"
        --audioconv  "${AUDIOCONV64_EXE}"
        --ffmpeg     "${FFMPEG_EXE}"
        --roms       "${_ROMS_DIR}"
        --out-wav    "${_raw_wav}"
        --out-wav64  "${_wav64_out}"
        --id         "${ID}"
        --kind       "${KIND}")

    if(KIND STREQUAL "music_loop")
        list(APPEND _args --intro "${ARGV3}" --loop "${ARGV4}")
    else()
        list(APPEND _args --duration "${ARGV3}")
    endif()

    add_custom_command(
        OUTPUT  "${_wav64_out}" "${_raw_wav}"
        COMMAND ${Python3_EXECUTABLE} "${_RENDER_SOUND_PY}" ${_args}
        DEPENDS "${_AUDIO_HOST_BIN}"
                "${_RENDER_SOUND_PY}"
                "${_AUDIO_MANIFEST}"
        COMMENT "[AUDIO] ${_wav64_name}"
        VERBATIM)

    add_custom_command(
        OUTPUT  "${_staged}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_wav64_out}" "${_staged}"
        DEPENDS "${_wav64_out}"
        COMMENT "[AUDIO] stage ${_wav64_name}"
        VERBATIM)

    set(AUDIO_PAYLOAD_STAGED_FILES
        "${AUDIO_PAYLOAD_STAGED_FILES};${_staged}" PARENT_SCOPE)
endfunction()

# -----------------------------------------------------------------------------
# Parse the manifest. One whitespace-tokenised row per entry.
# UTF-8 encoding required so the file's em-dashes don't get replaced with
# `;` separators and chopped into spurious tokens.
# -----------------------------------------------------------------------------
set(AUDIO_PAYLOAD_STAGED_FILES "")

file(STRINGS "${_AUDIO_MANIFEST}" _SOUND_LINES ENCODING UTF-8)
foreach(_line IN LISTS _SOUND_LINES)
    string(REGEX REPLACE "#.*$" "" _line "${_line}")
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE "[ \t]+" ";" _tokens "${_line}")
    list(LENGTH _tokens _n)
    list(GET _tokens 0 _id)
    list(GET _tokens 1 _kind)
    list(GET _tokens 2 _filename)

    if(_kind STREQUAL "music_loop")
        if(NOT _n EQUAL 5)
            message(FATAL_ERROR "audio manifest: music_loop row needs intro + loop: ${_line}")
        endif()
        list(GET _tokens 3 _intro)
        list(GET _tokens 4 _loop)
        _audio_payload_add(${_kind} ${_id} ${_filename} ${_intro} ${_loop})
    elseif(_kind STREQUAL "music_oneshot" OR _kind STREQUAL "fx")
        if(NOT _n EQUAL 4)
            message(FATAL_ERROR "audio manifest: ${_kind} row needs duration: ${_line}")
        endif()
        list(GET _tokens 3 _duration)
        _audio_payload_add(${_kind} ${_id} ${_filename} ${_duration})
    else()
        message(FATAL_ERROR "audio manifest: unknown kind '${_kind}' in: ${_line}")
    endif()
endforeach()

add_custom_target(audio-payload ALL DEPENDS ${AUDIO_PAYLOAD_STAGED_FILES})
