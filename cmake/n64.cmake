# -----------------------------------------------------------------------------
# CannonBall N64 (libdragon) target file
#
# Used with the libdragon cross-compile toolchain. Invoke from cmake/:
#   cmake -B ../build-n64 \
#         -DCMAKE_TOOLCHAIN_FILE=$(pwd)/n64-toolchain.cmake \
#         -DTARGET=n64.cmake \
#         .
#
# The toolchain file sets N64=1 and CMAKE_*_COMPILER. This file populates the
# variables the base CMakeLists.txt consumes for an N64 build.
# -----------------------------------------------------------------------------

# Sound is feasible on N64 but is disabled in Phase 1 stubs.
# Re-enable in Phase 4 once the libdragon mixer integration lands.
# add_definitions(-DCOMPILE_SOUND_CODE)

# N64 platform layer. Public class shapes (Audio/Input/Timer/RenderBase/Render)
# match the SDL versions in src/main/sdl2 so the engine and frontend code see
# the same interface — selection happens at the seams in main.hpp and
# video.cpp via WITH_SDL / WITH_LIBDRAGON. Nothing in src/main/sdl2 is touched.
set(src_n64
    "${main_cpp_base}/n64/platform.hpp"
    "${main_cpp_base}/n64/audio.hpp"
    "${main_cpp_base}/n64/input.hpp"
    "${main_cpp_base}/n64/timer.hpp"
    "${main_cpp_base}/n64/renderbase.hpp"
    "${main_cpp_base}/n64/rendersurface.hpp"
    "${main_cpp_base}/n64/save.hpp"

    "${main_cpp_base}/n64/n64main.cpp"
    "${main_cpp_base}/n64/audio.cpp"
    "${main_cpp_base}/n64/input.cpp"
    "${main_cpp_base}/n64/timer.cpp"
    "${main_cpp_base}/n64/renderbase.cpp"
    "${main_cpp_base}/n64/rendersurface.cpp"
    "${main_cpp_base}/n64/save.cpp"
    "${main_cpp_base}/utils_crc32.cpp"
    "${main_cpp_base}/utils_crc32.hpp"
)

# Source exclusions for the N64 target (SDL backend, host renderers, DirectX
# FF, shared main(), menu + cabinet diagnostics) live in cmake/CMakeLists.txt
# under `if(N64)` so they survive the unconditional set() blocks above SRCS.

set(platform_link_libs )
set(platform_link_dirs )
