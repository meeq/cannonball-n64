#pragma once

#include "stdint.hpp"

class HWRoad
{
public:
    HWRoad();
    ~HWRoad();

    void init(const uint8_t*, const bool hires);
    void write16(uint32_t adr, const uint16_t data);
    void write16(uint32_t* adr, const uint16_t data);
    void write32(uint32_t* adr, const uint32_t data);
    uint16_t read_road_control();
    void write_road_control(const uint8_t);
    uint8_t  get_road_control() const { return road_control; }
    // Per-pixel foreground rasteriser. Writes RGBA5551 directly into the
    // engine scratch surface (320x224, uint16_t per pixel) using the supplied
    // engine→RGBA5551 palette LUT. Pixels left untouched stay at whatever the
    // caller's start_frame() zeroed them to — alpha=0, transparent under the
    // composite blit so the underlying road_bg / tile layers show through.
    void (HWRoad::*render_foreground)(uint16_t* dst_rgba, const uint16_t* rgb_lut);

    // RDP road background — looks up which scanlines are solid-filled per the
    // S16 road_control / road RAM contents and emits batched rdpq fill rects
    // directly into the framebuffer at (x_offset, y_offset). Assumes the
    // caller has attached the display and not yet set a fill mode.
    void render_rdp_background(const uint16_t* rgb_lut, int x_offset, int y_offset, int s16_width);

private:
    uint8_t road_control;
    uint16_t color_offset1;
    uint16_t color_offset2;
    uint16_t color_offset3;
    int32_t x_offset;

    static const uint16_t ROAD_RAM_SIZE = 0x1000;
    static const uint16_t rom_size = 0x8000;

    // Decoded road graphics
    uint8_t roads[0x40200];

    // Two halves of RAM
    uint16_t ram[ROAD_RAM_SIZE / 2];
    uint16_t ramBuff[ROAD_RAM_SIZE / 2];

    void decode_road(const uint8_t*);
    void render_foreground_lores(uint16_t* dst_rgba, const uint16_t* rgb_lut);
    void render_foreground_hires(uint16_t* dst_rgba, const uint16_t* rgb_lut);

    // RSP foreground rasteriser — same signature/contract as
    // render_foreground_lores. Caller must invoke n64::hwroad_rsp::init()
    // once at startup before this is safe to use.
public:
    void render_foreground_lores_rsp(uint16_t* dst_rgba, const uint16_t* rgb_lut);

    // RDP foreground rasteriser — handles all 4 (road_control & 3) values.
    // Split into two phases so the CPU mask-build doesn't fight RDP DMA
    // traffic for the RDRAM bus:
    //   build_…  runs before finalize_frame queues road_bg / tiles / etc.
    //            Fills per-line state + CI4 mask + per-line TLUT in RAM
    //            (writes hit uncached aliases, so RDP DMA sees fresh bytes
    //            with no writeback).
    //   emit_…   runs after the scratch composite, with rdpq_attach'd target.
    //            Reads the prebuilt state and emits per-line fill + CI4
    //            textured rectangles.
    // n64::hwroad_rdp::init() must run once at startup before either call.
    void build_foreground_lores_rdp(const uint16_t* rgb_lut);
    void emit_foreground_lores_rdp(int x_off, int y_off);

    // RSP-build variant of build_foreground_lores_rdp. Same on-frame
    // contract — populates the shared mask/TLUT/line state in
    // n64::hwroad_rdp::detail. CPU still does TLUT + spans + descriptor
    // build + uncached pre-fill; RSP overlay does the per-pixel CI4 pack.
    // Caller selects via n64::hwroad_rdp_rsp::enabled. emit_… consumes
    // the same buffers either way. n64::hwroad_rdp_rsp::init() must run
    // once at startup before this is safe to call.
    void build_foreground_lores_rdp_rsp(const uint16_t* rgb_lut);
};

extern HWRoad hwroad;