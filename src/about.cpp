#include "about.h"

#include "app.h"
#include "fsutil.h"
#include "util.h"
#include "version.h"

#include <SDL3/SDL.h>
#include <crengine-ng-config.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

#include <cstdlib>

namespace dr {

namespace {
const char* const REPO = "github.com/nicocarbone/DeckReader";
const char* const DEVELOPER = "nicocarbone";
const char* const COPYRIGHT = "\xC2\xA9 2026 nicocarbone";
const char* const STATUS = "Beta \xC2\xB7 lightly tested";
const char* const TIP = "ko-fi.com/nicocarbone";
const char* const DOT = " \xC2\xB7 ";

std::string read_line(const std::string& path) {
    std::string text;
    read_file(path, text, 4096);
    return trim(text);
}

// Versions of the libraries actually loaded (the runtime's, not the
// headers').
std::string libraries() {
    int sdl = SDL_GetVersion();
    std::string out = strf("SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl),
                           SDL_VERSIONNUM_MICRO(sdl));
    FT_Library ft = nullptr;
    if (FT_Init_FreeType(&ft) == 0) {
        FT_Int a = 0, b = 0, c = 0;
        FT_Library_Version(ft, &a, &b, &c);
        FT_Done_FreeType(ft);
        out += strf("%sFreeType %d.%d.%d", DOT, a, b, c);
    }
    out += std::string(DOT) + "HarfBuzz " + hb_version_string();
    return out;
}

// "1280 × 800 · Game mode (x11)"
std::string screen_text(const App& app) {
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    std::string session = std::getenv("GAMESCOPE_WAYLAND_DISPLAY") ? "Game mode"
                          : desktop && *desktop                   ? desktop
                                                                  : "Desktop";
    const char* driver = SDL_GetCurrentVideoDriver();
    std::string out = strf("%d \xC3\x97 %d", app.display.width(), app.display.height()) + DOT + session;
    if (driver)
        out += strf(" (%s)", driver);
    return out;
}
} // namespace

BuildInfo parse_build_info(const std::string& text) {
    BuildInfo info;
    for (const std::string& line : split(text, '\n')) {
        if (starts_with(line, "date="))
            info.date = trim(line.substr(5));
        else if (starts_with(line, "commit="))
            info.commit = trim(line.substr(7));
    }
    return info;
}

std::string build_text(const BuildInfo& info) {
    if (info.date.empty())
        return "development build";
    return info.commit.empty() ? info.date : info.date + DOT + info.commit;
}

std::string device_name(const std::string& vendor, const std::string& product) {
    if (product == "Galileo")
        return "Steam Deck OLED";
    if (product == "Jupiter")
        return "Steam Deck LCD";
    if (vendor == "Valve")
        return "Valve " + product;
    return "PC";
}

std::string short_path(const std::string& path, const std::string& home) {
    if (!home.empty() && home != "/" && starts_with(path, home + "/"))
        return "~" + path.substr(home.size());
    return path;
}

std::unique_ptr<Menu> make_about_menu(App& app) {
    auto m = std::make_unique<Menu>();
    m->title = "About DeckReader";
    m->width = 760;
    auto row = [&](const std::string& label, const std::string& value) {
        m->items.push_back({label, [value] { return value; }});
    };
    auto line = [&](const std::string& label) { m->items.push_back({label}); };
    auto header = [&](const std::string& label) {
        MenuItem it;
        it.label = label;
        it.header = true;
        m->items.push_back(it);
    };
    const std::string& home = app.paths.home;
    std::string info;
    read_file(path_join(app.paths.app_data, "build-info"), info, 4096);

    row("Version", DECKREADER_VERSION);
    row("Status", STATUS);
    row("Build", build_text(parse_build_info(info)));
    row("Developer", DEVELOPER);
    row("Source code", REPO);
    row("Licence", "GPL-2.0-or-later");
    line(COPYRIGHT);
    line("Free software, provided without any warranty");

    header("Support the project");
    row("Tip (card or PayPal)", TIP);

    header("System");
    row("Engine", std::string("crengine-ng ") + CRE_NG_VERSION + ", patched");
    row("Libraries", libraries());
    row("Device", device_name(read_line("/sys/class/dmi/id/sys_vendor"), read_line("/sys/class/dmi/id/product_name")));
    row("Screen", screen_text(app));
    // Evaluated when drawn: the helper reports after it started.
    m->items.push_back({"Keep awake", [&app] { return app.keep_awake.status(); }});
    for (const std::string& folder : app.cfg.folders)
        row("Book folder", short_path(folder, home));
    row("Articles (wallabag)", app.articles->about());
    if (app.articles->configured())
        row("Articles folder", short_path(app.articles->dir(), home));
    row("Wikipedia", app.wikipedia->about());
    if (app.wikipedia->configured())
        row("Wikipedia folder", short_path(app.wikipedia->dir(), home));
    row("Settings", short_path(path_join(app.paths.config_dir, "config.ini"), home));
    row("Reading data", short_path(app.paths.data_dir, home));
    row("Cache", short_path(app.paths.cache_dir, home));

    header("Credits");
    row("Book engine", "crengine-ng (GPL-2.0-or-later)");
    row("Built on", "SDL3, FreeType, HarfBuzz, FriBiDi");
    row("Also", "libunibreak, utf8proc, nlohmann/json, libcurl");
    row("Math", "Temml, QuickJS-ng (MIT)");
    row("Fonts", "Literata, Inter, Atkinson Hyperlegible, STIX Two Math (OFL)");
    row("Hyphenation", "TeX patterns, via crengine-ng");
    row("Companion app", "DeckVideoPlayer");

    MenuItem sep;
    sep.separator = true;
    m->items.push_back(sep);
    line("Not affiliated with Valve Corporation.");
    line("Steam and Steam Deck are trademarks of Valve Corporation.");
    m->items.push_back({"Close", nullptr, nullptr, [&app] { app.pop_menu(); }});
    return m;
}

} // namespace dr
