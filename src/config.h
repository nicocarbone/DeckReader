// ~/.config/deckreader/config.ini: what the user sets by editing a file
// (library folders, interface options). Reading choices changed from the
// in-app menu are remembered in the state file instead (state.h, D11).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dr {

// [wallabag]: web articles from a wallabag account (D31).
struct WallabagSettings {
    std::string server; // e.g. https://app.wallabag.it
    std::string client_id, client_secret, username, password;
    int max_articles = 50; // unread articles kept on the Deck, newest first, 1..500
    bool images = true;    // download the articles' pictures from their sites
    // All five set: the Articles source and its menu entries appear.
    bool configured() const {
        return !server.empty() && !client_id.empty() && !client_secret.empty() && !username.empty() &&
               !password.empty();
    }
};

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
    bool keep_awake = true;     // no dimming or sleep while reading (D28)
    int keep_awake_minutes = 10; // ...until this long without input, 1..240
    WallabagSettings wallabag;

    // Problems found while reading the file (unknown keys, bad values).
    std::vector<std::string> warnings;
};

// Parses config.ini text. Rules match DeckVideoPlayer's config.lua: keys are
// case-insensitive, "-" and "_" are the same, values may be quoted, "#" and
// ";" start comment lines, and "folder =" may repeat.
Config parse_config(const std::string& text, const std::string& home);

} // namespace dr
