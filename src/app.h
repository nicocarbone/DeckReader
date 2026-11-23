// The application: owns every service, routes input, schedules work and
// draws frames. Event-driven: the loop sleeps in SDL_WaitEventTimeout until
// input arrives or a timer that is actually needed comes due (D1).
#pragma once

#include "articles.h"
#include "canvas.h"
#include "config.h"
#include "display.h"
#include "input.h"
#include "keepawake.h"
#include "library_screen.h"
#include "menu.h"
#include "metacache.h"
#include "reader_screen.h"
#include "state.h"
#include "testscript.h"
#include "text.h"
#include "ui.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dr {

struct Paths {
    std::string home;
    std::string config_dir;  // ~/.config/deckreader (config.ini)
    std::string data_dir;    // $XDG_DATA_HOME/deckreader (state.json)
    std::string cache_dir;   // $XDG_CACHE_HOME/deckreader (covers, layouts)
    std::string bin_dir;     // where the executable is (/app/bin): helpers
    std::string app_data;    // /app/share/deckreader (fonts, config template)
    std::string engine_data; // /app/share/crengine-ng (CSS, hyphenation)
    static Paths detect();
};

struct AppOptions {
    DisplayOptions display;
    std::string script;    // test script (testscript.h)
    std::string open_path; // a book to open at start ("Open with")
};

class App {
public:
    App();
    ~App();
    int run(const AppOptions& opt);

    // --- services for the screens ---
    Paths paths;
    Config cfg;
    StateStore state;
    MetaCache meta;
    Display display;
    TextRenderer text;
    Input input;
    UiTheme theme;
    KeepAwake keep_awake; // while reading (D28)
    std::unique_ptr<Articles> articles;  // wallabag (D31); made once SDL runs
    std::unique_ptr<Articles> wikipedia; // Wikipedia reading lists (D37)

    uint64_t now() const { return now_; }
    // Pixels per layout unit for book pages (screen height / 800).
    float page_unit() const { return display.height() / Ui::BASE_HEIGHT; }
    bool engine_ready();

    void redraw() { dirty_ = true; }
    // `yield`: not over a longer message still showing (another source's
    // error, D38).
    void toast(const std::string& message, uint64_t ms = 2200, bool yield = false);
    void push_menu(std::unique_ptr<Menu> m);
    // Both are safe to call from a menu item's own callback (the menus are
    // destroyed once the current input has been handled).
    void pop_menu();
    void close_menus();
    Menu* top_menu() { return menus_.empty() ? nullptr : menus_.back().get(); }

    void open_book(const std::string& path, bool from_start);
    // The book being read, "" in the library.
    std::string open_book_path() const;
    void show_library();
    // Lists the library's current place again (when the library is shown).
    void refresh_library();
    // The library is showing that source's folder.
    bool showing_articles(ArticleSource source) const;
    // The articles source of a row or folder; nullptr for None.
    Articles* article_source(ArticleSource source);
    // Article sources that are set up, wallabag first.
    std::vector<Articles*> article_sources();
    // "Update articles" (menu, Y on a source's row): every source that is
    // set up, at the same time; "Stop updating articles" stops them.
    void update_articles();
    void stop_articles();
    bool articles_busy();
    void quit() { running_ = false; }
    void open_about();

    std::string mountinfo() const;

private:
    void loop();
    void handle_event(const SDL_Event& e);
    void dispatch(const InputEvent& e);
    // The reader was used: keep the screen awake a while longer (D28).
    void reading_activity();
    void tap(float x_px, float y_px);
    void tick();
    void render();
    void draw_toast(Ui& ui);
    void draw_loading(Ui& ui);
    void resized();
    void make_backdrop();
    int64_t next_timeout();
    bool run_script_step();
    void flush();

    enum class Screen { Library, Reader } screen_ = Screen::Library;
    std::unique_ptr<LibraryScreen> library_;
    std::unique_ptr<ReaderScreen> reader_;
    std::vector<std::unique_ptr<Menu>> menus_;
    std::vector<std::unique_ptr<Menu>> graveyard_; // closed, destroyed later

    Canvas overlay_;
    bool running_ = true;
    bool dirty_ = true;
    uint64_t now_ = 0;
    int engine_state_ = 0; // 0 not tried, 1 ready, -1 failed

    std::string toast_;
    uint64_t toast_until_ = 0;
    uint64_t toast_ms_ = 0; // how long the one showing was given

    struct PendingOpen {
        std::string path;
        bool from_start = false;
        bool active = false;
        bool shown = false; // the "Opening..." frame was presented
    } pending_;

    uint64_t cursor_hide_at_ = 0;
    std::map<SDL_JoystickID, SDL_Gamepad*> gamepads_; // closed on removal (D22)
    // Pointer/touch press position: a release far from it is a drag/swipe,
    // not a tap (D22).
    bool pointer_down_ = false;
    float down_x_ = 0, down_y_ = 0;
    std::unique_ptr<TestScript> script_;
    std::string screenshot_;
};

} // namespace dr
