# -----------------------------------------------------------------------------
# bake-sprites — generate the native-byteswapped sprite + tile ROM blobs that
# the runtime CPU EOR-walk spillover DMAs at decode-on-miss time.
#
# Builds the host bake-sprites tool out-of-tree (via a nested cmake). The
# static atlas pre-decode pipeline (sprite_atlas.bin + sprite_atlas_index.c)
# was removed: the runtime atlas hash cache decodes on first miss and
# amortizes the ~500us EOR walk across subsequent frames.
#
# Inputs from caller (set BEFORE include):
#   N64_DFS_ROOT       — DFS staging directory (the same one passed to
#                        n64_create_rom). The module appends /sprites/ and
#                        /tiles/.
#
# Outputs:
#   BAKE_SPRITES_STAGED_NATIVE_BLOB — staged sprites_native.bin
#   BAKE_TILES_STAGED_NATIVE_BLOB   — staged tiles_native.bin
#   target  bake-sprites-host       — internal host-tool build target
# -----------------------------------------------------------------------------

if(NOT DEFINED N64_DFS_ROOT)
    message(FATAL_ERROR "bake-sprites: caller must set N64_DFS_ROOT before include()")
endif()

set(_BAKE_TOOL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../tools/bake-sprites")
set(_BAKE_ROMS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../roms/")
set(_BAKE_STAGE_DIR       "${N64_DFS_ROOT}/sprites")
set(_BAKE_TILES_STAGE_DIR "${N64_DFS_ROOT}/tiles")
file(MAKE_DIRECTORY "${_BAKE_STAGE_DIR}")
file(MAKE_DIRECTORY "${_BAKE_TILES_STAGE_DIR}")

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
# Run the bake. Single tool invocation emits both native blobs.
# -----------------------------------------------------------------------------
set(BAKE_SPRITES_NATIVE_BLOB        "${CMAKE_BINARY_DIR}/sprites_native.bin")
set(BAKE_SPRITES_STAGED_NATIVE_BLOB "${_BAKE_STAGE_DIR}/sprites_native.bin")
set(BAKE_TILES_NATIVE_BLOB          "${CMAKE_BINARY_DIR}/tiles_native.bin")
set(BAKE_TILES_STAGED_NATIVE_BLOB   "${_BAKE_TILES_STAGE_DIR}/tiles_native.bin")

add_custom_command(
    OUTPUT  "${BAKE_SPRITES_NATIVE_BLOB}"
            "${BAKE_TILES_NATIVE_BLOB}"
    COMMAND "${_BAKE_HOST_BIN}"
            --roms          "${_BAKE_ROMS_DIR}"
            --sprites-blob  "${BAKE_SPRITES_NATIVE_BLOB}"
            --tiles-blob    "${BAKE_TILES_NATIVE_BLOB}"
    DEPENDS "${_BAKE_HOST_BIN}"
    COMMENT "[BAKE] sprites_native.bin + tiles_native.bin"
    VERBATIM)

add_custom_command(
    OUTPUT  "${BAKE_SPRITES_STAGED_NATIVE_BLOB}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BAKE_SPRITES_NATIVE_BLOB}" "${BAKE_SPRITES_STAGED_NATIVE_BLOB}"
    DEPENDS "${BAKE_SPRITES_NATIVE_BLOB}"
    COMMENT "[BAKE] stage sprites_native.bin"
    VERBATIM)

add_custom_command(
    OUTPUT  "${BAKE_TILES_STAGED_NATIVE_BLOB}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BAKE_TILES_NATIVE_BLOB}" "${BAKE_TILES_STAGED_NATIVE_BLOB}"
    DEPENDS "${BAKE_TILES_NATIVE_BLOB}"
    COMMENT "[BAKE] stage tiles_native.bin"
    VERBATIM)

add_custom_target(bake-sprites ALL
    DEPENDS "${BAKE_SPRITES_STAGED_NATIVE_BLOB}"
            "${BAKE_TILES_STAGED_NATIVE_BLOB}")
