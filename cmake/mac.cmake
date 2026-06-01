# -----------------------------------------------------------------------------
# CannonBall macOS Setup (Homebrew, Apple Silicon)
# -----------------------------------------------------------------------------

# Homebrew install prefixes
set(boost_dir /opt/homebrew/opt/boost/include)
set(sdl2_dir  /opt/homebrew/opt/sdl2/lib/cmake/SDL2)

# Use OpenGL for rendering.
set(OPENGL 1)

find_package(OpenGL REQUIRED)

# Platform Specific Libraries
set(platform_link_libs
    ${OPENGL_LIBRARIES}
)

# macOS deprecated the OpenGL framework; silence the per-call warnings.
add_definitions(-DGL_SILENCE_DEPRECATION)
