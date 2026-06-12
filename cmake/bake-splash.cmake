# -----------------------------------------------------------------------------
# bake-splash — convert assets/splash/sega_sweep.png and
# assets/splash/sega_fade/*.png into the two sprites the pre-boot intro
# animation needs.
#
#   sega_sweep.sprite — CI8 baked from assets/splash/sega_sweep.png via
#                       libdragon mksprite. Carries the rainbow-indexed
#                       wordmark used during the sweep phase; the splash
#                       runtime re-indexes its pixels at boot so each
#                       x-stripe gets a unique slot.
#
#   sega_fade.sprite  — CI4 baked from assets/splash/sega_fade/4.png after
#                       a Python preprocess step. The four sega_fade frames
#                       are the same anti-aliased SEGA wordmark with
#                       progressively richer palettes (pale cyan → cyan →
#                       mid blue → deep blue); build_fade.py canonicalises
#                       them into one 7-level indexed PNG plus a generated
#                       header (sega_fade_palettes.h) holding the four
#                       RGBA16 palettes. The runtime cycles those palettes
#                       during the fade phase.
#
# Inputs from caller (set BEFORE include):
#   N64_DFS_ROOT  — DFS staging directory (same one passed to n64_create_rom).
#                   The module appends /splash/.
#
# Outputs:
#   SPLASH_STAGED_SWEEP_SPRITE  — staged DFS path; pass via n64_create_rom EXTRA_DFS_DEPS
#   SPLASH_STAGED_FADE_SPRITE   — staged DFS path; pass via n64_create_rom EXTRA_DFS_DEPS
#   SPLASH_STAGED_AUDIO         — staged DFS path; pass via n64_create_rom EXTRA_DFS_DEPS
#   SPLASH_FADE_HEADER_DIR      — include path for the generated palette header
#   target  bake-splash         — ALL target producing the staged files
# -----------------------------------------------------------------------------

if(NOT DEFINED N64_DFS_ROOT)
    message(FATAL_ERROR "bake-splash: caller must set N64_DFS_ROOT before include()")
endif()

find_program(MKSPRITE_EXE mksprite
    PATHS "${N64_BINDIR}"
    DOC   "libdragon mksprite (PNG -> .sprite converter)"
    REQUIRED)

find_program(UV_EXE uv
    DOC   "uv (Python script runner; manages build_fade.py's pillow dep)"
    REQUIRED)

find_program(AUDIOCONV64_EXE audioconv64
    PATHS "${N64_BINDIR}"
    DOC   "libdragon audioconv64 (MP3/WAV -> wav64 encoder)"
    REQUIRED)

set(_SPLASH_BUILD_DIR     "${CMAKE_BINARY_DIR}/splash")
set(_SPLASH_STAGE_DIR     "${N64_DFS_ROOT}/splash")
file(MAKE_DIRECTORY "${_SPLASH_BUILD_DIR}")
file(MAKE_DIRECTORY "${_SPLASH_STAGE_DIR}")

# -----------------------------------------------------------------------------
# Sweep sprite: assets/sega_sweep.png -> sega_sweep.sprite (CI8).
# --dither NONE keeps the 20 source RGB triples exact in the quantised TLUT
# so the runtime's per-stripe re-index can deterministically locate the
# white slot.
# -----------------------------------------------------------------------------
set(_SPLASH_SWEEP_SRC          "${CMAKE_CURRENT_SOURCE_DIR}/../assets/splash/sega_sweep.png")
set(_SPLASH_SWEEP_BUILT_SPRITE "${_SPLASH_BUILD_DIR}/sega_sweep.sprite")
set(SPLASH_STAGED_SWEEP_SPRITE "${_SPLASH_STAGE_DIR}/sega_sweep.sprite")

add_custom_command(
    OUTPUT  "${_SPLASH_SWEEP_BUILT_SPRITE}"
    COMMAND "${MKSPRITE_EXE}"
            --format CI8
            --dither NONE
            -o "${_SPLASH_BUILD_DIR}"
            "${_SPLASH_SWEEP_SRC}"
    DEPENDS "${_SPLASH_SWEEP_SRC}"
    COMMENT "[BAKE] sega_sweep.sprite"
    VERBATIM)

add_custom_command(
    OUTPUT  "${SPLASH_STAGED_SWEEP_SPRITE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_SPLASH_SWEEP_BUILT_SPRITE}" "${SPLASH_STAGED_SWEEP_SPRITE}"
    DEPENDS "${_SPLASH_SWEEP_BUILT_SPRITE}"
    COMMENT "[BAKE] stage sega_sweep.sprite"
    VERBATIM)

