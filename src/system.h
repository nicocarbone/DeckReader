// What the reader's status line needs from the system: battery and clock.
// In Gamescope the Steam top bar is hidden, so the HUD shows both
// (requirements 5.2).
#pragma once

#include <string>

namespace dr {

struct Battery {
    bool present = false;
    int percent = 0;
    bool charging = false;
};

// First battery in /sys/class/power_supply (readable inside the Flatpak
// sandbox without any permission).
Battery read_battery();
// Local time "14:05" or "2:05 PM".
std::string clock_text(bool h24);
// Milliseconds until the next minute starts (to refresh the clock).
int ms_to_next_minute();

} // namespace dr
