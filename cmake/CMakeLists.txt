# -----------------------------------------------------------------------------
# CannonBall N64 build
#
# Invoke from cmake/:
#   cmake -B ../build-n64 \
#         -DCMAKE_TOOLCHAIN_FILE=$(pwd)/n64-toolchain.cmake \
#         .
#   make -C ../build-n64
#
# Requires N64_INST pointing at a libdragon toolchain install.
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.13)
project(cannonball)

set(main_cpp_base ../src/main)

# -----------------------------------------------------------------------------
# Source sets
# -----------------------------------------------------------------------------

set(src_main
    "${main_cpp_base}/globals.hpp"
    "${main_cpp_base}/main.hpp"
    "${main_cpp_base}/romloader.hpp"
    "${main_cpp_base}/roms.hpp"
    "${main_cpp_base}/trackloader.hpp"
    "${main_cpp_base}/stdint.hpp"
    "${main_cpp_base}/video.hpp"
    "${main_cpp_base}/utils.hpp"
    "${main_cpp_base}/utils_crc32.hpp"

    "${main_cpp_base}/romloader.cpp"
    "${main_cpp_base}/trackloader.cpp"
    "${main_cpp_base}/roms.cpp"
    "${main_cpp_base}/video.cpp"
    "${main_cpp_base}/utils.cpp"
    "${main_cpp_base}/utils_crc32.cpp"
)

set(src_frontend
    "${main_cpp_base}/frontend/config.hpp"
    "${main_cpp_base}/frontend/ttrial.hpp"

    "${main_cpp_base}/frontend/config.cpp"
    "${main_cpp_base}/frontend/ttrial.cpp"
)

set(src_hwvideo
    "${main_cpp_base}/hwvideo/hwroad.hpp"
    "${main_cpp_base}/hwvideo/hwsprites.hpp"
    "${main_cpp_base}/hwvideo/hwtiles.hpp"

    "${main_cpp_base}/hwvideo/hwroad.cpp"
    "${main_cpp_base}/hwvideo/hwsprites.cpp"
    "${main_cpp_base}/hwvideo/hwtiles.cpp"
)

set(src_hwaudio
    "${main_cpp_base}/hwaudio/segapcm.hpp"
    "${main_cpp_base}/hwaudio/soundchip.hpp"
    "${main_cpp_base}/hwaudio/ym2151.hpp"

    "${main_cpp_base}/hwaudio/segapcm.cpp"
    "${main_cpp_base}/hwaudio/soundchip.cpp"
    "${main_cpp_base}/hwaudio/ym2151.cpp"
)

