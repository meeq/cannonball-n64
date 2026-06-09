# -----------------------------------------------------------------------------
# bake-sprites — host-side asset pipeline tool.
#
# Reads the OutRun sprite ROM via the runtime's romloader, statically walks
# the master CPU ROM's sprite/anim descriptor tables to enumerate every
# (bank, addr, pitch, max_h) tuple the hardware could render, and emits a
# pre-decoded CI4 atlas blob + a sorted lookup index linked into the N64 ELF.
#
# Invoke from the repo root:
#   cmake -S tools/bake-sprites -B build-host/bake-sprites
#   cmake --build build-host/bake-sprites
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.13)
project(bake-sprites CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(main_root "${repo_root}/src/main")

include_directories(${main_root})

set(romloader
    "${main_root}/roms.cpp"
    "${main_root}/romloader.cpp"
    "${main_root}/utils_crc32.cpp"
)
set(driver
    "${CMAKE_CURRENT_SOURCE_DIR}/bake_sprites.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/walker.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs.cpp"
)

add_executable(bake-sprites ${romloader} ${driver})
target_compile_options(bake-sprites PRIVATE -Wall -Wno-deprecated-declarations)
