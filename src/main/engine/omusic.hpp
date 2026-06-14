/***************************************************************************
    Music Selection Screen.

    This is a combination of a tilemap and overlayed sprites.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#pragma once

#include "outrun.hpp"

class RomLoader;

class OMusic
{
public:
    OMusic(void);
    ~OMusic(void);

    bool load_widescreen_map(std::string path);
    void enable();
    void disable();
    void tick();
    void blit();
    void check_start();
    void play_music(int index = -1);
    void cycle_music();
    void cycle_music_prev();

    // Drives the in-game "now playing: <title>" overlay countdown. Call
    // once per engine tick while in-game; cycle_music / cycle_music_prev
    // arm the overlay by blitting the title and setting the counter,
    // this tears it back down when the counter hits zero.
    void tick_track_overlay();

    // Set when the player has manually cycled tracks during a run. The
    // engine's every-5-stages auto-cycle (oinitengine.cpp) skips while
    // this is true, giving the player full control for the rest of the
    // run. Reset in enable() so a fresh music-select session re-arms
    // the auto-cycle.
    bool auto_cycle_disabled;

private:
    // Modified Widescreen version of the Music Select Tilemap
    RomLoader* tilemap;
    // Additional Widescreen tiles
    RomLoader* tile_patch;

    // Next track to play
    music_t* next_track;

    // Music Track Selected By Player
    uint8_t music_selected;

    // Total tracks to include in music select (> 3 means user has added extra ones)
    int total_tracks;

    // Enahcned: Current Cursor Position
    int cursor_pos;

    uint16_t entry_start;

    // Used to preview music track
    int16_t last_music_selected;
    int8_t preview_counter;

    // Engine-tick countdown for the in-game track-title overlay. Tick
    // rate is the engine logic rate (30Hz), so 60 ≈ 2 seconds.
    uint8_t track_overlay_ticks;

    const static short HAND_LEFT = 0, HAND_CENTRE = 1, HAND_RIGHT = 2;
    
	void setup_sprite1();
	void setup_sprite2();
	void setup_sprite3();
	void setup_sprite4();
	void setup_sprite5();
    void tick_original(oentry*, oentry*, oentry*);
    void tick_enhanced(oentry*, oentry*, oentry*);
    void set_hand(short, oentry*, oentry*, oentry*);
    void blit_music_select();
};

extern OMusic omusic;

