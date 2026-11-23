#include "reader_screen.h"

#include "app.h"
#include "log.h"
#include "util.h"

#include <algorithm>
#include <cmath>

namespace dr {

ReaderScreen::ReaderScreen(App& app) : app_(app) {}

ReaderScreen::~ReaderScreen() {
    drop_textures();
}

bool ReaderScreen::open(const std::string& path, bool from_start, std::string* error) {
    close();
    if (!app_.engine_ready()) {
        if (error)
            *error = "The book engine could not start (bundled fonts missing).";
        return false;
    }
    uint64_t t0 = mono_ms();
    if (!book_.open(path, error))
        return false;
    apply_colors();
    book_.configure(app_.state.settings, app_.display.width(), app_.display.height(), app_.page_unit());
    BookState& st = app_.state.at(path);
    if (!from_start && st.finished && st.xpointer.empty()) {
        // Marked as read in the library (D34): its last screen, as a book
        // read to the end, so it stays finished.
        book_.go_to_page(engine::last_screen_page(book_.page_count(), book_.pages_per_screen()));
    } else if (from_start || st.xpointer.empty() || !book_.go_to_position(st.xpointer)) {
        book_.go_to_page(0);
    }
    st.title = book_.info().title;
    st.author = book_.info().author;
    refresh_bookmark_pages();
    hud_ = false;
    scrubbing_ = false;
    relayout_at_ = 0;
    app_.input.set_stick_scrub(true);
    save_position();
    app_.redraw();
    LOG_I("opened %s in %llu ms", path_basename(path).c_str(), static_cast<unsigned long long>(mono_ms() - t0));
    return true;
}

void ReaderScreen::close() {
    if (!book_.is_open())
        return;
    save_position();
    drop_textures();
    book_.close();
    bookmark_pages_.clear();
    app_.input.set_stick_scrub(false);
}

void ReaderScreen::save_position() {
    if (!book_.is_open())
        return;
    BookState& st = app_.state.at(book_.path());
    int cur = book_.current_page(), count = book_.page_count();
    st.xpointer = book_.position();
    st.percent = progress_at(cur).percent * 100; // same counting as the HUD and progress line
    st.page = cur;
    st.pages = count;
    st.finished = count > 0 && cur + book_.pages_per_screen() >= count;
    if (st.finished)
        st.percent = 10000;
    st.last_read = wall_time_ms();
    st.hidden = false; // read again: back among the recent books
    app_.state.mark_dirty();
}

// --- progress ----------------------------------------------------------------

ReaderScreen::Progress ReaderScreen::progress_at(int page) const {
    Progress p;
    const int n = std::max(1, book_.page_count()), pps = book_.pages_per_screen();
    const int shown_end = std::min(n, page + pps); // pages up to here are read
    p.last = shown_end >= n;
    p.percent = p.last ? 100 : shown_end * 100 / n;
    p.book_left = n - shown_end;
    if (!book_.toc().empty()) {
        int next = book_.next_chapter_page(page);
        p.chapter_left = next < 0 ? p.book_left : std::max(0, next - shown_end);
    }
    return p;
}

std::string ReaderScreen::chapter_left_text(const Progress& p) const {
    if (p.chapter_left < 0)
        return {};
    if (p.chapter_left == 0)
        return p.last ? std::string() : "Last page of the chapter";
    return p.chapter_left == 1 ? "1 page left in chapter" : strf("%d pages left in chapter", p.chapter_left);
}

// The optional progress line (D24), drawn into the page image itself, so a
// page turn costs nothing more. Pages left in the chapter under the left
// page; percentage and pages left in the book under the right one.
void ReaderScreen::draw_progress_line(int page, uint32_t* px, int w, int h) {
    const ReaderSettings& s = app_.state.settings;
    const float u = app_.page_unit();
    const Progress p = progress_at(page);
    const std::string left = chapter_left_text(p);
    const std::string right = p.last ? std::string("100% \xC2\xB7 End of the book")
                            : p.book_left == 1 ? strf("%d%% \xC2\xB7 1 page left", p.percent)
                                               : strf("%d%% \xC2\xB7 %d pages left", p.percent, p.book_left);
    const float size = 15 * u;
    const int band = std::max(1, static_cast<int>(std::lround(PROGRESS_LINE_UNITS * u)));
    const int cy = static_cast<int>(std::lround(h - (s.vert_margin + PROGRESS_LINE_UNITS) * u / 2));
    const int y0 = std::clamp(cy - band / 2, 0, std::max(0, h - band));
    Canvas strip;
    strip.resize(w, band);
    const Color ink = Color::hex(THEMES[s.theme].text, 0.6f);
    const float base = static_cast<float>(cy - y0) + app_.text.cap_height(size, false) / 2;
    const float margin = std::max(s.side_margin, 16) * u;
    const float half = w / 2.0f - margin - 20 * u;
    if (!left.empty())
        app_.text.draw(strip, margin, base, app_.text.ellipsize(left, size, false, half), size, false, ink);
    const std::string r = app_.text.ellipsize(right, size, false, half);
    if (!r.empty())
        app_.text.draw(strip, w - margin - app_.text.measure(r, size, false), base, r, size, false, ink);
    // Premultiplied strip over the opaque page.
    for (int y = 0; y < band && y0 + y < h; y++) {
        uint32_t* dst = px + static_cast<size_t>(y0 + y) * w;
        const uint32_t* src = strip.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; x++) {
            uint32_t a = src[x] >> 24;
            if (!a)
                continue;
            uint32_t out = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                uint32_t c = ((src[x] >> sh) & 0xFF) + (((dst[x] >> sh) & 0xFF) * (255 - a) + 127) / 255;
                out |= std::min<uint32_t>(c, 255) << sh;
            }
            dst[x] = out;
        }
    }
}

