/***************************************************************************
    Platform-neutral Audio header shim.

    Picks the platform's Audio implementation based on build features:
      - WITH_SDL       → src/main/sdl2/audio.hpp
      - WITH_LIBDRAGON → src/main/n64/audio.hpp
    Both expose the same public Audio class shape so engine/frontend code is
    platform-agnostic. Add new platforms by extending this seam.
***************************************************************************/

#pragma once

#if defined(WITH_SDL)
    #include "sdl2/audio.hpp"
#elif defined(WITH_LIBDRAGON)
    #include "n64/audio.hpp"
#else
    #error "No audio backend selected (define WITH_SDL or WITH_LIBDRAGON)"
#endif
