# -----------------------------------------------------------------------------
# CannonBall CMake setup
#
# For ports, please try not to edit this file directly.
# Everything should go into a win64.cmake equivalent.
# -----------------------------------------------------------------------------

cmake_minimum_required (VERSION 3.3.2) # Might compile lower, but untested
project (cannonball)

# -----------------------------------------------------------------------------
# Directory Setup
# -----------------------------------------------------------------------------

# CMake default file (override with -DTARGET=file)
set(DCMAKE win64-opengl.cmake)

# Source location
set(main_cpp_base ../src/main)

# -----------------------------------------------------------------------------
# Platform Specific Setup
# -----------------------------------------------------------------------------

# Include relevant CMAKE file (default, mingw, pandora etc.)
if(TARGET)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${TARGET}")
        message("CMake files for ${TARGET} found")
        include(${TARGET})
    else()
        message("CMake files for ${TARGET} not found, using ${DCMAKE}")
        include(${DCMAKE})
    endif()
else()
    message("TARGET not passed, using ${DCMAKE}")
    include(${DCMAKE})
endif(TARGET)

# -----------------------------------------------------------------------------
# Project Source Files
# -----------------------------------------------------------------------------
    
set(src_main
    "${main_cpp_base}/globals.hpp"
    "${main_cpp_base}/romloader.hpp"
    "${main_cpp_base}/roms.hpp"
    "${main_cpp_base}/trackloader.hpp"
    "${main_cpp_base}/stdint.hpp"
    "${main_cpp_base}/main.hpp"
    "${main_cpp_base}/video.hpp"
    "${main_cpp_base}/utils.hpp"
    "${main_cpp_base}/windirent.h"

    "${main_cpp_base}/romloader.cpp"
    "${main_cpp_base}/trackloader.cpp"
    "${main_cpp_base}/roms.cpp"
    "${main_cpp_base}/video.cpp"
    "${main_cpp_base}/utils.cpp"
    )

# main.cpp owns the SDL event loop and entry point. Platform-specific entry
# points (eg. src/main/n64/n64main.cpp) replace this on non-SDL builds, which
# clear src_main_entry in their target file.
set(src_main_entry
    "${main_cpp_base}/main.cpp"
    )

set(src_frontend
    "${main_cpp_base}/frontend/config.hpp"
    "${main_cpp_base}/frontend/ttrial.hpp"

    "${main_cpp_base}/frontend/config.cpp"
    "${main_cpp_base}/frontend/ttrial.cpp"
    )

