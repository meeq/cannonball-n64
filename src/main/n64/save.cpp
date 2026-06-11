/***************************************************************************
    N64 EEPROM-backed save layer — eepfs implementation.

    Init contract:
      EEPROM_NONE  → eepfs_init not called, all subsequent calls return false
      EEPROM_4K    → mismatch with our 16k layout; treat as NONE + log
      EEPROM_16K   → eepfs_init(entries, 7); on signature mismatch, eepfs_wipe()
                     and start fresh — saves return success once the layout
                     stabilises.

    Read path: eepfs_read returns EEPFS_CORRUPTED on a freshly wiped (or
    backup-failed) file because the all-zero block's checksum doesn't match.
    Callers treat that as "no save data" and keep the in-RAM defaults.
***************************************************************************/

#include <libdragon.h>
#include <cstring>

#include "save.hpp"
#include "../engine/ohiscore.hpp"

namespace
{
    // Score table on-disk record: 13 B × 20 entries = 260 B / table.
    struct __attribute__((packed)) saved_score_entry
    {
        uint32_t score;        // BCD
        uint32_t maptiles;     // route bitmap
        uint16_t time;         // BCD lap time
        uint8_t  initials[3];
    };
    static_assert(sizeof(saved_score_entry) == 13, "saved_score_entry layout drift");

    constexpr int SCORES_PER_TABLE = 20;
    constexpr size_t SCORES_BYTES  = sizeof(saved_score_entry) * SCORES_PER_TABLE;  // 260
    constexpr size_t TTRIAL_BYTES  = sizeof(uint16_t) * 15;                          // 30

    // Locked layout. Order must match the on-disk filesystem signature —
    // any reordering / resize forces eepfs_wipe() on next boot.
    const eepfs_entry_t k_entries[] = {
        { "scores",     SCORES_BYTES,                  true, true  },  // arcade world
        { "scores_jap", SCORES_BYTES,                  true, true  },
        { "cont",       SCORES_BYTES,                  true, true  },  // continuous world
        { "cont_jap",   SCORES_BYTES,                  true, false },
        { "ttrial",     TTRIAL_BYTES,                  true, true  },
        { "ttrial_jap", TTRIAL_BYTES,                  true, false },
        { "settings",   sizeof(n64save::saved_settings_v1), true, true },
    };
    constexpr size_t k_entry_count = sizeof(k_entries) / sizeof(k_entries[0]);

    bool g_eepfs_ready = false;

    const char* scores_path(bool original_mode, bool jap)
    {
        if (original_mode)
            return jap ? "scores_jap" : "scores";
        return jap ? "cont_jap" : "cont";
    }

    const char* ttrial_path(bool jap) { return jap ? "ttrial_jap" : "ttrial"; }
}

namespace n64save
{
    bool init()
    {
        eeprom_type_t et = eeprom_present();
        if (et == EEPROM_NONE)
        {
            debugf("n64save: no EEPROM present — saves disabled\n");
            return false;
        }
        if (et == EEPROM_4K)
        {
            debugf("n64save: 4K EEPROM but layout needs 16K — saves disabled\n");
            return false;
        }

        int rc = eepfs_init(k_entries, k_entry_count);
        if (rc != EEPFS_ESUCCESS)
        {
            debugf("n64save: eepfs_init failed: %d — saves disabled\n", rc);
            return false;
        }

        if (!eepfs_verify_signature())
        {
            debugf("n64save: signature mismatch — wiping EEPROM (~3.8 s)\n");
            eepfs_wipe();
        }

        g_eepfs_ready = true;
        return true;
    }

    bool is_present() { return g_eepfs_ready; }

    bool load_scores(bool original_mode, bool jap, score_entry* scores)
    {
        if (!g_eepfs_ready || !scores) return false;

        saved_score_entry buf[SCORES_PER_TABLE];
        if (eepfs_read(scores_path(original_mode, jap), buf, sizeof(buf)) != EEPFS_ESUCCESS)
            return false;

        for (int i = 0; i < SCORES_PER_TABLE; ++i)
        {
            scores[i].score    = buf[i].score;
            scores[i].maptiles = buf[i].maptiles;
            scores[i].time     = buf[i].time;
            scores[i].initial1 = buf[i].initials[0];
            scores[i].initial2 = buf[i].initials[1];
            scores[i].initial3 = buf[i].initials[2];
        }
        return true;
    }

    bool save_scores(bool original_mode, bool jap, const score_entry* scores)
    {
        if (!g_eepfs_ready || !scores) return false;

        saved_score_entry buf[SCORES_PER_TABLE];
        for (int i = 0; i < SCORES_PER_TABLE; ++i)
        {
            buf[i].score       = scores[i].score;
            buf[i].maptiles    = scores[i].maptiles;
            buf[i].time        = scores[i].time;
            buf[i].initials[0] = scores[i].initial1;
            buf[i].initials[1] = scores[i].initial2;
            buf[i].initials[2] = scores[i].initial3;
        }
        return eepfs_write(scores_path(original_mode, jap), buf, sizeof(buf)) == EEPFS_ESUCCESS;
    }

    bool load_ttrial(bool jap, uint16_t* best_times)
    {
        if (!g_eepfs_ready || !best_times) return false;
        return eepfs_read(ttrial_path(jap), best_times, TTRIAL_BYTES) == EEPFS_ESUCCESS;
    }

    bool save_ttrial(bool jap, const uint16_t* best_times)
    {
        if (!g_eepfs_ready || !best_times) return false;
        return eepfs_write(ttrial_path(jap), best_times, TTRIAL_BYTES) == EEPFS_ESUCCESS;
    }

    bool load_settings(saved_settings_v1& out)
    {
        if (!g_eepfs_ready) return false;
        if (eepfs_read("settings", &out, sizeof(out)) != EEPFS_ESUCCESS)
            return false;
        // version == 0 means the record was never written — caller keeps defaults.
        return out.version == 1;
    }

    bool save_settings(const saved_settings_v1& in)
    {
        if (!g_eepfs_ready) return false;
        return eepfs_write("settings", &in, sizeof(in)) == EEPFS_ESUCCESS;
    }

    bool wipe()
    {
        if (!g_eepfs_ready) return false;
        eepfs_wipe();
        return true;
    }
}
