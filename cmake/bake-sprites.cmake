# -----------------------------------------------------------------------------
# bake-sprites — generate the pre-decoded sprite atlas blob + lookup index.
#
# Builds the host bake-sprites tool out-of-tree (via a nested cmake), which
# statically walks the OutRun master CPU ROM's sprite/anim descriptor tables
# to enumerate every (bank, addr, pitch, max_h) tuple the hardware can be
# asked to render. Emits sprite_atlas.bin into the DFS root and
# sprite_atlas_index.c into the build tree (linked into the N64 ELF).
#
# Inputs from caller (set BEFORE include):
#   N64_DFS_ROOT       — DFS staging directory (the same one passed to
#                        n64_create_rom). The module appends /sprites/.
#
# Outputs:
#   BAKE_SPRITES_INDEX_SRC     — generated .c (add to cannonball sources)
#   BAKE_SPRITES_STAGED_BLOB   — staged .bin path (add to EXTRA_DFS_DEPS)
#   target  bake-sprites-host  — internal host-tool build target
# -----------------------------------------------------------------------------

if(NOT DEFINED N64_DFS_ROOT)
    message(FATAL_ERROR "bake-sprites: caller must set N64_DFS_ROOT before include()")
endif()

set(_BAKE_TOOL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../tools/bake-sprites")
set(_BAKE_ROMS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../roms/")
set(_BAKE_STAGE_DIR "${N64_DFS_ROOT}/sprites")
file(MAKE_DIRECTORY "${_BAKE_STAGE_DIR}")

# -----------------------------------------------------------------------------
# Host bake-sprites tool — child cmake invocation (auto-detects host
# compiler instead of inheriting the n64 toolchain). Mirrors the
# audio-payload pattern.
# -----------------------------------------------------------------------------
set(_BAKE_HOST_BLD "${CMAKE_BINARY_DIR}/bake-sprites-host")
set(_BAKE_HOST_BIN "${_BAKE_HOST_BLD}/bake-sprites")

file(GLOB _BAKE_HOST_SRCS
    "${_BAKE_TOOL_SRC}/*.cpp"
    "${_BAKE_TOOL_SRC}/CMakeLists.txt")

add_custom_command(
    OUTPUT  "${_BAKE_HOST_BIN}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_BAKE_HOST_BLD}"
    COMMAND ${CMAKE_COMMAND} -S "${_BAKE_TOOL_SRC}"
                             -B "${_BAKE_HOST_BLD}"
                             -DCMAKE_BUILD_TYPE=Release
                             -G "${CMAKE_GENERATOR}"
    COMMAND ${CMAKE_COMMAND} --build "${_BAKE_HOST_BLD}"
                             --target bake-sprites
    DEPENDS ${_BAKE_HOST_SRCS}
    COMMENT "[BAKE] host bake-sprites"
    VERBATIM)

# -----------------------------------------------------------------------------
# Run the bake. Both outputs come out of a single tool invocation, so they
# share one custom_command.
# -----------------------------------------------------------------------------
set(BAKE_SPRITES_INDEX_SRC          "${CMAKE_BINARY_DIR}/sprite_atlas_index.c")
set(BAKE_SPRITES_BLOB               "${CMAKE_BINARY_DIR}/sprite_atlas.bin")
set(BAKE_SPRITES_STAGED_BLOB        "${_BAKE_STAGE_DIR}/sprite_atlas.bin")
set(BAKE_SPRITES_NATIVE_BLOB        "${CMAKE_BINARY_DIR}/sprites_native.bin")
set(BAKE_SPRITES_STAGED_NATIVE_BLOB "${_BAKE_STAGE_DIR}/sprites_native.bin")

add_custom_command(
    OUTPUT  "${BAKE_SPRITES_INDEX_SRC}"
            "${BAKE_SPRITES_BLOB}"
            "${BAKE_SPRITES_NATIVE_BLOB}"
    COMMAND "${_BAKE_HOST_BIN}"
            --roms          "${_BAKE_ROMS_DIR}"
            --blob          "${BAKE_SPRITES_BLOB}"
            --index         "${BAKE_SPRITES_INDEX_SRC}"
            --sprites-blob  "${BAKE_SPRITES_NATIVE_BLOB}"
    DEPENDS "${_BAKE_HOST_BIN}"
    COMMENT "[BAKE] sprite_atlas.bin + sprite_atlas_index.c + sprites_native.bin"
    VERBATIM)

add_custom_command(
    OUTPUT  "${BAKE_SPRITES_STAGED_BLOB}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BAKE_SPRITES_BLOB}" "${BAKE_SPRITES_STAGED_BLOB}"
    DEPENDS "${BAKE_SPRITES_BLOB}"
    COMMENT "[BAKE] stage sprite_atlas.bin"
    VERBATIM)

add_custom_command(
    OUTPUT  "${BAKE_SPRITES_STAGED_NATIVE_BLOB}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BAKE_SPRITES_NATIVE_BLOB}" "${BAKE_SPRITES_STAGED_NATIVE_BLOB}"
    DEPENDS "${BAKE_SPRITES_NATIVE_BLOB}"
    COMMENT "[BAKE] stage sprites_native.bin"
    VERBATIM)

add_custom_target(bake-sprites ALL
    DEPENDS "${BAKE_SPRITES_INDEX_SRC}"
            "${BAKE_SPRITES_STAGED_BLOB}"
            "${BAKE_SPRITES_STAGED_NATIVE_BLOB}")
