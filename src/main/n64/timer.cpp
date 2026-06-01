/***************************************************************************
    N64 Timer — wraps libdragon's timer_ticks() into milliseconds.
***************************************************************************/

#include "timer.hpp"
#include "platform.hpp"

namespace
{
    inline int now_ms()
    {
        // timer_ticks() returns 93.75MHz CPU counter ticks. Divide by
        // (TICKS_PER_SECOND / 1000) for ms.
        return (int)(timer_ticks() / (TICKS_PER_SECOND / 1000));
    }
}

Timer::Timer()
    : startTicks(0), pausedTicks(0), paused(false), started(false)
{
}

void Timer::start()
{
    started = true;
    paused  = false;
    startTicks = now_ms();
}

void Timer::stop()
{
    started = false;
    paused  = false;
}

void Timer::pause()
{
    if (started && !paused)
    {
        paused = true;
        pausedTicks = now_ms() - startTicks;
    }
}

void Timer::unpause()
{
    if (paused)
    {
        paused = false;
        startTicks  = now_ms() - pausedTicks;
        pausedTicks = 0;
    }
}

int Timer::get_ticks()
{
    if (started)
        return paused ? pausedTicks : (now_ms() - startTicks);
    return 0;
}

bool Timer::is_started() { return started; }
bool Timer::is_paused()  { return paused;  }
