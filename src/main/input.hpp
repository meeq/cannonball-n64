/***************************************************************************
    Platform-neutral Input header shim.

    Picks the platform's Input implementation based on build features:
      - WITH_SDL       → src/main/sdl2/input.hpp
      - WITH_LIBDRAGON → src/main/n64/input.hpp
    Both expose the same public Input class shape so engine/frontend code is
    platform-agnostic.
***************************************************************************/

#pragma once

#if defined(WITH_SDL)
    #include "sdl2/input.hpp"
#elif defined(WITH_LIBDRAGON)
    #include "n64/input.hpp"
#else
    #error "No input backend selected (define WITH_SDL or WITH_LIBDRAGON)"
#endif
