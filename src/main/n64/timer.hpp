/***************************************************************************
    N64 / libdragon Timer.

    Public class shape matches src/main/sdl2/timer.hpp. Backed by
    timer_ticks() / TICKS_PER_SECOND from libdragon.
***************************************************************************/

#pragma once

class Timer
{
public:
    Timer();

    void start();
    void stop();
    void pause();
    void unpause();

    int  get_ticks();
    bool is_started();
    bool is_paused();

private:
    int  startTicks;
    int  pausedTicks;
    bool paused;
    bool started;
};
