# -----------------------------------------------------------------------------
# bake-splash — convert assets/splash/{sweep,fade}.png and sega.mp3 into the
# sprites + audio the pre-boot intro animation needs.
#
#   sweep.sprite — CI8 baked from assets/splash/sweep.png via libdragon
#                  mksprite. Carries the 18-hue rainbow-indexed wordmark
#                  (transparent background); the splash runtime re-indexes
#                  its pixels at boot so each x-stripe gets a unique slot.
#
#   fade.sprite  — IA4 baked from assets/splash/fade.png. Anti-aliased
#                  SEGA silhouette where dark intensities mark the logo
#                  body and lighter intensities mark the AA edges. The
#                  splash runtime sets a per-frame PRIMITIVE colour and
#                  uses the colour combiner LERP(PRIM, ONE, TEX0_I) to
#                  tint the body and feather the edges into white.
#
#   sega.wav64   — VADPCM jingle resampled to 22050 Hz, played at the
#                  fade-phase boundary.
#
#   disclaimer.sprite — I8 baked from assets/splash/disclaimer.png. Full
#                  320x240 grayscale displayed for 10 s (skippable with
#                  START) before the SEGA splash, then faded to black via
#                  a per-frame PRIMITIVE colour scaling its luma.
#
# Inputs from caller (set BEFORE include):
#   N64_DFS_ROOT  — DFS staging directory (same one passed to n64_create_rom).
#                   The module appends /splash/.
#
# Outputs:
#   SPLASH_STAGED_SWEEP_SPRITE       — staged DFS path; pass via n64_create_rom EXTRA_DFS_DEPS
#   SPLASH_STAGED_FADE_SPRITE        — staged DFS path; pass via n64_create_rom EXTRA_DFS_DEPS
#   SPLASH_STAGED_DISCLAIMER_SPRITE  — staged DFS path; pass via n64_create_rom EXTRA_DFS_DEPS
#   SPLASH_STAGED_AUDIO              — staged DFS path; pass via n64_create_rom EXTRA_DFS_DEPS
#   target  bake-splash              — ALL target producing the staged files
# -----------------------------------------------------------------------------

if(NOT DEFINED N64_DFS_ROOT)
    message(FATAL_ERROR "bake-splash: caller must set N64_DFS_ROOT before include()")
endif()

