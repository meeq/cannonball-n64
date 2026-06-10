/***************************************************************************
    YM2151 stub (N64 build).

    The original Burczynski/MAME emulator is replaced wholesale on N64: every
    music track, jingle, and FM SFX is dispatched as a pre-rendered wav64 in
    n64/audio.cpp (see "wav64 dispatch"). The chip's stream_update() never
    runs, so the operator state, frequency LUTs, envelope generator, LFO,
    sin/TL tables, and timer machinery are all dead — together they
    contributed ~48 KiB of BSS and several hundred KiB of text/rodata.

    OSound still calls two methods every tick (osound.cpp fm_dotimera /
    fm_write_reg), gated by #ifdef TIMER_CODE:

      ym->read_status() & BIT_0       -- timer A "overflow" gate
      ym->read_status() & BIT_7       -- busy gate before fm_write_reg
      ym->write_reg(reg, value)       -- ignored

    We return status = BIT_0 (timer A "overflowed", busy clear) so both
    gates pass. The Z80 code that follows the gates queues sounds, which
    audio.cpp's wav64 intercept catches before they reach the chip.

    state_view / state_bytes are host-only (find-song-loop) and never called
    on N64; stubs return 0 / write nothing so the vtable resolves.
***************************************************************************/

#include "hwaudio/ym2151.hpp"

YM2151::YM2151(float, uint32_t)
{
    irq          = false;
    initalized   = false;
    sample_freq  = 0;
    channels     = 0;
    buffer_size  = 0;
}

YM2151::~YM2151() {}

void YM2151::init(int, int) {}

void YM2151::stream_update() {}

void YM2151::write_reg(int, int) {}

int YM2151::read_status()
{
    // BIT_0 = timer A overflowed (gates fm_dotimera); BIT_7 = busy (clear so
    // fm_write_reg proceeds and is then no-op'd).
    return 0x01;
}

size_t YM2151::state_bytes() { return 0; }
void   YM2151::state_view(uint8_t*) const {}
