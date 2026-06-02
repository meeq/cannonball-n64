# -----------------------------------------------------------------------------
# CannonBall N64 cross-compile toolchain (libdragon)
#
# Modeled on /Users/cdb/Projects/n64/AnotherWorld/cmake/N64LibDragon.cmake.
# Invoke from cmake/:
#   cmake -B ../build-n64 \
#         -DCMAKE_TOOLCHAIN_FILE=$(pwd)/n64-toolchain.cmake \
#         .
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.13)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR "mips")
set(CMAKE_CROSSCOMPILING 1)

# Default to Debug so libdragon's ISViewer logging (debug_init_isviewer/debugf)
# and assert() remain active. CMake's Release/RelWithDebInfo/MinSizeRel inject
# -DNDEBUG which silently disables both.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING
        "Build type (default: Debug for N64; ISViewer needs NDEBUG unset)" FORCE)
endif()

set(N64_INST $ENV{N64_INST})
if(NOT IS_DIRECTORY ${N64_INST})
    message(FATAL_ERROR "Please set N64_INST in your environment")
endif()

set(N64_TARGET     mips64-elf)
set(N64_GCC_PREFIX ${N64_INST}/bin/${N64_TARGET}-)
set(N64_INCLUDE    ${N64_INST}/${N64_TARGET}/include)
set(N64_LIB        ${N64_INST}/${N64_TARGET}/lib)
set(N64_BINDIR     ${N64_INST}/bin)

set(CMAKE_ASM_COMPILER  ${N64_GCC_PREFIX}gcc     CACHE PATH "MIPS assembler")
set(CMAKE_C_COMPILER    ${N64_GCC_PREFIX}gcc     CACHE PATH "MIPS C compiler")
set(CMAKE_CXX_COMPILER  ${N64_GCC_PREFIX}g++     CACHE PATH "MIPS C++ compiler")
set(CMAKE_LINKER        ${N64_GCC_PREFIX}ld      CACHE PATH "MIPS linker")
set(CMAKE_AR            ${N64_GCC_PREFIX}gcc-ar  CACHE PATH "MIPS archiver")
set(CMAKE_AS            ${N64_GCC_PREFIX}as      CACHE PATH "MIPS assembler")
set(CMAKE_STRIP         ${N64_GCC_PREFIX}strip   CACHE PATH "MIPS strip")

set(N64_OBJCOPY    ${N64_GCC_PREFIX}objcopy)
set(N64_OBJDUMP    ${N64_GCC_PREFIX}objdump)
set(N64_SIZE       ${N64_GCC_PREFIX}size)
set(N64_NM         ${N64_GCC_PREFIX}nm)

set(N64_MKDFS         ${N64_BINDIR}/mkdfs)
set(N64_TOOL          ${N64_BINDIR}/n64tool)
set(N64_SYM           ${N64_BINDIR}/n64sym)
set(N64_ELFCOMPRESS   ${N64_BINDIR}/n64elfcompress)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "Shared libs not available")

add_definitions(-DN64)

set(ARCH "-march=vr4300 -mtune=vr4300 -mabi=o64")

set(COMMON_FLAGS_LIST
    "${ARCH}"
    "-falign-functions=32"
    "-ffunction-sections"
    "-fdata-sections"
    "-g"
    "-ffast-math"
    "-Os"
    "-flto"
    "-Wall"
    "-Wno-deprecated-declarations"
    "-Wno-unused-variable"
    "-Wno-unused-but-set-variable"
    "-Wno-unused-function"
    "-Wno-unused-parameter"
    "-Wno-unused-but-set-parameter"
    "-Wno-unused-label"
    "-Wno-unused-local-typedefs"
    "-Wno-unused-const-variable"
    "-fdiagnostics-color=always"
    "-MMD"
)
string(JOIN " " COMMON_FLAGS ${COMMON_FLAGS_LIST})

# Drop exceptions + RTTI for C++ only. Cannonball doesn't use them; libstdc++
# still links fine. Saves both code size and per-call overhead.
set(CMAKE_C_FLAGS   "${COMMON_FLAGS} -std=gnu17 -include ktls.h"
    CACHE STRING "C flags")
set(CMAKE_CXX_FLAGS "${COMMON_FLAGS} -fno-exceptions -fno-rtti -std=gnu++17 -include ktls.h"
    CACHE STRING "C++ flags")

# newlib_overrides first, matches n64.mk include order.
include_directories("${N64_INCLUDE}/newlib_overrides")
include_directories("${N64_INCLUDE}")