find_program(MKSPRITE_EXE mksprite
    PATHS "${N64_BINDIR}"
    DOC   "libdragon mksprite (PNG -> .sprite converter)"
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
# Sweep sprite: assets/splash/sweep.png -> sweep.sprite (CI8).
# --dither NONE keeps the 18 source rainbow hues exact in the quantised
# TLUT so the runtime's per-stripe re-index can deterministically locate
# the transparent bg slot (the only entry with alpha=0).
# -----------------------------------------------------------------------------
set(_SPLASH_SWEEP_SRC          "${CMAKE_CURRENT_SOURCE_DIR}/../assets/splash/sweep.png")
set(_SPLASH_SWEEP_BUILT_SPRITE "${_SPLASH_BUILD_DIR}/sweep.sprite")
set(SPLASH_STAGED_SWEEP_SPRITE "${_SPLASH_STAGE_DIR}/sweep.sprite")

add_custom_command(
    OUTPUT  "${_SPLASH_SWEEP_BUILT_SPRITE}"
    COMMAND "${MKSPRITE_EXE}"
            --format CI8
            --dither NONE
            -o "${_SPLASH_BUILD_DIR}"
            "${_SPLASH_SWEEP_SRC}"
    DEPENDS "${_SPLASH_SWEEP_SRC}"
    COMMENT "[BAKE] sweep.sprite"
    VERBATIM)

add_custom_command(
    OUTPUT  "${SPLASH_STAGED_SWEEP_SPRITE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_SPLASH_SWEEP_BUILT_SPRITE}" "${SPLASH_STAGED_SWEEP_SPRITE}"
    DEPENDS "${_SPLASH_SWEEP_BUILT_SPRITE}"
    COMMENT "[BAKE] stage sweep.sprite"
    VERBATIM)

# -----------------------------------------------------------------------------
# Fade sprite: assets/splash/fade.png -> fade.sprite (IA4).
# The source PNG is an anti-aliased silhouette of the SEGA wordmark over a
# transparent background; mksprite converts it straight to IA4 (3-bit
# intensity + 1-bit alpha). The runtime drives all colour from a per-frame
# PRIMITIVE colour + combiner LERP — no palette is needed.
# -----------------------------------------------------------------------------
set(_SPLASH_FADE_SRC          "${CMAKE_CURRENT_SOURCE_DIR}/../assets/splash/fade.png")
set(_SPLASH_FADE_BUILT_SPRITE "${_SPLASH_BUILD_DIR}/fade.sprite")
set(SPLASH_STAGED_FADE_SPRITE "${_SPLASH_STAGE_DIR}/fade.sprite")

add_custom_command(
    OUTPUT  "${_SPLASH_FADE_BUILT_SPRITE}"
    COMMAND "${MKSPRITE_EXE}"
            --format IA4
            --dither NONE
            -o "${_SPLASH_BUILD_DIR}"
            "${_SPLASH_FADE_SRC}"
    DEPENDS "${_SPLASH_FADE_SRC}"
    COMMENT "[BAKE] fade.sprite"
    VERBATIM)

add_custom_command(
    OUTPUT  "${SPLASH_STAGED_FADE_SPRITE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_SPLASH_FADE_BUILT_SPRITE}" "${SPLASH_STAGED_FADE_SPRITE}"
    DEPENDS "${_SPLASH_FADE_BUILT_SPRITE}"
    COMMENT "[BAKE] stage fade.sprite"
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

# -----------------------------------------------------------------------------
# Disclaimer sprite: assets/splash/disclaimer.png -> disclaimer.sprite (I8).
# Full-screen 320x240 grayscale. Splash runtime drives all colour from the
# combiner: result.rgb = TEX0 * PRIM, so a per-frame PRIM that LERPs from
# white→black scales the I8 luma cleanly down to black.
# -----------------------------------------------------------------------------
set(_SPLASH_DISCLAIMER_SRC          "${CMAKE_CURRENT_SOURCE_DIR}/../assets/splash/disclaimer.png")
set(_SPLASH_DISCLAIMER_BUILT_SPRITE "${_SPLASH_BUILD_DIR}/disclaimer.sprite")
set(SPLASH_STAGED_DISCLAIMER_SPRITE "${_SPLASH_STAGE_DIR}/disclaimer.sprite")

add_custom_command(
    OUTPUT  "${_SPLASH_DISCLAIMER_BUILT_SPRITE}"
    COMMAND "${MKSPRITE_EXE}"
            --format I8
            --dither NONE
            -o "${_SPLASH_BUILD_DIR}"
            "${_SPLASH_DISCLAIMER_SRC}"
    DEPENDS "${_SPLASH_DISCLAIMER_SRC}"
    COMMENT "[BAKE] disclaimer.sprite"
    VERBATIM)

add_custom_command(
    OUTPUT  "${SPLASH_STAGED_DISCLAIMER_SPRITE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_SPLASH_DISCLAIMER_BUILT_SPRITE}" "${SPLASH_STAGED_DISCLAIMER_SPRITE}"
    DEPENDS "${_SPLASH_DISCLAIMER_BUILT_SPRITE}"
    COMMENT "[BAKE] stage disclaimer.sprite"
    VERBATIM)

add_custom_target(bake-splash ALL
    DEPENDS "${SPLASH_STAGED_SWEEP_SPRITE}"
            "${SPLASH_STAGED_FADE_SPRITE}"
            "${SPLASH_STAGED_DISCLAIMER_SPRITE}"
            "${SPLASH_STAGED_AUDIO}")