// --- page textures ----------------------------------------------------------

SDL_Texture* ReaderScreen::texture_for(int page, bool render_if_missing) {
    for (Cached& c : cache_)
        if (c.tex && c.page == page) {
            c.used = ++use_counter_;
            return c.tex;
        }
    if (!render_if_missing || !book_.is_open())
        return nullptr;
    const int cur = book_.current_page();
    Cached* slot = nullptr;
    for (Cached& c : cache_)
        if (!c.tex || c.page < 0) {
            slot = &c;
            break;
        }
    if (!slot)
        for (Cached& c : cache_)
            if (c.page != cur && (!slot || c.used < slot->used))
                slot = &c;
    if (!slot)
        return nullptr;
    if (!slot->tex)
        slot->tex = app_.display.create_page_texture();
    if (!slot->tex)
        return nullptr;
    const int w = app_.display.width(), h = app_.display.height();
    pixels_.resize(static_cast<size_t>(w) * h);
    uint64_t t0 = mono_ms();
    if (!book_.render(page, pixels_.data(), w, h))
        return nullptr;
    if (app_.state.settings.progress_line)
        draw_progress_line(page, pixels_.data(), w, h);
    app_.display.upload(slot->tex, pixels_.data(), w * 4);
    LOG_D("page %d rendered in %llu ms", page, static_cast<unsigned long long>(mono_ms() - t0));
    slot->page = page;
    slot->used = ++use_counter_;
    return slot->tex;
}

void ReaderScreen::drop_textures() {
    for (Cached& c : cache_) {
        app_.display.destroy_texture(c.tex);
        c = Cached{};
    }
    pixels_.clear();
    pixels_.shrink_to_fit();
}

void ReaderScreen::invalidate_pages() {
    for (Cached& c : cache_)
        c.page = -1;
}

SDL_Texture* ReaderScreen::page_texture() {
    if (!book_.is_open())
        return nullptr;
    return texture_for(book_.current_page(), true);
}

uint32_t ReaderScreen::background() const {
    return THEMES[app_.state.settings.theme].bg;
}

bool ReaderScreen::has_idle_work() const {
    if (!book_.is_open() || scrubbing_ || relayout_at_)
        return false;
    int cur = book_.current_page(), pps = book_.pages_per_screen(), n = book_.page_count();
    for (int p : {cur + pps, cur - pps}) {
        if (p < 0 || p >= n)
            continue;
        bool cached = false;
        for (const Cached& c : cache_)
            cached = cached || (c.tex && c.page == p);
        if (!cached)
            return true;
    }
    return false;
}

bool ReaderScreen::idle_work() {
    if (!has_idle_work())
        return false;
    int cur = book_.current_page(), pps = book_.pages_per_screen(), n = book_.page_count();
    // The next screen first: reading goes forward far more often.
    for (int p : {cur + pps, cur - pps})
        if (p >= 0 && p < n && !texture_for(p, false)) {
            texture_for(p, true);
            if (!has_idle_work()) {
                // Both neighbours are on the GPU: the CPU copy is not needed
                // until the next uncached page (4 MB on the Deck, 33 MB at 4K).
                pixels_.clear();
                pixels_.shrink_to_fit();
            }
            return true;
        }
    return false;
}