# -----------------------------------------------------------------------------
# Fade sprite: assets/sega_fade/*.png -> sega_fade.sprite (CI4) + palette header.
# build_fade.py canonicalises the four fade frames into a single 7-level
# indexed PNG and emits the matching palette table. mksprite then bakes the
# indexed PNG into a CI4 sprite while preserving the level→index order.
# -----------------------------------------------------------------------------
set(_FADE_SRC_DIR             "${CMAKE_CURRENT_SOURCE_DIR}/../assets/splash/sega_fade")
set(_FADE_SCRIPT              "${CMAKE_CURRENT_SOURCE_DIR}/../tools/bake-splash/build_fade.py")
set(_FADE_BUILT_PNG           "${_SPLASH_BUILD_DIR}/sega_fade.png")
set(_FADE_BUILT_HEADER        "${_SPLASH_BUILD_DIR}/sega_fade_palettes.h")
set(_FADE_BUILT_SPRITE        "${_SPLASH_BUILD_DIR}/sega_fade.sprite")
set(SPLASH_STAGED_FADE_SPRITE "${_SPLASH_STAGE_DIR}/sega_fade.sprite")
set(SPLASH_FADE_HEADER_DIR    "${_SPLASH_BUILD_DIR}")

add_custom_command(
    OUTPUT  "${_FADE_BUILT_PNG}" "${_FADE_BUILT_HEADER}"
    COMMAND "${UV_EXE}" run --quiet "${_FADE_SCRIPT}"
            "${_FADE_SRC_DIR}" "${_FADE_BUILT_PNG}" "${_FADE_BUILT_HEADER}"
    DEPENDS "${_FADE_SCRIPT}"
            "${_FADE_SRC_DIR}/1.png"
            "${_FADE_SRC_DIR}/2.png"
            "${_FADE_SRC_DIR}/3.png"
            "${_FADE_SRC_DIR}/4.png"
    COMMENT "[BAKE] sega_fade.png + sega_fade_palettes.h"
    VERBATIM)

add_custom_command(
    OUTPUT  "${_FADE_BUILT_SPRITE}"
    COMMAND "${MKSPRITE_EXE}"
            --format CI4
            --dither NONE
            -o "${_SPLASH_BUILD_DIR}"
            "${_FADE_BUILT_PNG}"
    DEPENDS "${_FADE_BUILT_PNG}"
    COMMENT "[BAKE] sega_fade.sprite"
    VERBATIM)

add_custom_command(
    OUTPUT  "${SPLASH_STAGED_FADE_SPRITE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_FADE_BUILT_SPRITE}" "${SPLASH_STAGED_FADE_SPRITE}"
    DEPENDS "${_FADE_BUILT_SPRITE}"
    COMMENT "[BAKE] stage sega_fade.sprite"
    VERBATIM)

# -----------------------------------------------------------------------------
# Splash audio: assets/splash/sega.mp3 -> sega.wav64 (VADPCM @ 22050 Hz).
# audioconv64 accepts MP3 input directly and emits a wav64 named after the
# input basename, so the bake is a single command + a staging copy.
# -----------------------------------------------------------------------------
set(_SPLASH_AUDIO_SRC   "${CMAKE_CURRENT_SOURCE_DIR}/../assets/splash/sega.mp3")
set(_SPLASH_AUDIO_BUILT "${_SPLASH_BUILD_DIR}/sega.wav64")
set(SPLASH_STAGED_AUDIO "${_SPLASH_STAGE_DIR}/sega.wav64")

add_custom_command(
    OUTPUT  "${_SPLASH_AUDIO_BUILT}"
    COMMAND "${AUDIOCONV64_EXE}"
            --wav-compress 1
            --wav-resample 22050
            -o "${_SPLASH_BUILD_DIR}"
            "${_SPLASH_AUDIO_SRC}"
    DEPENDS "${_SPLASH_AUDIO_SRC}"
    COMMENT "[BAKE] sega.wav64"
    VERBATIM)

add_custom_command(
    OUTPUT  "${SPLASH_STAGED_AUDIO}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_SPLASH_AUDIO_BUILT}" "${SPLASH_STAGED_AUDIO}"
    DEPENDS "${_SPLASH_AUDIO_BUILT}"
    COMMENT "[BAKE] stage sega.wav64"
    VERBATIM)

add_custom_target(bake-splash ALL
    DEPENDS "${SPLASH_STAGED_SWEEP_SPRITE}"
            "${SPLASH_STAGED_FADE_SPRITE}"
            "${SPLASH_STAGED_AUDIO}"
            "${_FADE_BUILT_HEADER}")
