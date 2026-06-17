/***************************************************************************
    Music Selection Screen.

    This is a combination of a tilemap and overlayed sprites.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include "main.hpp"
#include "engine/oferrari.hpp"
#include "engine/ohud.hpp"
#include "engine/oinputs.hpp"
#include "engine/ologo.hpp"
#include "engine/omusic.hpp"
#include "engine/otiles.hpp"
#include "engine/otraffic.hpp"
#include "engine/ostats.hpp"

OMusic omusic;

OMusic::OMusic(void)
{
    tilemap             = NULL;
    tile_patch          = NULL;
    auto_cycle_disabled = false;
    track_overlay_ticks = 0;
}


OMusic::~OMusic(void)
{
    if (tilemap)    delete tilemap;
    if (tile_patch) delete tile_patch;
}

// Load Modified Widescreen version of tilemap
bool OMusic::load_widescreen_map(std::string path)
{
    int status = 0;

    if (tilemap == NULL)
    {
        tilemap = new RomLoader();
        status += tilemap->load_binary(std::string(path + "tilemap.bin").c_str());
    }

    if (tile_patch == NULL)
    {
        tile_patch = new RomLoader();
        status += tile_patch->load_binary(std::string(path + "tilepatch.bin").c_str());
    }

    return status == 0;
}

// Initialize Music Selection Screen
//
// Source: 0xB342
void OMusic::enable()
{
    oferrari.car_ctrl_active = false;
    video.clear_text_ram();
    osprites.disable_sprites();
    otraffic.disable_traffic();
    //edit jump table 3
    oinitengine.car_increment = 0;
    oferrari.car_inc_old      = 0;
    osprites.spr_cnt_main     = 0;
    osprites.spr_cnt_shadow   = 0;
    oroad.road_ctrl           = ORoad::ROAD_BOTH_P0;
    oroad.horizon_base        = ORoad::HORIZON_OFF;
    last_music_selected       = -1;
    auto_cycle_disabled       = false;
    track_overlay_ticks       = 0;
    preview_counter           = -20; // Delay before playing music
    // music_timer == 0 disables the arcade-style auto-advance from
    // music select. Skip setting the countdown so decrement_timers()
    // never trips in Outrun::main_switch GS_MUSIC. The N64 build
    // defaults to 0 (manual start required); arcade builds keep the
    // 0x30-frame timeout.
    if (config.sound.music_timer > 0)
    {
        ostats.time_counter   = config.sound.music_timer; // Move 30 seconds to timer countdown (note on the original roms this is 15 seconds)
        ostats.frame_counter  = ostats.frame_reset;
    }

    blit_music_select();
    // "SELECT MUSIC BY STEERING" is a World rev-B addition; the Japanese
    // rom has no matching blit_text2 record at the analogous offset, so
    // skip the call in Japan mode rather than render whatever happens to
    // sit there. The music-select interaction works either way.
    if (!config.engine.jap)
        ohud.blit_text2(outrun.adr.text2_select_music);
  
    osoundint.queue_sound(sound::RESET);
    if (!config.sound.preview)
        osoundint.queue_sound(sound::PCM_WAVE); // Wave Noises

    // Enable block of sprites
    entry_start = OSprites::SPRITE_ENTRIES - 0x10;    
    for (int i = entry_start; i < entry_start + 5; i++)
    {
        osprites.jump_table[i].init(i);
    }

    setup_sprite1();
    setup_sprite2();
    setup_sprite3();
    setup_sprite4();
    setup_sprite5();

    // Widescreen tiles need additional palette information copied over
    if (tile_patch->loaded && config.s16_x_off > 0)
    {
        video.tile_layer->patch_tiles(tile_patch);
        otiles.setup_palette_widescreen();
    }

    video.tile_layer->set_x_clamp(video.tile_layer->CENTRE);
    cursor_pos = 1;
    total_tracks = (int)config.sound.music.size();
}

void OMusic::disable()
{
    // Disable block of sprites
    for (int i = entry_start; i < entry_start + 5; i++)
    {
        osprites.jump_table[i].control &= ~OSprites::ENABLE;
    }

    video.tile_layer->set_x_clamp(video.tile_layer->RIGHT);

    // Restore original palette for widescreen tiles.
    if (config.s16_x_off > 0)
    {
        video.tile_layer->restore_tiles();
        otiles.setup_palette_tilemap();
    }

    video.enabled = false; // Turn screen off
}

// Music Selection Screen: Setup Radio Sprite
// Source: 0xCAF0
void OMusic::setup_sprite1()
{
    oentry *e = &osprites.jump_table[entry_start + 0];
    e->x = 28;
    e->y = 180;
    e->road_priority = 0xFF;
    e->priority = 0x1FE;
    e->zoom = 0x7F;
    e->pal_src = 0xB0;
    e->addr = outrun.adr.sprite_radio;
    osprites.map_palette(e);
}

// Music Selection Screen: Setup Equalizer Sprite
// Source: 0xCB2A
void OMusic::setup_sprite2()
{
    oentry *e = &osprites.jump_table[entry_start + 1];
    e->x = 4;
    e->y = 189;
    e->road_priority = 0xFF;
    e->priority = 0x1FE;
    e->zoom = 0x7F;
    e->pal_src = 0xA7;
    e->addr = outrun.adr.sprite_eq;
    osprites.map_palette(e);
}

// Music Selection Screen: Setup FM Radio Readout
// Source: 0xCB64
void OMusic::setup_sprite3()
{
    oentry *e = &osprites.jump_table[entry_start + 2];
    e->x = -8;
    e->y = 176;
    e->road_priority = 0xFF;
    e->priority = 0x1FE;
    e->zoom = 0x7F;
    e->pal_src = 0x87;
    e->addr = outrun.adr.sprite_fm_left;
    osprites.map_palette(e);
}

// Music Selection Screen: Setup FM Radio Dial
// Source: 0xCB9E
void OMusic::setup_sprite4()
{
    oentry *e = &osprites.jump_table[entry_start + 3];
    e->x = 68;
    e->y = 181;
    e->road_priority = 0xFF;
    e->priority = 0x1FE;
    e->zoom = 0x7F;
    e->pal_src = 0x89;
    e->addr = outrun.adr.sprite_dial_left;
    osprites.map_palette(e);
}

// Music Selection Screen: Setup Hand Sprite
// Source: 0xCBD8
void OMusic::setup_sprite5()
{
    oentry *e = &osprites.jump_table[entry_start + 4];
    e->x = 21;
    e->y = 196;
    e->road_priority = 0xFF;
    e->priority = 0x1FE;
    e->zoom = 0x7F;
    e->pal_src = 0xAF;
    e->addr = outrun.adr.sprite_hand_left;
    osprites.map_palette(e);
}

// Check for start button during music selection screen
//
// Source: 0xB768
void OMusic::check_start()
{
    if (ostats.credits && (input.has_pressed(Input::START) || input.has_pressed(Input::ACCEL)))
    {
        outrun.game_state = GS_INIT_GAME;
        ologo.disable();
        disable();
    }
}

// Tick and Blit
void OMusic::tick()
{
    // Radio Sprite
    osprites.do_spr_order_shadows(&osprites.jump_table[entry_start + 0]);

    // Animated EQ Sprite (Cycle the graphical equalizer on the radio)
    oentry *e = &osprites.jump_table[entry_start + 1];
    e->reload++; // Increment palette entry
    e->pal_src = roms.rom0p->read8((e->reload & 0x3E) >> 1 | outrun.adr.music_eq_pal);
    osprites.map_palette(e);
    osprites.do_spr_order_shadows(e);

    // Draw appropriate FM station on radio, depending on steering setting
    // Draw Dial on radio, depending on steering setting
    e = &osprites.jump_table[entry_start + 2];
    oentry *dial = &osprites.jump_table[entry_start + 3];
    oentry *hand = &osprites.jump_table[entry_start + 4];

    // Determine Track Selection Logic
    if (total_tracks < 3) tick_original(e, dial, hand);
    else tick_enhanced(e, dial, hand);

    osprites.do_spr_order_shadows(e);
    osprites.do_spr_order_shadows(dial);
    osprites.do_spr_order_shadows(hand);;

    // Enhancement: Preview Music On Sound Selection Screen
    if (config.sound.preview)
    {
        if (music_selected != last_music_selected)
        {
            if (preview_counter == 0 && last_music_selected != -1)
                osoundint.queue_sound(sound::FM_RESET);

            if (++preview_counter >= 10)
            {
                play_music();
                preview_counter = 0;
            }      
        }
    }
}

void OMusic::play_music(int index)
{
    if (index == -1) index = music_selected;

    next_track = &config.sound.music.at(index);

    switch (next_track->type)
    {
        case music_t::IS_YM_INT:
            cannonball::audio.clear_wav();
            osoundint.queue_sound(next_track->cmd);
            break;

        case music_t::IS_YM_EXT:
            cannonball::audio.clear_wav();
            roms.load_ym_data((config.data.res_path + next_track->filename).c_str());
            osoundint.queue_sound(next_track->cmd);
            break;

        case music_t::IS_WAV:
            cannonball::audio.load_wav((config.data.res_path + next_track->filename).c_str());
            break;

        case music_t::IS_NONE:
            // Silent slot — kill anything currently playing on the music
            // channels. FM_RESET both stops the wav64 music channel (via the
            // N64 intercept) and resets YM2151 register state on SDL.
            cannonball::audio.clear_wav();
            osoundint.queue_sound(sound::FM_RESET);
            break;
    }

    last_music_selected = index;
}

// In-game track-title overlay: row mirrors the music-select title (Y=11)
// and the countdown runs ~2 seconds at the 30Hz engine tick.
namespace {
    constexpr uint8_t TRACK_OVERLAY_TICKS = 60;
    // Row 5 sits above the in-game pause overlay (PAUSED is blit_text_big at
    // row 9-10, CONTINUE/RETRY/QUIT at 12/14/16) so the mid-race music-change
    // banner doesn't clobber the pause-menu options when the player pauses
    // right after changing tracks.
    constexpr uint8_t TRACK_OVERLAY_ROW   = 5;
}

// Cycle music in continuous mode
void OMusic::cycle_music()
{
    const int n = (int)config.sound.music.size();
    if (n <= 0) return;
    int idx = (int)music_selected + 1;
    if (idx >= n) idx = 0;
    music_selected = (uint8_t)idx;
    play_music();
    ohud.blit_text_big(TRACK_OVERLAY_ROW,
                       config.sound.music.at(music_selected).title.c_str(), true);
    track_overlay_ticks = TRACK_OVERLAY_TICKS;
}

void OMusic::cycle_music_prev()
{
    const int n = (int)config.sound.music.size();
    if (n <= 0) return;
    int idx = (int)music_selected - 1;
    if (idx < 0) idx = n - 1;
    music_selected = (uint8_t)idx;
    play_music();
    ohud.blit_text_big(TRACK_OVERLAY_ROW,
                       config.sound.music.at(music_selected).title.c_str(), true);
    track_overlay_ticks = TRACK_OVERLAY_TICKS;
}

void OMusic::tick_track_overlay()
{
    if (track_overlay_ticks == 0) return;
    if (--track_overlay_ticks == 0)
        ohud.blit_text_big(TRACK_OVERLAY_ROW, ""); // empty string => clear-row pass only
}

// Original Version of Music Selection Screen With 3 Tracks. 
// Wheel Left = Track 0, Wheel Centre = Track 1, Wheel Right = Track 2
void OMusic::tick_original(oentry* fm, oentry* dial, oentry* hand)
{
    // Note tiles to append to left side of text
    const uint32_t NOTE_TILES1 = 0x8A7A8A7B;
    const uint32_t NOTE_TILES2 = 0x8A7C8A7D;

    // Steer Left
    if (oinputs.steering_adjust + 0x80 <= 0x55)
    {                
        set_hand(HAND_LEFT, fm, dial, hand);
        ohud.blit_text2(outrun.adr.text2_magical);
        video.write_text32(0x1105C0, NOTE_TILES1);
        video.write_text32(0x110640, NOTE_TILES2);
        music_selected = 0;
    }
    // Centre
    else if (oinputs.steering_adjust + 0x80 <= 0xAA)
    {
        set_hand(HAND_CENTRE, fm, dial, hand);
        ohud.blit_text2(outrun.adr.text2_breeze);
        video.write_text32(0x1105C6, NOTE_TILES1);
        video.write_text32(0x110646, NOTE_TILES2);
        music_selected = 1;
    }
    // Steer Right
    else
    {
        set_hand(HAND_RIGHT, fm, dial, hand);
        ohud.blit_text2(outrun.adr.text2_splash);
        video.write_text32(0x1105C8, NOTE_TILES1);
        video.write_text32(0x110648, NOTE_TILES2);
        music_selected = 2;
    }
}

// Enhanced Version of music selection with infinite tracks.
void OMusic::tick_enhanced(oentry* fm, oentry* dial, oentry* hand)
{
    if (input.has_pressed(Input::LEFT) || oinputs.is_analog_l())
        if (--cursor_pos < 0) cursor_pos = total_tracks - 1;  
    if (input.has_pressed(Input::RIGHT) || oinputs.is_analog_r())
        if (++cursor_pos >= total_tracks) cursor_pos = 0;

    if (oinputs.steering_adjust + 0x80 <= 0x70)
        set_hand(HAND_LEFT, fm, dial, hand);
    else if (oinputs.steering_adjust + 0x80 <= 0x90)
        set_hand(HAND_CENTRE, fm, dial, hand);
    else
        set_hand(HAND_RIGHT, fm, dial, hand);

    music_selected = cursor_pos;    
    ohud.blit_text_big(11, config.sound.music.at(music_selected).title.c_str(), true);
}

void OMusic::set_hand(short direction, oentry* fm, oentry* dial, oentry* hand)
{
    if (direction == HAND_LEFT)
    {                
        hand->x = 17;
        fm->addr   = outrun.adr.sprite_fm_left;
        dial->addr = outrun.adr.sprite_dial_left;
        hand->addr = outrun.adr.sprite_hand_left;
    }
    // Centre
    else if (direction == HAND_CENTRE)
    {
        hand->x = 21;
        fm->addr   = outrun.adr.sprite_fm_centre;
        dial->addr = outrun.adr.sprite_dial_centre;
        hand->addr = outrun.adr.sprite_hand_centre;
    }
    // Steer Right
    else if (direction == HAND_RIGHT)
    {
        hand->x = 21;
        fm->addr   = outrun.adr.sprite_fm_right;
        dial->addr = outrun.adr.sprite_dial_right;
        hand->addr = outrun.adr.sprite_hand_right;
    }
}

// Blit Only: Used when frame skipping
void OMusic::blit()
{
    for (int i = 0; i < 5; i++)
        osprites.do_spr_order_shadows(&osprites.jump_table[entry_start + i]);
}

// Blit Music Selection Tiles to text ram layer (Double Row)
// 
// Source Address: 0xE0DC
// Input:          Destination address into tile ram
// Output:         None
//
// Tilemap data is stored in the ROM as a series of words.
//
// A basic compression format is used:
//
// 1/ If a word is not '0000', copy it directly to tileram
// 2/ If a word is '0000' a long follows which details the compression.
//    The upper word of the long is the value to copy.
//    The lower word of the long is the number of times to copy that value.
//
// Tile structure:
//
// MSB          LSB
// ---nnnnnnnnnnnnn Tile index (0-8191)
// ---ccccccc------ Palette (0-127)
// p--------------- Priority flag
// -??------------- Unknown

void OMusic::blit_music_select()
{
    const uint32_t TILEMAP_RAM_16 = 0x10F030;

    // Palette Ram: 1F Long Entries For Sky Shade On Horizon, For Colour Change Effect
    const uint32_t PAL_RAM_SKY = 0x120F00;

    uint32_t src_addr = outrun.adr.pal_music_select;
    uint32_t dst_addr = PAL_RAM_SKY;

    // Write 32 Palette Longs to Palette RAM
    for (int i = 0; i < 32; i++)
        video.write_pal32(&dst_addr, roms.rom0p->read32(&src_addr));

    // Set Tilemap Scroll
    otiles.set_scroll(config.s16_x_off);
    
    // --------------------------------------------------------------------------------------------
    // Blit to Tilemap 16: Widescreen Version. Uses Custom Tilemap. 
    // --------------------------------------------------------------------------------------------
    if (tilemap->loaded && config.s16_x_off > 0)
    {
        uint32_t tilemap16 = TILEMAP_RAM_16 - 20;
        src_addr = 0;

        const uint16_t rows = tilemap->read16(&src_addr);
        const uint16_t cols = tilemap->read16(&src_addr);

        for (int y = 0; y < rows; y++)
        {
            dst_addr = tilemap16;
            for (int x = 0; x < cols; x++)
                video.write_tile16(&dst_addr, tilemap->read16(&src_addr));
            tilemap16 += 0x80; // next line of tiles
        }
    }
    // --------------------------------------------------------------------------------------------
    // Blit to Tilemap 16: Original 4:3 Version. 
    // --------------------------------------------------------------------------------------------
    else
    {
        uint32_t tilemap16 = TILEMAP_RAM_16;
        src_addr = TILEMAP_MUSIC_SELECT;

        for (int y = 0; y < 28; y++)
        {
            dst_addr = tilemap16;
            for (int x = 0; x < 40;)
            {
                // get next tile
                uint32_t data = roms.rom0p->read16(&src_addr);
                // No Compression: write tile directly to tile ram
                if (data != 0)
                {
                    video.write_tile16(&dst_addr, data);    
                    x++;
                }
                // Compression
                else
                {
                    uint16_t value = roms.rom0p->read16(&src_addr); // tile index to copy
                    uint16_t count = roms.rom0p->read16(&src_addr); // number of times to copy value

                    for (uint16_t i = 0; i <= count; i++)
                    {
                        video.write_tile16(&dst_addr, value);
                        x++;
                    }
                }
            }
            tilemap16 += 0x80; // next line of tiles
        } // end for

        // Fix Misplaced tile on music select screen (above steering wheel)
        if (config.engine.fix_bugs)
            video.write_tile16(0x10F730, 0x0C80);
    } 
}
