// DeckReader: an e-book reader for SteamOS game mode.
#include "app.h"
#include "log.h"
#include "version.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void usage() {
    std::printf(
        "DeckReader %s - e-book reader for Steam Deck / SteamOS game mode\n"
        "\n"
        "usage: deckreader [options] [book.epub]\n"
        "  --window            windowed instead of fullscreen\n"
        "  --size WxH          window size (default 1280x800)\n"
        "  --software          SDL software renderer (headless tests)\n"
        "  --script \"...\"      test script (see src/testscript.h)\n"
        "  -v, --verbose       debug logging\n"
        "  --version\n",
        DECKREADER_VERSION);
}

int main(int argc, char** argv) {
    dr::AppOptions opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--window") {
            opt.display.fullscreen = false;
        } else if (a == "--size" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &opt.display.width, &opt.display.height) != 2) {
                usage();
                return 2;
            }
        } else if (a == "--software") {
            opt.display.software = true;
        } else if (a == "--script" && i + 1 < argc) {
            opt.script = argv[++i];
        } else if (a == "-v" || a == "--verbose") {
            dr::set_log_level(dr::LogLevel::Debug);
        } else if (a == "--version") {
            std::printf("%s\n", DECKREADER_VERSION);
            return 0;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            opt.open_path = a;
        } else {
            usage();
            return 2;
        }
    }

    SDL_SetAppMetadata("DeckReader", DECKREADER_VERSION, "io.github.nicocarbone.DeckReader");
    // Under gamescope (SteamOS game mode) use its Xwayland, like
    // DeckVideoPlayer does; elsewhere SDL picks the best driver.
    if (std::getenv("GAMESCOPE_WAYLAND_DISPLAY") && !std::getenv("SDL_VIDEO_DRIVER"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11,wayland");
    // SDL would keep the screensaver off for as long as the app runs (where
    // it can: desktop sessions), even idle in the library. DeckReader keeps
    // the screen awake itself, only while a book is being read (D28).
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    // The Steam Deck's controls arrive through Steam Input's virtual pad.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "[deckreader] error: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int rc;
    {
        dr::App app;
        rc = app.run(opt);
    }
    SDL_Quit();
    return rc;
}
