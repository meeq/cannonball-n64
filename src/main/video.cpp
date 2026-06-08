/***************************************************************************
    Video Rendering. 
    
    - Renders the System 16 Video Layers
    - Handles Reads and Writes to these layers from the main game code
    - Interfaces with platform specific rendering code

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include "video.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"
#include "engine/oroad.hpp"
#include "n64/rendersurface.hpp"
#include "n64/hwroad_rsp.hpp"
#include "n64/hwroad_rdp.hpp"
#include "n64/hwroad_rdp_rsp.hpp"
#include <libdragon.h>

// Per-sub-phase profiler. Each macro pair brackets one rasterizer pass and
// EMA-smooths the result into n64_profile::sub_us[SLOT] for the overlay.
#define N64_PROFILE_PHASE_BEGIN() uint64_t _phase_t0 = get_ticks_us()
#define N64_PROFILE_PHASE_END(SLOT) do {                                  \
        uint64_t _phase_t1 = get_ticks_us();                              \
        uint32_t _us = (uint32_t)(_phase_t1 - _phase_t0);                 \
        n64_profile::sub_us[SLOT] =                                       \
            (n64_profile::sub_us[SLOT] * 7 + _us) >> 3;                   \
        n64_profile::raw_sub_us[SLOT] = _us;                              \
        _phase_t0 = _phase_t1;                                            \
    } while (0)

Video video;

Video::Video(void)
{
    renderer     = new Render();
    sprite_layer = new hwsprites();
    tile_layer   = new hwtiles();

    set_shadow_intensity(shadow::ORIGINAL);
    enabled      = false;
}

Video::~Video(void)
{
    delete sprite_layer;
    delete tile_layer;
    renderer->disable();
    delete renderer;
}

int Video::init(Roms* roms, video_settings_t* settings)
{
    if (!set_video_mode(settings))
        return 0;

    // Convert S16 tiles to a more useable format
    tile_layer->init(roms->tiles.rom, config.video.hires != 0);
    
    clear_tile_ram();
    clear_text_ram();
    if (roms->tiles.rom)
    {
        delete[] roms->tiles.rom;
        roms->tiles.rom = NULL;
    }

    // Convert S16 sprites
    sprite_layer->init(roms->sprites.rom);
    if (roms->sprites.rom)
    {
        delete[] roms->sprites.rom;
        roms->sprites.rom = NULL;
    }

    // Convert S16 Road Stuff
    hwroad.init(roms->road.rom, config.video.hires != 0);
    if (roms->road.rom)
    {
        delete[] roms->road.rom;
        roms->road.rom = NULL;
    }

    enabled = true;
    return 1;
}

void Video::disable()
{
    renderer->disable();
    enabled = false;
}

// ------------------------------------------------------------------------------------------------
// Configure video settings from config file
// ------------------------------------------------------------------------------------------------

int Video::set_video_mode(video_settings_t* settings)
{
    if (settings->widescreen)
    {
        config.s16_width  = S16_WIDTH_WIDE;
        config.s16_x_off = (S16_WIDTH_WIDE - S16_WIDTH) / 2;
    }
    else
    {
        config.s16_width = S16_WIDTH;
        config.s16_x_off = 0;
    }

    config.s16_height = S16_HEIGHT;

    // Internal video buffer is doubled in hi-res mode.
    if (settings->hires)
    {
        config.s16_width  <<= 1;
        config.s16_height <<= 1;
    }

    if (settings->scanlines < 0) settings->scanlines = 0;
    else if (settings->scanlines > 100) settings->scanlines = 100;

    if (settings->scale < 1)
        settings->scale = 1;

    set_shadow_intensity(settings->shadow == 0 ? shadow::ORIGINAL : shadow::MAME);

    renderer->init(config.s16_width, config.s16_height, settings->scale, settings->mode, settings->scanlines);

    return 1;
}

// --------------------------------------------------------------------------------------------
// Shadow Colours. 
// 63% Intensity is the correct value derived from hardware as follows:
//
// 1/ Shadows are just an extra 220 ohm resistor that goes to ground when enabled.
// 2/ This is in parallel with the resistor-"DAC" (3.9k, 2k, 1k, 0.5k, 0.25k), 
//    and otherwise left floating.
//
// Static calculation example:
// 
// const float rDAC   = 1.f / (1.f/3900.f + 1.f/2000.f + 1.f/1000.f + 1.f/500.f + 1.f/250.f); 
// const float rShade = 220.f;                                                             
// const float shadeAttenuation = rShade / (rShade + rDAC); // 0.63f
// 
// (MAME uses an incorrect value which is closer to 78% Intensity)
// --------------------------------------------------------------------------------------------

void Video::set_shadow_intensity(float f)
{
    renderer->set_shadow_intensity(f);
}

void Video::prepare_frame()
{
    // start_frame() zeroes the engine scratch surface through KSEG1 — much
    // cheaper than the old cached pass since stores skip the read-for-
    // ownership + writeback round-trip. Untouched scratch pixels stay at
    // alpha=0 and get dropped by the alpha-compare composite blit in
    // finalize_frame, so anything we don't overwrite reveals the RDP layers
    // (road_bg / tile_bg / tile_fg) underneath.
    if (!renderer->start_frame())
        return;

    if (!enabled)
        return;

    // OutRun Hardware Video Emulation. road_bg, both tile layers (bg/fg at
    // priority 0), all pri=8 sprites and the text layer are drawn via RDP in
    // finalize_frame, so this CPU pass only covers road_fg — written straight
    // to the scratch surface in its final RGBA5551 form.
    tile_layer->update_tile_values();

    N64_PROFILE_PHASE_BEGIN();
    if (n64::hwroad_rdp::should_render_road_fg())
    {
        // RDP road_fg overlay: build the CI4 mask + per-line TLUTs here, in
        // prepare_frame, so the writes happen with no RDP DMA traffic on the
        // RDRAM bus (the previous frame has long since finished; this frame
        // hasn't queued anything yet). build_ also rspq_waits at entry as
        // a safety net against overwriting buffers the RDP still references.
        // emit_foreground_lores_rdp in finalize_frame replays the prebuilt
        // state into the framebuffer.
        if (n64::hwroad_rdp::should_skip_cpu(hwroad.get_road_control()))
        {
            // RSP-build variant offloads the per-pixel CI4 pack to the RSP.
            // CPU still builds TLUT + spans + descriptor + pre-fill. Default
            // off — toggle n64::hwroad_rdp_rsp::enabled at runtime to A/B.
            if (n64::hwroad_rdp_rsp::enabled)
                hwroad.build_foreground_lores_rdp_rsp(renderer->rgb_lut());
            else
                hwroad.build_foreground_lores_rdp(renderer->rgb_lut());
        }
        else if (n64::hwroad_rsp::enabled)
            hwroad.render_foreground_lores_rsp(renderer->scratch_uc(),
                                               renderer->rgb_lut());
        else
            (hwroad.*hwroad.render_foreground)(renderer->scratch_uc(),
                                               renderer->rgb_lut());
    }
    N64_PROFILE_PHASE_END(n64_profile::SUB_ROAD_FG);
}

void Video::render_frame()
{
    renderer->finalize_frame();
}

// ---------------------------------------------------------------------------
// Text Handling Code
// ---------------------------------------------------------------------------

void Video::clear_text_ram()
{
    for (uint32_t i = 0; i <= 0xFFF; i++)
        tile_layer->text_ram[i] = 0;
}

void Video::write_text8(uint32_t addr, const uint8_t data)
{
    tile_layer->text_ram[addr & 0xFFF] = data;
}

void Video::write_text16(uint32_t* addr, const uint16_t data)
{
    tile_layer->text_ram[*addr & 0xFFF] = (data >> 8) & 0xFF;
    tile_layer->text_ram[(*addr+1) & 0xFFF] = data & 0xFF;

    *addr += 2;
}

void Video::write_text16(uint32_t addr, const uint16_t data)
{
    tile_layer->text_ram[addr & 0xFFF] = (data >> 8) & 0xFF;
    tile_layer->text_ram[(addr+1) & 0xFFF] = data & 0xFF;
}

void Video::write_text32(uint32_t* addr, const uint32_t data)
{
    tile_layer->text_ram[*addr & 0xFFF] = (data >> 24) & 0xFF;
    tile_layer->text_ram[(*addr+1) & 0xFFF] = (data >> 16) & 0xFF;
    tile_layer->text_ram[(*addr+2) & 0xFFF] = (data >> 8) & 0xFF;
    tile_layer->text_ram[(*addr+3) & 0xFFF] = data & 0xFF;

    *addr += 4;
}

void Video::write_text32(uint32_t addr, const uint32_t data)
{
    tile_layer->text_ram[addr & 0xFFF] = (data >> 24) & 0xFF;
    tile_layer->text_ram[(addr+1) & 0xFFF] = (data >> 16) & 0xFF;
    tile_layer->text_ram[(addr+2) & 0xFFF] = (data >> 8) & 0xFF;
    tile_layer->text_ram[(addr+3) & 0xFFF] = data & 0xFF;
}

uint8_t Video::read_text8(uint32_t addr)
{
    return tile_layer->text_ram[addr & 0xFFF];
}

// ---------------------------------------------------------------------------
// Tile Handling Code
// ---------------------------------------------------------------------------

void Video::clear_tile_ram()
{
    for (uint32_t i = 0; i <= 0xFFFF; i++)
        tile_layer->tile_ram[i] = 0;
}

void Video::write_tile8(uint32_t addr, const uint8_t data)
{
    tile_layer->tile_ram[addr & 0xFFFF] = data;
} 

void Video::write_tile16(uint32_t* addr, const uint16_t data)
{
    tile_layer->tile_ram[*addr & 0xFFFF] = (data >> 8) & 0xFF;
    tile_layer->tile_ram[(*addr+1) & 0xFFFF] = data & 0xFF;

    *addr += 2;
}

void Video::write_tile16(uint32_t addr, const uint16_t data)
{
    tile_layer->tile_ram[addr & 0xFFFF] = (data >> 8) & 0xFF;
    tile_layer->tile_ram[(addr+1) & 0xFFFF] = data & 0xFF;
}   

void Video::write_tile32(uint32_t* addr, const uint32_t data)
{
    tile_layer->tile_ram[*addr & 0xFFFF] = (data >> 24) & 0xFF;
    tile_layer->tile_ram[(*addr+1) & 0xFFFF] = (data >> 16) & 0xFF;
    tile_layer->tile_ram[(*addr+2) & 0xFFFF] = (data >> 8) & 0xFF;
    tile_layer->tile_ram[(*addr+3) & 0xFFFF] = data & 0xFF;

    *addr += 4;
}

void Video::write_tile32(uint32_t addr, const uint32_t data)
{
    tile_layer->tile_ram[addr & 0xFFFF] = (data >> 24) & 0xFF;
    tile_layer->tile_ram[(addr+1) & 0xFFFF] = (data >> 16) & 0xFF;
    tile_layer->tile_ram[(addr+2) & 0xFFFF] = (data >> 8) & 0xFF;
    tile_layer->tile_ram[(addr+3) & 0xFFFF] = data & 0xFF;
}

uint8_t Video::read_tile8(uint32_t addr)
{
    return tile_layer->tile_ram[addr & 0xFFFF];
}


// ---------------------------------------------------------------------------
// Sprite Handling Code
// ---------------------------------------------------------------------------

void Video::write_sprite16(uint32_t* addr, const uint16_t data)
{
    sprite_layer->write(*addr & 0xfff, data);
    *addr += 2;
}

// ---------------------------------------------------------------------------
// Palette Handling Code
// ---------------------------------------------------------------------------

void Video::write_pal8(uint32_t* palAddr, const uint8_t data)
{
    palette[*palAddr & 0x1fff] = data;
    refresh_palette(*palAddr & 0x1fff);
    *palAddr += 1;
}

void Video::write_pal16(uint32_t* palAddr, const uint16_t data)
{    
    uint32_t adr = *palAddr & 0x1fff;
    palette[adr]   = (data >> 8) & 0xFF;
    palette[adr+1] = data & 0xFF;
    refresh_palette(adr);
    *palAddr += 2;
}

void Video::write_pal32(uint32_t* palAddr, const uint32_t data)
{    
    uint32_t adr = *palAddr & 0x1fff;

    palette[adr]   = (data >> 24) & 0xFF;
    palette[adr+1] = (data >> 16) & 0xFF;
    palette[adr+2] = (data >> 8) & 0xFF;
    palette[adr+3] = data & 0xFF;

    refresh_palette(adr);
    refresh_palette(adr+2);

    *palAddr += 4;
}

void Video::write_pal32(uint32_t adr, const uint32_t data)
{    
    adr &= 0x1fff;

    palette[adr]   = (data >> 24) & 0xFF;
    palette[adr+1] = (data >> 16) & 0xFF;
    palette[adr+2] = (data >> 8) & 0xFF;
    palette[adr+3] = data & 0xFF;
    refresh_palette(adr);
    refresh_palette(adr+2);
}

uint8_t Video::read_pal8(uint32_t palAddr)
{
    return palette[palAddr & 0x1fff];
}

uint16_t Video::read_pal16(uint32_t palAddr)
{
    uint32_t adr = palAddr & 0x1fff;
    return (palette[adr] << 8) | palette[adr+1];
}

uint16_t Video::read_pal16(uint32_t* palAddr)
{
    uint32_t adr = *palAddr & 0x1fff;
    *palAddr += 2;
    return (palette[adr] << 8)| palette[adr+1];
}

uint32_t Video::read_pal32(uint32_t* palAddr)
{
    uint32_t adr = *palAddr & 0x1fff;
    *palAddr += 4;
    return (palette[adr] << 24) | (palette[adr+1] << 16) | (palette[adr+2] << 8) | palette[adr+3];
}

// Convert internal System 16 RRRR GGGG BBBB format palette to renderer output format
void Video::refresh_palette(uint32_t palAddr)
{
    palAddr &= ~1;
    uint32_t a = (palette[palAddr] << 8) | palette[palAddr + 1];
    uint32_t r = (a & 0x000f) << 1; // r rrr0
    uint32_t g = (a & 0x00f0) >> 3; // g ggg0
    uint32_t b = (a & 0x0f00) >> 7; // b bbb0
    if ((a & 0x1000) != 0)
        r |= 1; // r rrrr
    if ((a & 0x2000) != 0)
        g |= 1; // g gggg
    if ((a & 0x4000) != 0)
        b |= 1; // b bbbb

    renderer->convert_palette(palAddr, r, g, b);
}
