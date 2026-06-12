# -----------------------------------------------------------------------------
# bake-menu — convert assets/menu/*.png into .sprite files for the pre-engine
# title + options menus.
#
# Format selection:
#   *_bg.png   → RGBA16 (title_bg / options_bg keep their full-color art)
#   everything else → CI4 (text labels, pills, cursor — single-colour glyphs
#       so the runtime can re-tint them via rdpq_set_prim_color + the
#       combiner pipeline, no per-state palette swap needed)
#
# Inputs from caller (set BEFORE include):
#   N64_DFS_ROOT — DFS staging directory (same one passed to n64_create_rom).
#                  The module appends /menu/.
#
# Outputs:
#   MENU_STAGED_SPRITES — list of staged .sprite paths; pass via
#                         n64_create_rom EXTRA_DFS_DEPS.
#   target  bake-menu   — ALL target producing the staged files.
# -----------------------------------------------------------------------------

if(NOT DEFINED N64_DFS_ROOT)
    message(FATAL_ERROR "bake-menu: caller must set N64_DFS_ROOT before include()")
endif()

find_program(MKSPRITE_EXE mksprite
    PATHS "${N64_BINDIR}"
    DOC   "libdragon mksprite (PNG -> .sprite converter)"
    REQUIRED)

set(_MENU_SRC_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/../assets/menu")
set(_MENU_BUILD_DIR  "${CMAKE_BINARY_DIR}/menu")
set(_MENU_STAGE_DIR  "${N64_DFS_ROOT}/menu")
file(MAKE_DIRECTORY "${_MENU_BUILD_DIR}")
file(MAKE_DIRECTORY "${_MENU_STAGE_DIR}")

# CONFIGURE_DEPENDS so adding / removing a PNG re-runs cmake automatically
# at next build instead of silently using a stale glob (which previously
# crashed boot with "rom:/menu/off.sprite: No such file or directory").
file(GLOB _MENU_PNGS CONFIGURE_DEPENDS "${_MENU_SRC_DIR}/*.png")
set(MENU_STAGED_SPRITES)

foreach(png ${_MENU_PNGS})
    get_filename_component(stem ${png} NAME_WE)
    set(_built  "${_MENU_BUILD_DIR}/${stem}.sprite")
    set(_staged "${_MENU_STAGE_DIR}/${stem}.sprite")

    if(stem MATCHES "_bg$")
        set(_fmt "RGBA16")
    else()
        set(_fmt "CI4")
    endif()

    add_custom_command(
        OUTPUT  "${_built}"
        COMMAND "${MKSPRITE_EXE}"
                --format "${_fmt}"
                --dither NONE
                -o "${_MENU_BUILD_DIR}"
                "${png}"
        DEPENDS "${png}"
        COMMENT "[BAKE] ${stem}.sprite (${_fmt})"
        VERBATIM)

    add_custom_command(
        OUTPUT  "${_staged}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_built}" "${_staged}"
        DEPENDS "${_built}"
        COMMENT "[BAKE] stage ${stem}.sprite"
        VERBATIM)

    list(APPEND MENU_STAGED_SPRITES "${_staged}")
endforeach()

add_custom_target(bake-menu ALL DEPENDS ${MENU_STAGED_SPRITES})
