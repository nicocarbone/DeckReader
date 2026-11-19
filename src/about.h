// The About screen (D29), modelled on DeckVideoPlayer's (its D34): version
// and build, developer, source code and licence; how to support the
// project; what a bug report needs (engine and library versions, device,
// screen, keep-awake status, folders and files); credits and the trademark
// notice. Opened from the library menu and the reading menu.
#pragma once

#include "menu.h"

#include <memory>
#include <string>

namespace dr {

class App;

struct BuildInfo {
    std::string date;   // "2026-11-18"
    std::string commit; // "1a2b3c4", "1a2b3c4-modified" or ""
};

// Pure: the file scripts/build-info.sh writes into Flatpak builds
// ("date=…" and "commit=…" lines).
BuildInfo parse_build_info(const std::string& text);
// Pure: "2026-11-18 · 1a2b3c4", "2026-11-18", or "development build" when
// there is no build info (run from source).
std::string build_text(const BuildInfo& info);
// Pure: the device from /sys/class/dmi/id (sys_vendor, product_name):
// "Steam Deck OLED" (Galileo), "Steam Deck LCD" (Jupiter), else "PC".
std::string device_name(const std::string& vendor, const std::string& product);
// Pure: a path under `home` written with "~" ("~/.config/deckreader").
std::string short_path(const std::string& path, const std::string& home);

std::unique_ptr<Menu> make_about_menu(App& app);

} // namespace dr