// --- navigation -------------------------------------------------------------

void ReaderScreen::show_page(int page) {
    book_.go_to_page(page);
    save_position();
    app_.redraw();
}

void ReaderScreen::turn(int screens) {
    int pps = book_.pages_per_screen(), cur = book_.current_page(), n = book_.page_count();
    int target = cur + screens * pps;
    if (target < 0) {
        if (cur > 0)
            show_page(0);
        else
            app_.toast("Beginning of the book");
        return;
    }
    if (target >= n) {
        save_position(); // marks the book finished
        app_.toast("End of the book");
        return;
    }
    show_page(target);
}

void ReaderScreen::chapter(int dir) {
    int cur = book_.current_page();
    int page = dir > 0 ? book_.next_chapter_page(cur) : book_.prev_chapter_page(cur);
    if (page < 0) {
        app_.toast(book_.toc().empty() ? "This book has no chapters" : dir > 0 ? "Last chapter" : "First chapter");
        return;
    }
    show_page(page);
    std::string title = book_.chapter_title_at(book_.current_page());
    if (!title.empty())
        app_.toast(title);
}

// --- bookmarks ----------------------------------------------------------------

void ReaderScreen::refresh_bookmark_pages() {
    bookmark_pages_.clear();
    if (!book_.is_open())
        return;
    if (const BookState* st = app_.state.find(book_.path()))
        for (const Bookmark& b : st->bookmarks)
            bookmark_pages_.push_back(book_.page_of_position(b.xpointer));
}

bool ReaderScreen::bookmark_on_screen() const {
    if (!book_.is_open())
        return false;
    int cur = book_.current_page(), pps = book_.pages_per_screen();
    for (int p : bookmark_pages_)
        if (p >= cur && p < cur + pps)
            return true;
    return false;
}

void ReaderScreen::toggle_bookmark() {
    BookState& st = app_.state.at(book_.path());
    int cur = book_.current_page(), pps = book_.pages_per_screen();
    // Every bookmark on this screen goes (there can be two on a spread, made
    // in one-page mode or at another text size), so the ribbon disappears.
    size_t removed = 0;
    for (size_t i = st.bookmarks.size(); i-- > 0;) {
        if (i < bookmark_pages_.size() && bookmark_pages_[i] >= cur && bookmark_pages_[i] < cur + pps) {
            st.bookmarks.erase(st.bookmarks.begin() + static_cast<long>(i));
            removed++;
        }
    }
    if (removed) {
        refresh_bookmark_pages();
        app_.state.mark_dirty();
        app_.toast(removed == 1 ? "Bookmark removed" : strf("%zu bookmarks removed", removed));
        app_.redraw();
        return;
    }
    Bookmark b;
    b.xpointer = book_.position();
    b.percent = book_.percent();
    int page = book_.page_of_position(b.xpointer); // the anchor may be on the right page
    b.chapter = book_.chapter_title_at(page >= 0 ? page : cur);
    b.time = wall_time();
    st.bookmarks.push_back(b);
    std::stable_sort(st.bookmarks.begin(), st.bookmarks.end(),
                     [](const Bookmark& a, const Bookmark& c) { return a.percent < c.percent; });
    refresh_bookmark_pages();
    app_.state.mark_dirty();
    app_.toast("Bookmark added");
    app_.redraw();
}

// --- settings -------------------------------------------------------------------

void ReaderScreen::apply_colors() {
    const ThemeDef& t = THEMES[app_.state.settings.theme];
    book_.set_colors(t.bg, t.text);
}

void ReaderScreen::cycle_theme() {
    ReaderSettings& s = app_.state.settings;
    s.theme = (s.theme + 1) % THEME_COUNT;
    settings_changed(false);
    app_.toast(THEMES[s.theme].name);
}

void ReaderScreen::settings_changed(bool layout) {
    app_.state.settings.clamp();
    app_.state.mark_dirty();
    if (layout) {
        // Laying a book out takes a moment: wait until the buttons rest.
        // "Updating pages" shows meanwhile (and during the layout itself).
        relayout_at_ = app_.now() + 250;
    } else {
        apply_colors();
        invalidate_pages();
    }
    app_.redraw();
}