# ---------------------------------------------------------------------------
# n64_setup_linking(target_name)
#
# Mirrors n64.mk's link line:
#   <objs> -lc -lstdc++ -ldragon -lm -ldragonsys  <flags>
# g++ appends its own -lc and -lgcc at the end which resolves libdragon's
# libc references.
# ---------------------------------------------------------------------------
function(n64_setup_linking target_name)
    set_target_properties(${target_name} PROPERTIES
        LINK_LIBRARIES "-lc;-lstdc++;-ldragon;-lm;-ldragonsys"
    )
    target_link_options(${target_name} PRIVATE
        "-flto"
        "-Os"
        "-Wl,-g"
        "-Wl,-L${N64_LIB}"
        "-Wl,-Tn64.ld"
        "-Wl,--gc-sections"
        "-Wl,--wrap=__do_global_ctors"
    )
endfunction()

# ---------------------------------------------------------------------------
# n64_create_rom(target_name [TITLE ...] [DFS_ROOT ...])
#
# Produces a .z64 from an ELF target, optionally including a DFS filesystem.
# Mirrors the %.z64 rule in n64.mk:
#   1. Generate symbol table (n64sym)
#   2. Strip the ELF
#   3. Compress the stripped ELF (n64elfcompress)
#   4. Assemble ROM with n64tool
# ---------------------------------------------------------------------------
function(n64_create_rom target_name)
    cmake_parse_arguments(ARG "" "TITLE;DFS_ROOT" "" ${ARGN})

    if(NOT ARG_TITLE)
        set(ARG_TITLE "Made with libdragon")
    endif()

    set(elf_out  "$<TARGET_FILE:${target_name}>")
    set(sym_out  "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.sym")
    set(stripped "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.stripped")
    set(rom_out  "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.z64")

    add_custom_command(
        OUTPUT "${sym_out}"
        COMMAND ${N64_SYM} --all "${elf_out}" "${sym_out}"
        DEPENDS ${target_name}
        COMMENT "[SYM] ${target_name}.sym"
    )

    add_custom_command(
        OUTPUT "${stripped}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${elf_out}" "${stripped}.tmp"
        COMMAND ${CMAKE_STRIP} -s "${stripped}.tmp"
        COMMAND ${CMAKE_COMMAND} -E rename "${stripped}.tmp" "${stripped}"
        DEPENDS ${target_name}
        COMMENT "[STRIP] ${target_name}.stripped"
    )

    add_custom_command(
        OUTPUT "${stripped}.compressed"
        COMMAND ${N64_ELFCOMPRESS} -o "${CMAKE_CURRENT_BINARY_DIR}" -c 1 "${stripped}"
        COMMAND ${CMAKE_COMMAND} -E touch "${stripped}.compressed"
        DEPENDS ${stripped}
        COMMENT "[ELFCOMPRESS] ${target_name}.stripped"
    )

    file(GLOB N64_VERSION_FILES "${N64_INCLUDE}/*.version")

    set(n64tool_deps "${stripped}.compressed" "${sym_out}")
    set(n64tool_inputs "${stripped}" "${sym_out}")

    if(ARG_DFS_ROOT)
        set(dfs_out "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.dfs")
        # Re-run mkdfs whenever the DFS_ROOT tree changes.
        file(GLOB_RECURSE DFS_INPUTS "${ARG_DFS_ROOT}/*")
        add_custom_command(
            OUTPUT "${dfs_out}"
            COMMAND ${N64_MKDFS} "${dfs_out}" "${ARG_DFS_ROOT}"
            DEPENDS ${DFS_INPUTS}
            COMMENT "[DFS] ${target_name}.dfs"
        )
        list(APPEND n64tool_deps "${dfs_out}")
        list(APPEND n64tool_inputs "${dfs_out}")
    endif()

    add_custom_command(
        OUTPUT "${rom_out}"
        COMMAND ${N64_TOOL} --toc --title "${ARG_TITLE}"
                --output "${rom_out}.tmp"
                --align 256 ${n64tool_inputs} ${N64_VERSION_FILES}
        COMMAND ${CMAKE_COMMAND} -E rename "${rom_out}.tmp" "${rom_out}"
        DEPENDS ${n64tool_deps}
        COMMENT "[Z64] ${target_name}.z64"
    )

    add_custom_target(${target_name}_rom ALL DEPENDS "${rom_out}")
endfunction()