# Menu + cabinet diagnostics. The N64 target file clears src_frontend_menu so
# Phase 1 boots straight into attract mode (matches plan §Frontend).
set(src_frontend_menu
    "${main_cpp_base}/frontend/cabdiag.hpp"
    "${main_cpp_base}/frontend/menu.hpp"
    "${main_cpp_base}/frontend/menulabels.hpp"

    "${main_cpp_base}/frontend/cabdiag.cpp"
    "${main_cpp_base}/frontend/menu.cpp"
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

set(src_sdl
	"${main_cpp_base}/sdl2/audio.hpp"
	"${main_cpp_base}/sdl2/timer.hpp"
	"${main_cpp_base}/sdl2/input.hpp"
	"${main_cpp_base}/sdl2/renderbase.hpp"

    "${main_cpp_base}/sdl2/audio.cpp"
	"${main_cpp_base}/sdl2/timer.cpp"
	"${main_cpp_base}/sdl2/input.cpp"
	"${main_cpp_base}/sdl2/renderbase.cpp"
    )
   
if(OPENGL)
	set(src_render
	    "${main_cpp_base}/sdl2/rendergl.hpp"
	    "${main_cpp_base}/sdl2/rendergl.cpp"
	)
elseif(OPENGLES)
	set(src_render
	    "${main_cpp_base}/sdl2/rendergles.hpp"
	    "${main_cpp_base}/sdl2/rendergles.cpp"
	)   
else()
	set(src_render
	    "${main_cpp_base}/sdl2/rendersurface.hpp"
	    "${main_cpp_base}/sdl2/rendersurface.cpp"
	)
endif()

set(src_directx
    "${main_cpp_base}/directx/ffeedback.hpp"
    "${main_cpp_base}/directx/ffeedback.cpp"
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

# Per-platform source exclusions. The target files (linux.cmake/win64*.cmake)
# only need to ADD to the source set; carve-outs for non-host platforms (N64)
# live here so they survive the unconditional set() blocks above.
if(N64)
    # SDL backend, host renderers, and the SDL entry point are replaced by
    # src_n64 (n64main.cpp + n64/* backend). Menu + cabinet diagnostics are
    # deferred to Phase 5 / dropped — n64main.cpp routes STATE_INIT_MENU into
    # STATE_INIT_GAME so Menu is never instantiated.
    # ffeedback.cpp stays: its #ifndef WIN32 branch provides empty stubs that
    # ooutputs.cpp links against unconditionally.
    set(src_sdl           "")
    set(src_render        "")
    set(src_main_entry    "")
    set(src_frontend_menu "")
endif()

set(SRCS
    ${src_main}
    ${src_main_entry}
    ${src_frontend}
    ${src_frontend_menu}
    ${src_hwvideo}
    ${src_hwaudio}
    ${src_sdl}
    ${src_render}
    ${src_directx}
    ${src_engine}
    ${src_engine_data}
    ${src_engine_audio}
    ${src_n64}
)

# Group files into relevant filters for Visual Studio build
SOURCE_GROUP(src FILES ${src_main})
SOURCE_GROUP(src\\frontend FILES ${src_frontend})
SOURCE_GROUP(src\\frontend FILES ${src_frontend_menu})
SOURCE_GROUP(src\\hwvideo FILES ${src_hwvideo})
SOURCE_GROUP(src\\hwaudio FILES ${src_hwaudio})
SOURCE_GROUP(src\\sdl FILES ${src_sdl})
SOURCE_GROUP(src\\sdl FILES ${src_render})
SOURCE_GROUP(src\\directx FILES ${src_directx})
SOURCE_GROUP(src\\engine FILES ${src_engine})
SOURCE_GROUP(src\\engine\\data FILES ${src_engine_data})
SOURCE_GROUP(src\\engine\\audio FILES ${src_engine_audio})
SOURCE_GROUP(src\\n64 FILES ${src_n64})

# Add Icon For Windows Builds
if(WIN32)
  set(SRCS ${SRCS} ../res/cannonball.rc)
endif(WIN32)

if(N64)
    # Match libdragon convention: ELF target produces cannonball.elf
    add_executable(cannonball ${SRCS})
    set_target_properties(cannonball PROPERTIES SUFFIX ".elf")
else()
    add_executable(cannonball ${SRCS})
endif()

# -----------------------------------------------------------------------------
# Compiler Definitions
# -----------------------------------------------------------------------------

if(OPENGL)
    add_definitions(-DWITH_OPENGL)
endif()

if(OPENGLES)
    add_definitions(-DWITH_OPENGLES)
endif()

# Feature flags driven by host capability. Non-SDL platforms (eg. N64) clear
# these in their target file by skipping this block.
if(NOT N64)
    add_definitions(-DWITH_SDL -DWITH_BOOST -DWITH_XML_CONFIG -DWITH_DIRENT)
endif()

# -----------------------------------------------------------------------------
# Library Setup
# -----------------------------------------------------------------------------

if(N64)
    # libdragon ships its own libc/libstdc++/libdragon. No host find_package
    # calls — those would search the host toolchain. Link order matches n64.mk
    # via n64_setup_linking() from the toolchain file.
    include_directories(
        ${main_cpp_base}
    )
    n64_setup_linking(cannonball)

    # Stage DFS payload (ROMs + binary tile data) under a build-local tree, then
    # bake it into cannonball.z64.
    set(N64_DFS_ROOT "${CMAKE_CURRENT_BINARY_DIR}/dfs")
    add_custom_command(TARGET cannonball PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${N64_DFS_ROOT}/roms"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${N64_DFS_ROOT}/res"
    )
    # ROM files come from ../roms/ when present (user must drop the Rev B set).
    # Tile binaries ship in res/.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../roms")
        file(GLOB CANNONBALL_ROMS "${CMAKE_CURRENT_SOURCE_DIR}/../roms/*")
        foreach(rom ${CANNONBALL_ROMS})
            get_filename_component(rom_name ${rom} NAME)
            configure_file(${rom} "${N64_DFS_ROOT}/roms/${rom_name}" COPYONLY)
        endforeach()
    endif()
    configure_file(../res/tilemap.bin   "${N64_DFS_ROOT}/res/tilemap.bin"   COPYONLY)
    configure_file(../res/tilepatch.bin "${N64_DFS_ROOT}/res/tilepatch.bin" COPYONLY)

    n64_create_rom(cannonball
        TITLE "CannonBall"
        DFS_ROOT "${N64_DFS_ROOT}"
    )
else()
    set(BOOST_INCLUDEDIR ${boost_dir})
    find_package(Boost REQUIRED)

    set(SDL2_DIR ${sdl2_dir})
    find_package(SDL2 REQUIRED)
    message("SDL2 Include  : ${SDL2_INCLUDE_DIRS}")
    message("SDL2 Libraries: ${SDL2_LIBRARIES}")

    # Include
    include_directories(
        ${main_cpp_base}
        ${BOOST_INCLUDEDIR}
        ${SDL2_INCLUDE_DIRS}
    )

    link_directories(
        ${platform_link_dirs}
    )

    target_link_libraries(cannonball
        ${SDL2_LIBRARIES}
        ${platform_link_libs}
    )

    # -------------------------------------------------------------------------
    # Copy useful files to build destination
    # -------------------------------------------------------------------------

    configure_file(../res/config.xml ./config.xml
        COPYONLY
    )

    configure_file(../res/tilemap.bin ./res/tilemap.bin
        COPYONLY
    )
    configure_file(../res/tilepatch.bin ./res/tilepatch.bin
        COPYONLY
    )
    configure_file(../res/gamecontrollerdb.txt ./res/gamecontrollerdb.txt
        COPYONLY
    )
endif()