set(src_engine
    "${main_cpp_base}/engine/oaddresses.hpp"
    "${main_cpp_base}/engine/oanimseq.hpp"
    "${main_cpp_base}/engine/oanimsprite.hpp"
    "${main_cpp_base}/engine/oattractai.hpp"
    "${main_cpp_base}/engine/obonus.hpp"
    "${main_cpp_base}/engine/ocrash.hpp"
    "${main_cpp_base}/engine/oentry.hpp"
    "${main_cpp_base}/engine/oferrari.hpp"
    "${main_cpp_base}/engine/ohiscore.hpp"
    "${main_cpp_base}/engine/ohud.hpp"
    "${main_cpp_base}/engine/oinitengine.hpp"
    "${main_cpp_base}/engine/oinputs.hpp"
    "${main_cpp_base}/engine/olevelobjs.hpp"
    "${main_cpp_base}/engine/ologo.hpp"
    "${main_cpp_base}/engine/omap.hpp"
    "${main_cpp_base}/engine/omusic.hpp"
    "${main_cpp_base}/engine/ooutputs.hpp"
    "${main_cpp_base}/engine/opalette.hpp"
    "${main_cpp_base}/engine/oroad.hpp"
    "${main_cpp_base}/engine/osmoke.hpp"
    "${main_cpp_base}/engine/osprite.hpp"
    "${main_cpp_base}/engine/osprites.hpp"
    "${main_cpp_base}/engine/ostats.hpp"
    "${main_cpp_base}/engine/otiles.hpp"
    "${main_cpp_base}/engine/otraffic.hpp"
    "${main_cpp_base}/engine/outils.hpp"
    "${main_cpp_base}/engine/outrun.hpp"

    "${main_cpp_base}/engine/oanimseq.cpp"
    "${main_cpp_base}/engine/oattractai.cpp"
    "${main_cpp_base}/engine/obonus.cpp"
    "${main_cpp_base}/engine/ocrash.cpp"
    "${main_cpp_base}/engine/oferrari.cpp"
    "${main_cpp_base}/engine/ohiscore.cpp"
    "${main_cpp_base}/engine/ohud.cpp"
    "${main_cpp_base}/engine/oinitengine.cpp"
    "${main_cpp_base}/engine/oinputs.cpp"
    "${main_cpp_base}/engine/olevelobjs.cpp"
    "${main_cpp_base}/engine/ologo.cpp"
    "${main_cpp_base}/engine/omap.cpp"
    "${main_cpp_base}/engine/omusic.cpp"
    "${main_cpp_base}/engine/ooutputs.cpp"
    "${main_cpp_base}/engine/opalette.cpp"
    "${main_cpp_base}/engine/oroad.cpp"
    "${main_cpp_base}/engine/osmoke.cpp"
    "${main_cpp_base}/engine/osprite.cpp"
    "${main_cpp_base}/engine/osprites.cpp"
    "${main_cpp_base}/engine/ostats.cpp"
    "${main_cpp_base}/engine/otiles.cpp"
    "${main_cpp_base}/engine/otraffic.cpp"
    "${main_cpp_base}/engine/outils.cpp"
    "${main_cpp_base}/engine/outrun.cpp"
)

set(src_engine_data
    "${main_cpp_base}/engine/data/ozoom_lookup.hpp"
    "${main_cpp_base}/engine/data/sprite_pals.hpp"
)

set(src_engine_audio
    "${main_cpp_base}/engine/audio/commands.hpp"
    "${main_cpp_base}/engine/audio/osound.hpp"
    "${main_cpp_base}/engine/audio/osoundint.hpp"
    "${main_cpp_base}/engine/audio/osoundadr.hpp"

    "${main_cpp_base}/engine/audio/osound.cpp"
    "${main_cpp_base}/engine/audio/osoundint.cpp"
)

set(src_n64
    "${main_cpp_base}/n64/platform.hpp"
    "${main_cpp_base}/n64/audio.hpp"
    "${main_cpp_base}/n64/input.hpp"
    "${main_cpp_base}/n64/timer.hpp"
    "${main_cpp_base}/n64/rendersurface.hpp"
    "${main_cpp_base}/n64/save.hpp"
    "${main_cpp_base}/n64/splash.hpp"
    "${main_cpp_base}/n64/boot_menu.hpp"
    "${main_cpp_base}/n64/hwroad_rsp.hpp"
    "${main_cpp_base}/n64/hwroad_rdp.hpp"
    "${main_cpp_base}/n64/hwroad_rdp_internal.hpp"
    "${main_cpp_base}/n64/hwroad_rdp_rsp.hpp"

    "${main_cpp_base}/n64/n64main.cpp"
    "${main_cpp_base}/n64/audio.cpp"
    "${main_cpp_base}/n64/input.cpp"
    "${main_cpp_base}/n64/timer.cpp"
    "${main_cpp_base}/n64/rendersurface.cpp"
    "${main_cpp_base}/n64/save.cpp"
    "${main_cpp_base}/n64/splash.cpp"
    "${main_cpp_base}/n64/boot_menu.cpp"
    "${main_cpp_base}/n64/hwroad_rsp.cpp"
    "${main_cpp_base}/n64/hwroad_rdp.cpp"
    "${main_cpp_base}/n64/hwroad_rdp_rsp.cpp"
)

set(SRCS
    ${src_main}
    ${src_frontend}
    ${src_hwvideo}
    ${src_hwaudio}
    ${src_engine}
    ${src_engine_data}
    ${src_engine_audio}
    ${src_n64}
)

# -----------------------------------------------------------------------------
# Target
# -----------------------------------------------------------------------------

