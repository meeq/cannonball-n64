/***************************************************************************
    YM2151 stub (N64 build) — see ym2151.cpp for the full story.

    The Burczynski/MAME emulator is replaced wholesale by wav64 dispatch on
    N64. This shim preserves the SoundChip interface so engine/audio still
    links, but the chip emits nothing and holds no per-operator state.

    The full emulator's YM2151Operator struct + private helper declarations
    are intentionally absent — they were only meaningful to stream_update().
***************************************************************************/

#pragma once

#include <cstddef>

#include "stdint.hpp"
#include "romloader.hpp"
#include "hwaudio/soundchip.hpp"

class YM2151 : public SoundChip
{
public:
    bool irq;

    YM2151(float volume, uint32_t clock);
    ~YM2151();
    void init(int rate, int fps);
    void stream_update();
    void write_reg(int r, int v);
    int read_status();

    // Host-tool entry points (find-song-loop). Stubs on N64 — never called
    // at runtime, but kept so the symbol resolves if anything references
    // them.
    static size_t state_bytes();
    void state_view(uint8_t* dst) const;
};
