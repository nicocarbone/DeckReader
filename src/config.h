// ~/.config/deckreader/config.ini: what the user sets by editing a file
// (library folders, interface options). Reading choices changed from the
// in-app menu are remembered in the state file instead (state.h, D11).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dr {

struct Config {
    // [library]
    std::vector<std::string> folders; // "~" already expanded
    bool removable_media = true;
    // [interface]
    double ui_scale = 1.0;      // 0.5 .. 3
    bool clock_24h = true;
    int recently_read = 5;      // recent books on the start screen, 0..20 (0 = none)
    uint32_t accent = 0x10A38F; // teal: DeckReader's own (D20)
    // [reader]
    bool tap_zones = true;      // tap left/right edge to turn pages

    // Problems found while reading the file (unknown keys, bad values).
    std::vector<std::string> warnings;
};

// Parses config.ini text. Rules match DeckVideoPlayer's config.lua: keys are
// case-insensitive, "-" and "_" are the same, values may be quoted, "#" and
// ";" start comment lines, and "folder =" may repeat.
Config parse_config(const std::string& text, const std::string& home);

} // namespace dr