void ReaderScreen::relayout() {
    relayout_at_ = 0;
    if (!book_.is_open())
        return;
    if (book_.configure(app_.state.settings, app_.display.width(), app_.display.height(), app_.page_unit())) {
        invalidate_pages();
        refresh_bookmark_pages();
        save_position();
    }
    app_.redraw();
}

void ReaderScreen::resized() {
    drop_textures();
    relayout();
}

void ReaderScreen::open_settings() {
    ReaderSettings* s = &app_.state.settings;
    auto m = std::make_unique<Menu>();
    m->title = "Reading";
    auto adjust = [&](const std::string& label, std::function<std::string()> value, std::function<void(int)> fn) {
        m->items.push_back({label, std::move(value), std::move(fn)});
    };
    m->items.push_back({"Contents", [this] { return std::to_string(book_.toc().size()); }, nullptr,
                        [this] { open_toc(); }});
    m->items.push_back({"Bookmarks",
                        [this] {
                            const BookState* st = app_.state.find(book_.path());
                            return std::to_string(st ? st->bookmarks.size() : 0);
                        },
                        nullptr, [this] { open_bookmarks(); }});
    adjust("Theme", [s] { return std::string(THEMES[s->theme].name); }, [this, s](int d) {
        s->theme = (s->theme + d + THEME_COUNT) % THEME_COUNT;
        settings_changed(false);
    });
    adjust("Progress line", [s] { return std::string(s->progress_line ? "On" : "Off"); }, [this, s](int) {
        s->progress_line = !s->progress_line;
        settings_changed(true);
    });
    adjust("Font", [s] { return std::string(FONTS[std::max(0, s->font_index())].name); }, [this, s](int d) {
        s->font = FONTS[(std::max(0, s->font_index()) + d + FONT_COUNT) % FONT_COUNT].id;
        settings_changed(true);
    });
    adjust("Font size", [s] { return std::to_string(s->font_size); }, [this, s](int d) {
        s->font_size += d;
        settings_changed(true);
    });
    adjust("Line spacing", [s] { return line_spacing_label(s->line_spacing); }, [this, s](int d) {
        int v = s->line_spacing;
        if (d > 0)
            v = v == 0 ? 11 : std::min(18, v + 1);
        else
            v = v <= 11 ? 0 : v - 1;
        s->line_spacing = v;
        settings_changed(true);
    });
    adjust("Paragraph spacing", [s] { return para_spacing_label(s->para_spacing); }, [this, s](int d) {
        s->para_spacing = std::clamp(s->para_spacing + d, 0, 3);
        settings_changed(true);
    });
    adjust("Side margins", [s] { return std::to_string(s->side_margin); }, [this, s](int d) {
        s->side_margin += 8 * d;
        settings_changed(true);
    });
    adjust("Top and bottom margins", [s] { return std::to_string(s->vert_margin); }, [this, s](int d) {
        s->vert_margin += 8 * d;
        settings_changed(true);
    });
    adjust("Alignment", [s] { return align_label(s->align); }, [this, s](int d) {
        s->align = (s->align + d + 3) % 3;
        settings_changed(true);
    });
    adjust("Two pages side by side", [s] { return std::string(s->pages == 2 ? "When they fit" : "Off"); },
           [this, s](int) {
               s->pages = s->pages == 2 ? 1 : 2;
               settings_changed(true);
           });
    adjust("Hyphenation", [s] { return std::string(s->hyphenation ? "On" : "Off"); }, [this, s](int) {
        s->hyphenation = !s->hyphenation;
        settings_changed(true);
    });
    adjust("Book's own styles", [s] { return std::string(s->book_styles ? "On" : "Off"); }, [this, s](int) {
        s->book_styles = !s->book_styles;
        settings_changed(true);
    });
    adjust("Text rendering", [s] { return text_aa_label(s->text_aa); }, [this, s](int d) {
        s->text_aa = (s->text_aa + d + TEXT_AA_COUNT) % TEXT_AA_COUNT;
        settings_changed(true);
    });
    if (app_.articles->is_article(book_.path()))
        m->items.push_back({"Mark as read in wallabag", nullptr, nullptr, [this] {
                                // Closes the article (its position is saved, then forgotten).
                                std::string path = book_.path();
                                app_.close_menus();
                                app_.show_library();
                                app_.articles->mark_read(path);
                            }});
    m->items.push_back({"About DeckReader", nullptr, nullptr, [this] { app_.open_about(); }});
    m->items.push_back({"Close book", nullptr, nullptr, [this] {
                            app_.close_menus();
                            app_.show_library();
                        }});
    app_.push_menu(std::move(m));
}