add_executable(cannonball ${SRCS})
set_target_properties(cannonball PROPERTIES SUFFIX ".elf")

include_directories(${main_cpp_base})
n64_setup_linking(cannonball)

# RSP overlay: per-scanline hwroad foreground rasterizer. Source basename
# matches the rsp_ucode_t symbol used by DEFINE_RSP_UCODE in hwroad_rsp.cpp.
n64_add_rsp_ucode(cannonball
    "${main_cpp_base}/n64/rsp_hwroad.S")

# RSP overlay: per-scanline mask packer that backs the hwroad_rdp build
# phase. Source basename matches DEFINE_RSP_UCODE(rsp_hwroad_rdp) in
# hwroad_rdp_rsp.cpp.
n64_add_rsp_ucode(cannonball
    "${main_cpp_base}/n64/rsp_hwroad_rdp.S")

# -----------------------------------------------------------------------------
# DFS payload — ROMs + binary tile data baked into cannonball.z64
# -----------------------------------------------------------------------------

set(N64_DFS_ROOT "${CMAKE_CURRENT_BINARY_DIR}/dfs")
add_custom_command(TARGET cannonball PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${N64_DFS_ROOT}/roms"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${N64_DFS_ROOT}/res"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${N64_DFS_ROOT}/audio"
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../roms")
    file(GLOB CANNONBALL_ROMS "${CMAKE_CURRENT_SOURCE_DIR}/../roms/*")
    foreach(rom ${CANNONBALL_ROMS})
        get_filename_component(rom_name ${rom} NAME)
        configure_file(${rom} "${N64_DFS_ROOT}/roms/${rom_name}" COPYONLY)
    endforeach()
endif()
configure_file(../res/tilemap.bin   "${N64_DFS_ROOT}/res/tilemap.bin"   COPYONLY)
configure_file(../res/tilepatch.bin "${N64_DFS_ROOT}/res/tilepatch.bin" COPYONLY)

# wav64-compressed audio (music + SFX). Generated from the OutRun Z80 sound
# program at build time and staged into ${N64_DFS_ROOT}/audio — see
# cmake/audio-payload.cmake.
include(audio-payload.cmake)
add_dependencies(cannonball audio-payload)

# Native-byteswapped sprite + tile ROM blobs (DFS payloads consumed by the
# CPU EOR-walk spillover path in hwsprites/hwtiles). The pre-decoded atlas
# blob + lookup index were removed in favour of lazy decode-on-miss; the
# runtime atlas hash cache amortizes the EOR walk across frames. See
# cmake/bake-sprites.cmake.
include(bake-sprites.cmake)
add_dependencies(cannonball bake-sprites)

# Pre-boot SEGA splash: CI8 sweep sprite baked from assets/splash/sweep.png +
# IA4 silhouette sprite baked from assets/splash/fade.png. The fade tint comes
# entirely from a per-frame PRIMITIVE colour and a combiner LERP in
# src/main/n64/splash.cpp, so no generated palette header is needed.
# See cmake/bake-splash.cmake.
include(bake-splash.cmake)
add_dependencies(cannonball bake-splash)

# Pre-engine title + options menu sprites: PNG → .sprite (CI4 for text labels
# and pills so the runtime can re-tint them via the rdpq combiner; RGBA16 for
# the *_bg full-colour chrome). Staged into rom:/menu/ and consumed by
# src/main/n64/boot_menu.cpp. See cmake/bake-menu.cmake.
include(bake-menu.cmake)
add_dependencies(cannonball bake-menu)

n64_create_rom(cannonball
    TITLE "CannonBall"
    DFS_ROOT "${N64_DFS_ROOT}"
    EXTRA_DFS_DEPS ${AUDIO_PAYLOAD_STAGED_FILES}
                   "${BAKE_SPRITES_STAGED_NATIVE_BLOB}"
                   "${BAKE_TILES_STAGED_NATIVE_BLOB}"
                   "${SPLASH_STAGED_SWEEP_SPRITE}"
                   "${SPLASH_STAGED_FADE_SPRITE}"
                   "${SPLASH_STAGED_AUDIO}"
                   ${MENU_STAGED_SPRITES}
)
