/***************************************************************************
    N64 save layer — Phase 1 stubs.

    Phase 5 binds these to libdragon eepfs_* once the high-score struct
    layouts are finalised. EEPROM type (4 Kbit / 16 Kbit) is detected at
    boot via eeprom_present().
***************************************************************************/

#include "save.hpp"
#include "platform.hpp"

namespace n64save
{
    bool init()
    {
        // Placeholder — Phase 5 will register an eepfs entry list and call
        // eepfs_init() here, falling back gracefully if EEPROM isn't present.
        return true;
    }

    bool load_high_scores()  { return false; }
    bool save_high_scores()  { return false; }
    bool load_ttrial_scores(){ return false; }
    bool save_ttrial_scores(){ return false; }
}