void ReaderScreen::open_toc() {
    auto m = std::make_unique<Menu>();
    m->title = "Contents";
    m->width = 640;
    m->empty_text = "This book has no table of contents.";
    // Same chapter as the HUD shows: the one the first visible page is in.
    int current = book_.chapter_at(book_.current_page());
    const std::vector<TocEntry>& toc = book_.toc();
    for (size_t i = 0; i < toc.size(); i++) {
        MenuItem it;
        it.label = toc[i].title;
        it.indent = std::min(toc[i].level, 3);
        int page = toc[i].page;
        it.value = [page] { return std::to_string(page + 1); };
        it.activate = [this, page] {
            app_.close_menus();
            show_page(page);
        };
        it.marked = static_cast<int>(i) == current;
        m->items.push_back(std::move(it));
    }
    m->select_marked();
    app_.push_menu(std::move(m));
}

void ReaderScreen::open_bookmarks() {
    auto m = std::make_unique<Menu>();
    m->title = "Bookmarks";
    m->width = 600;
    m->secondary_label = "Delete";
    m->empty_text = "No bookmarks yet. Press X while reading to add one.";
    const BookState* st = app_.state.find(book_.path());
    if (st) {
        for (const Bookmark& b : st->bookmarks) {
            MenuItem it;
            it.label = b.chapter.empty() ? "Bookmark" : b.chapter;
            int pct = b.percent;
            it.value = [pct] { return strf("%d%%", pct / 100); };
            std::string xp = b.xpointer;
            it.activate = [this, xp] {
                app_.close_menus();
                book_.go_to_position(xp);
                save_position();
                app_.redraw();
            };
            it.secondary = [this, xp] {
                BookState& s = app_.state.at(book_.path());
                s.bookmarks.erase(std::remove_if(s.bookmarks.begin(), s.bookmarks.end(),
                                                 [&](const Bookmark& x) { return x.xpointer == xp; }),
                                  s.bookmarks.end());
                app_.state.mark_dirty();
                refresh_bookmark_pages();
                int sel = app_.top_menu() ? app_.top_menu()->selected : 0;
                app_.pop_menu();
                open_bookmarks();
                if (Menu* nm = app_.top_menu())
                    nm->selected = std::clamp(sel, 0, std::max(0, static_cast<int>(nm->items.size()) - 1));
                app_.toast("Bookmark deleted");
            };
            m->items.push_back(std::move(it));
        }
    }
    app_.push_menu(std::move(m));
}

// --- input ----------------------------------------------------------------------

void ReaderScreen::show_hud(bool on) {
    hud_ = on;
    if (on) {
        battery_ = read_battery();
        clock_ = clock_text(app_.cfg.clock_24h);
        clock_due_ = app_.now() + static_cast<uint64_t>(ms_to_next_minute());
    }
    app_.redraw();
}

void ReaderScreen::handle(const InputEvent& e) {
    if (!book_.is_open())
        return;
    switch (e.action) {
    case Action::Confirm:
        show_hud(!hud_);
        break;
    case Action::Back:
        if (hud_)
            show_hud(false);
        else
            app_.show_library();
        break;
    case Action::Left:
    case Action::L1:
        turn(-1);
        break;
    case Action::Right:
    case Action::R1:
        turn(1);
        break;
    case Action::Up:
    case Action::L2:
        if (!e.repeat)
            chapter(-1);
        break;
    case Action::Down:
    case Action::R2:
        if (!e.repeat)
            chapter(1);
        break;
    case Action::X:
        toggle_bookmark();
        break;
    case Action::Y:
        cycle_theme();
        break;
    case Action::View:
        open_toc();
        break;
    case Action::Menu:
        open_settings();
        break;
    default:
        break;
    }
}

void ReaderScreen::tap(float fx, float fy) {
    (void)fy;
    if (!book_.is_open())
        return;
    if (app_.cfg.tap_zones && fx < 0.3f)
        turn(-1);
    else if (app_.cfg.tap_zones && fx > 0.7f)
        turn(1);
    else
        show_hud(!hud_);
}

