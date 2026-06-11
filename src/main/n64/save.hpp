/***************************************************************************
    N64 EEPROM-backed save layer.

    16 Kbit EEPROM via libdragon eepfs. Layout matches
    project_eeprom_save_layout in CLAUDE memory:

      file              backup  on-disk
      scores            yes     528 B
      scores_jap        yes     528 B
      cont              yes     528 B
      cont_jap          no      264 B
      ttrial            yes      80 B
      ttrial_jap        no       32 B
      settings          yes      64 B
      ----------------------------------
      total used: 2032 B  /  free: 16 B (out of 2040 usable)

    All public functions are no-ops returning false when EEPROM is absent
    or the filesystem signature didn't verify (signature mismatch triggers
    a one-shot eepfs_wipe and is_present() stays true).
***************************************************************************/

#pragma once

#include "../stdint.hpp"

struct score_entry;

namespace n64save
{
    // 22-byte packed settings record stored alongside the high-score tables.
    // Bitfield order is stable for the single GCC mips64-elf target we ship.
    struct __attribute__((packed)) saved_settings_v1
    {
        uint8_t version;            // 0 = uninitialised (use defaults), 1 = this struct

        // Boot-time switches (require engine restart, surface in pre-engine menu)
        uint8_t jap        : 1;
        uint8_t prototype  : 1;
        uint8_t fps_mode   : 2;   // 0 = 30 Hz tick, 1 = 60 Hz display/30 Hz tick, 2 = 60, 3 = 120
        uint8_t widescreen : 1;
        uint8_t sound_on   : 1;
        uint8_t _rsv_boot  : 2;

        // Cheats / handling
        uint8_t freeze_timer    : 1;
        uint8_t disable_traffic : 1;
        uint8_t grippy_tyres    : 1;
        uint8_t offroad         : 1;
        uint8_t bumper          : 1;
        uint8_t turbo           : 1;
        uint8_t _rsv_cheat      : 2;

        // Engine
        uint8_t freeplay    : 1;
        uint8_t fix_bugs    : 1;
        uint8_t new_attract : 1;
        uint8_t _rsv_engine : 5;

        // Dips + gear
        uint8_t dip_time     : 2;
        uint8_t dip_traffic  : 2;
        uint8_t cont_traffic : 2;
        uint8_t gear         : 2;

        // Palette + time trial
        uint8_t car_pal        : 3;
        uint8_t ttrial_laps    : 3;
        uint8_t ttrial_traffic : 2;

        // Digital pedal speeds
        uint8_t steer_speed : 4;
        uint8_t pedal_speed : 4;

        // Analog axis invert flags
        uint8_t invert_x : 1;
        uint8_t invert_y : 1;
        uint8_t invert_z : 1;
        uint8_t _rsv_inv : 5;

        uint8_t music_timer;        // BCD, 0 = no override
        uint8_t rumble;             // 0..255 (×1/255 → controls.rumble float)

        // 12 padconfig buttons × 4 bits each. Helper accessors pack/unpack.
        // 4 bits per slot is enough for our joypad button numbering (0..14).
        uint8_t padconfig_packed[6];

        uint8_t reserved[6];        // free slack within same eepfs padding block
    };
    static_assert(sizeof(saved_settings_v1) == 22, "saved_settings_v1 layout drift");

    bool init();
    bool is_present();

    bool load_scores(bool original_mode, bool jap, score_entry* scores);
    bool save_scores(bool original_mode, bool jap, const score_entry* scores);

    bool load_ttrial(bool jap, uint16_t* best_times);
    bool save_ttrial(bool jap, const uint16_t* best_times);

    // Returns false on a brand-new EEPROM (version == 0 after signature wipe);
    // caller keeps the in-RAM defaults in that case.
    bool load_settings(saved_settings_v1& out);
    bool save_settings(const saved_settings_v1& in);

    bool wipe();

    // padconfig helpers — 12 nibbles spread across 6 bytes
    inline uint8_t padconfig_get(const uint8_t packed[6], int i)
    {
        return (i & 1) ? (packed[i >> 1] >> 4) : (packed[i >> 1] & 0x0F);
    }
    inline void padconfig_set(uint8_t packed[6], int i, uint8_t v)
    {
        packed[i >> 1] = (i & 1)
            ? ((packed[i >> 1] & 0x0F) | ((v & 0x0F) << 4))
            : ((packed[i >> 1] & 0xF0) | (v & 0x0F));
    }
}
