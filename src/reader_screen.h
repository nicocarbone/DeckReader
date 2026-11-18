// The reading screen: pages from crengine as textures, the HUD, scrubbing,
// bookmarks, and the Contents / Bookmarks / Settings drawers.
#pragma once

#include "book.h"
#include "input.h"
#include "system.h"
#include "ui.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace dr {

class App;

class ReaderScreen {
public:
    explicit ReaderScreen(App& app);
    ~ReaderScreen();

    bool open(const std::string& path, bool from_start, std::string* error);
    void close(); // saves the position
    bool is_open() const { return book_.is_open(); }
    const Book& book() const { return book_; }

    void handle(const InputEvent& e);
    void tap(float fx, float fy); // fractions of the screen
    void swipe(int screens) { if (book_.is_open()) turn(screens); }
    bool hud_visible() const { return hud_; }
    void tick(uint64_t now);
    int64_t next_due(uint64_t now) const;

    // Texture of the screen being read (rendered now if not cached).
    SDL_Texture* page_texture();
    uint32_t background() const;
    bool overlay_needed() const;
    void draw_overlay(Ui& ui);

    void resized();
    // Pre-renders the next/previous screen. True if it did something.
    bool idle_work();
    bool has_idle_work() const;
    void save_position();

private:
    // Where a screen (starting at a page) is in the book. Pages are counted
    // one by one, also when two are shown side by side.
    struct Progress {
        int percent = 0;       // 0..100, the shown pages counting as read
        int chapter_left = -1; // pages after this screen in its chapter; -1 = no chapters
        int book_left = 0;     // pages after this screen
        bool last = false;     // the last screen
    };
    Progress progress_at(int page) const;
    std::string chapter_left_text(const Progress& p) const;
    void draw_progress_line(int page, uint32_t* px, int w, int h);

    struct Cached {
        int page = -1;
        SDL_Texture* tex = nullptr;
        uint64_t used = 0;
    };
    SDL_Texture* texture_for(int page, bool render_if_missing);
    void drop_textures();
    void invalidate_pages();
    void show_page(int page);
    void turn(int screens);
    void chapter(int dir);
    void toggle_bookmark();
    void cycle_theme();
    void apply_colors();
    void settings_changed(bool layout);
    void relayout();
    void refresh_bookmark_pages();
    bool bookmark_on_screen() const;
    void open_settings();
    void open_toc();
    void open_bookmarks();
    void show_hud(bool on);
    void draw_hud(Ui& ui);
    void draw_scrub(Ui& ui);

    App& app_;
    Book book_;
    Cached cache_[3];
    uint64_t use_counter_ = 0;
    std::vector<uint32_t> pixels_;
    std::vector<int> bookmark_pages_;

    bool hud_ = false;
    Battery battery_;
    std::string clock_;
    uint64_t clock_due_ = 0;

    bool scrubbing_ = false;
    double scrub_pos_ = 0;
    int scrub_page_ = 0;
    uint64_t scrub_last_ = 0, scrub_shown_ = 0;

    uint64_t relayout_at_ = 0; // debounced re-layout after a settings change
};

} // namespace dr