void ReaderScreen::tick(uint64_t now) {
    if (!book_.is_open())
        return;
    if (relayout_at_ && now >= relayout_at_)
        relayout();
    if (hud_ && now >= clock_due_) {
        battery_ = read_battery();
        clock_ = clock_text(app_.cfg.clock_24h);
        clock_due_ = now + static_cast<uint64_t>(ms_to_next_minute());
        app_.redraw();
    }

    // Left stick: hold to scrub, speed grows with the deflection.
    float sx = app_.top_menu() ? 0.0f : app_.input.scrub_x();
    const int n = book_.page_count(), pps = book_.pages_per_screen();
    if (sx != 0.0f) {
        if (!scrubbing_) {
            scrubbing_ = true;
            // A flick turns one screen at once; holding keeps going.
            scrub_pos_ = std::clamp(book_.current_page() + (sx < 0 ? -pps : pps), 0, std::max(0, n - 1));
            scrub_last_ = now;
            scrub_shown_ = 0;
        }
        double dt = std::min(0.1, (now - scrub_last_) / 1000.0);
        scrub_last_ = now;
        double speed = 2.0 + 60.0 * sx * sx; // pages per second
        scrub_pos_ = std::clamp(scrub_pos_ + (sx < 0 ? -speed : speed) * dt, 0.0, std::max(0.0, n - 1.0));
        int target = static_cast<int>(scrub_pos_);
        target -= target % std::max(1, pps);
        if (target != scrub_page_) {
            scrub_page_ = target;
            app_.redraw();
        }
        // The page itself follows at a rate that keeps the APU relaxed.
        if (scrub_page_ != book_.current_page() && now - scrub_shown_ >= 120) {
            book_.go_to_page(scrub_page_);
            scrub_shown_ = now;
            app_.redraw();
        }
    } else if (scrubbing_) {
        scrubbing_ = false;
        show_page(scrub_page_);
    }
}

int64_t ReaderScreen::next_due(uint64_t now) const {
    if (!book_.is_open())
        return -1;
    int64_t best = -1;
    auto consider = [&](int64_t d) {
        if (d >= 0 && (best < 0 || d < best))
            best = d;
    };
    if (relayout_at_)
        consider(relayout_at_ > now ? static_cast<int64_t>(relayout_at_ - now) : 0);
    if (hud_)
        consider(clock_due_ > now ? static_cast<int64_t>(clock_due_ - now) : 0);
    if (scrubbing_ || app_.input.scrub_x() != 0.0f)
        consider(33);
    return best;
}

// --- drawing ----------------------------------------------------------------------

bool ReaderScreen::overlay_needed() const {
    return book_.is_open() && (hud_ || scrubbing_ || relayout_at_ || bookmark_on_screen());
}

void ReaderScreen::draw_overlay(Ui& ui) {
    if (!book_.is_open())
        return;
    if (bookmark_on_screen() && !hud_)
        ui.icon_bookmark(ui.W() - 60, 0, 22, 34, ui.col(ui.theme.accent));
    if (hud_)
        draw_hud(ui);
    else if (scrubbing_)
        draw_scrub(ui);
    if (relayout_at_) {
        const std::string msg = "Updating pages\xE2\x80\xA6";
        float w = ui.text_w(msg, 18) + 44, h = 44;
        float y = ui.H() - h - 24 - (hud_ ? 112 : 0);
        ui.rect(24, y, w, h, ui.col(ui.theme.panel2, 0.96f), h / 2);
        ui.text(24 + 22, y + h / 2, msg, 18, ui.col(ui.theme.text));
    }
}

void ReaderScreen::draw_scrub(Ui& ui) {
    const UiTheme& th = ui.theme;
    const int n = std::max(1, book_.page_count());
    float w = std::min(ui.W() - 80, 760.0f), x = (ui.W() - w) / 2, h = 104, y = ui.H() - h - 28;
    ui.rect(x, y, w, h, ui.col(0x000000, 0.86f), 16);
    const int pps = book_.pages_per_screen();
    const std::string pos = pps > 1 && scrub_page_ + 1 < n
                                ? strf("Pages %d\xE2\x80\x93%d of %d", scrub_page_ + 1, std::min(n, scrub_page_ + pps), n)
                                : strf("Page %d of %d", scrub_page_ + 1, n);
    ui.text(x + 28, y + 30, pos, 20, ui.col(th.text), true);
    // The same percentage as the status overlay and the progress line.
    const int percent = progress_at(scrub_page_).percent;
    ui.text(x + w - 28, y + 30, strf("%d%%", percent), 18, ui.col(th.subtext), false, ALIGN_RIGHT);
    ui.text(x + 28, y + 58, book_.chapter_title_at(scrub_page_), 16, ui.col(th.subtext), false, ALIGN_LEFT, w - 56);
    ui.progress(x + 28, y + 80, w - 56, 6, percent / 100.0f, ui.col(th.accent), ui.col(th.panel2));
}

void ReaderScreen::draw_hud(Ui& ui) {
    const UiTheme& th = ui.theme;
    const float W = ui.W(), H = ui.H();
    const int n = std::max(1, book_.page_count());
    const int cur = scrubbing_ ? scrub_page_ : book_.current_page();
    const int pps = book_.pages_per_screen();
    Color white = ui.col(th.text), sub = ui.col(th.subtext);

    // Top: book, chapter, clock and battery (Gamescope hides Steam's bar).
    ui.rect(0, 0, W, 86, ui.col(0x000000, 0.84f));
    float right = W - 40;
    float cw = ui.text(right, 32, clock_, 22, white, true, ALIGN_RIGHT);
    float info_left = right - cw;
    if (battery_.present) {
        float pw = ui.text(right - cw - 22, 32, strf("%d%%", battery_.percent), 17, sub, false, ALIGN_RIGHT);
        float bx = right - cw - 22 - pw - 10 - 30;
        ui.icon_battery(bx, 32, 15, battery_.percent, battery_.charging, sub);
        info_left = bx;
    }
    ui.text(40, 32, book_.info().title, 22, white, true, ALIGN_LEFT, info_left - 40 - 32);
    std::string author = book_.info().author;
    std::string chapter = book_.chapter_title_at(cur);
    ui.text(40, 62, chapter.empty() ? author : chapter, 16, sub, false, ALIGN_LEFT, W - 80);

    // Bottom: progress with chapter marks, position, hints.
    const float bh = 112, by = H - bh;
    ui.rect(0, by, W, bh, ui.col(0x000000, 0.84f));
    float px = 40, pw = W - 80, py = by + 28;
    ui.progress(px, py, pw, 6, static_cast<float>(std::min(n, cur + pps)) / n, ui.col(th.accent), ui.col(th.panel2));
    for (const TocEntry& e : book_.toc())
        if (e.level == 0 && e.page > 0)
            ui.rect(px + pw * e.page / n - 1, py, 2, 6, ui.col(0x000000, 0.9f));
    if (bookmark_on_screen())
        ui.icon_bookmark(W - 60, by - 36, 22, 34, ui.col(th.accent));

    std::string pos = pps > 1 && cur + 1 < n ? strf("Pages %d\xE2\x80\x93%d of %d", cur + 1, std::min(n, cur + pps), n)
                                             : strf("Page %d of %d", cur + 1, n);
    float tx = 40 + ui.text(40, by + 72, pos, 18, white, true);
    // Same counting as the progress line and the library (D24).
    const Progress prog = progress_at(cur);
    std::string extra = strf("%d%%", prog.percent);
    const std::string chapter_left = chapter_left_text(prog);
    if (!chapter_left.empty())
        extra += " \xC2\xB7 " + chapter_left;
    // No button hints under a drawer menu: they belong to reading, and the
    // drawer would cut them in half.
    std::vector<Hint> hints;
    if (!app_.top_menu())
        hints = {{Glyph::X, bookmark_on_screen() ? "Unmark" : "Bookmark"},
                 {Glyph::Y, "Theme"},
                 {Glyph::View, "Contents"},
                 {Glyph::Menu, "Settings"}};
    // Hints take the right side; the details get what is left, if useful.
    float hints_w = 0;
    for (const Hint& h : hints)
        hints_w += 26 + 10 + ui.text_w(h.label, 19) + 30;
    float room = W - 40 - hints_w - tx - 16;
    if (room >= 40)
        ui.text(tx + 16, by + 72, extra, 16, sub, false, ALIGN_LEFT, room);
    if (!hints.empty())
        ui.hints_inline(hints, W - 40, by + 72);
}

} // namespace dr
