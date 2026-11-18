#include "library_screen.h"

#include "app.h"
#include "fsutil.h"
#include "util.h"

#include <algorithm>
#include <cmath>

namespace dr {

namespace {
constexpr float ROW_H = 72;
constexpr float LEFT = 48;
constexpr float TEXT_X = 104;
} // namespace

LibraryScreen::LibraryScreen(App& app) : app_(app) {
    show_root();
}

void LibraryScreen::show_root() {
    stack_.clear();
    Location root;
    root.type = Location::Root;
    root.title = "Library";
    stack_.push_back(root);
    list(stack_.back());
}

void LibraryScreen::list(Location& loc) {
    std::string keep;
    if (loc.selected >= 0 && loc.selected < static_cast<int>(loc.entries.size()))
        keep = loc.entries[static_cast<size_t>(loc.selected)].path;
    switch (loc.type) {
    case Location::Root:
        loc.entries = list_root(app_.cfg, app_.state, app_.mountinfo(), app_.paths.home);
        break;
    case Location::Recent:
        loc.entries = list_recent(app_.state, 200);
        break;
    case Location::Folder:
        loc.entries = list_folder(loc.path);
        sort_entries(loc.entries, sort_, app_.meta, app_.state);
        break;
    }
    loc.selected = 0;
    for (size_t i = 0; i < loc.entries.size(); i++)
        if (!keep.empty() && loc.entries[i].path == keep)
            loc.selected = static_cast<int>(i);
    meta_cursor_ = 0;
    meta_pending_ = true;
    fetched_any_ = false;
    app_.redraw();
}

void LibraryScreen::refresh() {
    list(stack_.back());
}

int LibraryScreen::rows_visible(const Ui& ui) const {
    return std::max(1, static_cast<int>((ui.H() - Ui::HINT_BAR_HEIGHT - 8 - list_top_) / ROW_H));
}

void LibraryScreen::keep_visible(int rows) {
    Location& loc = stack_.back();
    int n = static_cast<int>(loc.entries.size());
    if (loc.selected < loc.scroll)
        loc.scroll = loc.selected;
    if (loc.selected >= loc.scroll + rows)
        loc.scroll = loc.selected - rows + 1;
    loc.scroll = std::clamp(loc.scroll, 0, std::max(0, n - rows));
}

void LibraryScreen::enter(const LibEntry& e, bool from_start) {
    switch (e.kind) {
    case LibEntry::Book:
        app_.open_book(e.path, from_start);
        return;
    case LibEntry::Recent: {
        Location loc;
        loc.type = Location::Recent;
        loc.title = "Recent books";
        stack_.push_back(loc);
        list(stack_.back());
        return;
    }
    case LibEntry::Folder:
    case LibEntry::Source: {
        if (!is_dir(e.path)) {
            app_.toast("Folder not found: " + e.path);
            return;
        }
        Location loc;
        loc.type = Location::Folder;
        loc.path = e.path;
        loc.title = e.name;
        stack_.push_back(loc);
        list(stack_.back());
        return;
    }
    }
}

void LibraryScreen::handle(const InputEvent& e) {
    Location& loc = stack_.back();
    const int n = static_cast<int>(loc.entries.size());
    const int page = std::max(1, last_rows_ - 1);
    auto sel = [&]() -> const LibEntry* { return n ? &loc.entries[static_cast<size_t>(loc.selected)] : nullptr; };
    switch (e.action) {
    case Action::Up:
        if (n)
            loc.selected = e.repeat ? std::max(0, loc.selected - 1) : (loc.selected - 1 + n) % n;
        break;
    case Action::Down:
        if (n)
            loc.selected = e.repeat ? std::min(n - 1, loc.selected + 1) : (loc.selected + 1) % n;
        break;
    case Action::Left:
    case Action::L1:
        loc.selected = std::max(0, loc.selected - page);
        break;
    case Action::Right:
    case Action::R1:
        loc.selected = std::min(std::max(0, n - 1), loc.selected + page);
        break;
    case Action::L2:
        loc.selected = 0;
        break;
    case Action::R2:
        loc.selected = std::max(0, n - 1);
        break;
    case Action::Confirm:
        if (const LibEntry* s = sel())
            enter(*s, false);
        return;
    case Action::X:
        if (const LibEntry* s = sel(); s && s->kind == LibEntry::Book)
            enter(*s, true);
        return;
    case Action::Back:
        if (stack_.size() > 1) {
            stack_.pop_back();
            list(stack_.back()); // folders may have changed meanwhile
        } else {
            app_.quit();
        }
        return;
    case Action::Y:
        if (loc.type == Location::Folder) {
            sort_ = sort_ == SortMode::Name ? SortMode::Author : sort_ == SortMode::Author ? SortMode::Recent : SortMode::Name;
            sort_entries(loc.entries, sort_, app_.meta, app_.state);
            loc.selected = 0;
            loc.scroll = 0;
            app_.toast(sort_label(sort_));
        }
        break;
    case Action::View:
        if (const LibEntry* s = sel(); s && s->kind == LibEntry::Book)
            open_info(*s);
        return;
    case Action::Menu:
        open_menu();
        return;
    default:
        break;
    }
    app_.redraw();
}

void LibraryScreen::open_menu() {
    auto m = std::make_unique<Menu>();
    m->title = "DeckReader";
    m->items.push_back({"Refresh library", nullptr, nullptr, [this] {
                            app_.close_menus();
                            refresh();
                            app_.toast("Library refreshed");
                        }});
    m->items.push_back({"About DeckReader", nullptr, nullptr, [this] { app_.open_about(); }});
    m->items.push_back({"Quit", nullptr, nullptr, [this] { app_.quit(); }});
    app_.push_menu(std::move(m));
}

void LibraryScreen::open_info(const LibEntry& e) {
    auto m = std::make_unique<Menu>();
    const BookMeta* meta = app_.meta.get(e.path);
    m->title = display_title(e, meta);
    m->width = 560;
    auto row = [&](const std::string& label, const std::string& value) {
        if (!value.empty())
            m->items.push_back({label, [value] { return value; }});
    };
    row("Author", meta ? meta->author : "");
    row("Language", meta ? meta->language : "");
    uint64_t size = 0;
    file_stat(e.path, &size, nullptr);
    row("File", path_basename(e.path));
    row("Size", size >= (1u << 20) ? strf("%.1f MB", size / 1048576.0) : strf("%llu KB", static_cast<unsigned long long>(size >> 10)));
    if (const BookState* b = app_.state.find(e.path)) {
        if (b->finished)
            row("Progress", "Finished");
        else if (b->last_read)
            row("Progress", strf("%d%%", b->percent / 100));
        if (!b->bookmarks.empty())
            row("Bookmarks", std::to_string(b->bookmarks.size()));
    }
    row("Folder", path_dirname(e.path));
    std::string path = e.path;
    m->items.push_back({"Read", nullptr, nullptr, [this, path] {
                            app_.close_menus();
                            app_.open_book(path, false);
                        }});
    m->selected = static_cast<int>(m->items.size()) - 1;
    app_.push_menu(std::move(m));
}

std::string LibraryScreen::breadcrumb() const {
    std::string out;
    for (size_t i = 0; i < stack_.size(); i++) {
        if (i)
            out += "  \xE2\x80\xBA  "; // ›
        out += stack_[i].title;
    }
    return out;
}

bool LibraryScreen::has_idle_work() const {
    return meta_pending_;
}

bool LibraryScreen::idle_work() {
    if (!meta_pending_ || !app_.engine_ready()) {
        meta_pending_ = false;
        return false;
    }
    Location& loc = stack_.back();
    // Visible rows first, then the rest of the list (titles for sorting).
    for (int i = loc.scroll; i < loc.scroll + last_rows_ && i < static_cast<int>(loc.entries.size()); i++) {
        const LibEntry& e = loc.entries[static_cast<size_t>(i)];
        if (e.kind == LibEntry::Book && !app_.meta.get(e.path)) {
            app_.meta.fetch(e.path);
            fetched_any_ = true;
            app_.redraw();
            return true;
        }
    }
    while (meta_cursor_ < loc.entries.size()) {
        const LibEntry& e = loc.entries[meta_cursor_++];
        if (e.kind == LibEntry::Book && !app_.meta.get(e.path)) {
            app_.meta.fetch(e.path);
            fetched_any_ = true;
            app_.redraw();
            return true;
        }
    }
    meta_pending_ = false;
    app_.meta.save_if_dirty();
    // Titles and authors replaced file names: sort again, once, keeping the
    // cursor on the same book (D22).
    if (loc.type == Location::Folder && fetched_any_) {
        std::string keep = loc.entries.empty() ? std::string() : loc.entries[static_cast<size_t>(loc.selected)].path;
        sort_entries(loc.entries, sort_, app_.meta, app_.state);
        for (size_t i = 0; i < loc.entries.size(); i++)
            if (loc.entries[i].path == keep)
                loc.selected = static_cast<int>(i);
        app_.redraw();
    }
    fetched_any_ = false;
    return false;
}

void LibraryScreen::draw_row(Ui& ui, const LibEntry& e, float y, bool sel) {
    const UiTheme& th = ui.theme;
    float cy = y + ROW_H / 2;
    if (sel)
        ui.rect(28, y + 3, ui.W() - 56, ROW_H - 6, ui.col(th.accent), 12);
    Color fg = ui.col(th.text);
    Color sub = sel ? ui.col(th.text, 0.85f) : ui.col(th.subtext);
    Color icon = sel ? ui.col(th.text) : ui.col(th.subtext);
    float right = ui.W() - 48;

    if (e.kind != LibEntry::Book) {
        if (e.kind == LibEntry::Recent)
            ui.icon_recent(LEFT + 2, cy, 30, icon);
        else if (e.kind == LibEntry::Source && e.icon == LibEntry::IconCard)
            ui.icon_card(LEFT, cy, 32, icon);
        else if (e.kind == LibEntry::Source && e.icon == LibEntry::IconUsb)
            ui.icon_usb(LEFT, cy, 32, icon);
        else
            ui.icon_folder(LEFT, cy, 32, e.missing ? ui.col(th.dim) : icon);
        if (e.detail.empty()) {
            ui.text(TEXT_X, cy, e.name, 21, fg, sel, ALIGN_LEFT, right - TEXT_X);
        } else {
            ui.text(TEXT_X, cy - 11, e.name, 21, e.missing && !sel ? ui.col(th.subtext) : fg, sel, ALIGN_LEFT, right - TEXT_X);
            ui.text(TEXT_X, cy + 15, e.detail, 15, sub, false, ALIGN_LEFT, right - TEXT_X);
        }
        return;
    }

    const BookMeta* m = app_.meta.get(e.path);
    // Cover thumbnail, or a book icon until it is known.
    float ch = ROW_H - 16;
    const Image* cover = m && m->has_cover ? app_.meta.cover(e.path, static_cast<int>(std::lround(ch * ui.scale()))) : nullptr;
    if (cover) {
        float cw = std::min(cover->w / ui.scale(), 46.0f);
        float cx = LEFT + 20 - cw / 2;
        ui.rect(cx - 1, cy - ch / 2 - 1, cw + 2, ch + 2, ui.col(0x000000, 0.35f), 4);
        ui.clip(cx, cy - ch / 2, cw, ch);
        ui.image(*cover, cx, cy - ch / 2, cover->w / ui.scale(), ch);
        ui.unclip();
    } else {
        ui.icon_book(LEFT + 4, cy, 32, icon);
    }

    // Progress on the right, like DeckVideoPlayer's resume bars.
    const BookState* b = app_.state.find(e.path);
    float text_right = right;
    if (b && b->finished) {
        ui.icon_check(right - 12, cy, 12);
        text_right = right - 40;
    } else if (b && b->last_read && b->percent > 0) {
        float bw = 110;
        ui.progress(right - bw, cy - 3, bw, 6, b->percent / 10000.0f, sel ? ui.col(th.text) : ui.col(th.accent),
                    sel ? ui.col(th.text, 0.35f) : ui.col(th.panel2));
        float tw = ui.text(right - bw - 12, cy, strf("%d%%", b->percent / 100), 15, sub, false, ALIGN_RIGHT);
        text_right = right - bw - 12 - tw - 24;
    }

    std::string title = display_title(e, m);
    std::string second;
    if (e.continue_row)
        second = "Continue reading" + std::string(m && !m->author.empty() ? " \xC2\xB7 " + m->author : "");
    else if (m && !m->author.empty())
        second = m->author;
    else if (m && m->failed)
        second = "Not a readable EPUB";
    if (second.empty()) {
        ui.text(TEXT_X, cy, title, 21, fg, sel, ALIGN_LEFT, text_right - TEXT_X);
    } else {
        ui.text(TEXT_X, cy - 11, title, 21, fg, sel, ALIGN_LEFT, text_right - TEXT_X);
        ui.text(TEXT_X, cy + 15, second, 15, e.continue_row && !sel ? ui.col(th.accent) : sub, false, ALIGN_LEFT,
                text_right - TEXT_X);
    }
}

void LibraryScreen::draw(Ui& ui) {
    const UiTheme& th = ui.theme;
    Location& loc = stack_.back();

    // Header (DeckVideoPlayer layout).
    ui.text(40, 34, "DECK READER", 13, ui.col(th.accent), true);
    ui.text(40, 72, loc.title, 30, ui.col(th.text), true, ALIGN_LEFT, ui.W() - 360);
    ui.text(40, 102, breadcrumb(), 13, ui.col(th.subtext), false, ALIGN_LEFT, ui.W() - 360);
    int books = 0, folders = 0;
    for (const LibEntry& e : loc.entries)
        (e.kind == LibEntry::Book ? books : folders)++;
    if (loc.type != Location::Root) {
        std::string count = books == 1 ? "1 book" : strf("%d books", books);
        if (folders)
            count = (folders == 1 ? std::string("1 folder") : strf("%d folders", folders)) + " \xC2\xB7 " + count;
        ui.text(ui.W() - 40, 50, count, 16, ui.col(th.subtext), false, ALIGN_RIGHT);
        if (loc.type == Location::Folder)
            ui.text(ui.W() - 40, 78, sort_label(sort_), 13, ui.col(th.dim), false, ALIGN_RIGHT);
    } else {
        ui.text(ui.W() - 40, 50, "Steam Deck e-book reader", 16, ui.col(th.subtext), false, ALIGN_RIGHT);
    }

    int rows = rows_visible(ui);
    last_rows_ = rows;
    keep_visible(rows);
    if (loc.entries.empty()) {
        std::string msg = loc.type == Location::Recent ? "No books read yet."
                                                       : "No books here. Copy .epub files into this folder, "
                                                         "then choose Refresh in the menu.";
        float y = list_top_ + 40;
        for (const std::string& line : ui.wrap(msg, 20, false, ui.W() - 160, 3)) {
            ui.text(LEFT, y, line, 20, ui.col(th.subtext));
            y += 30;
        }
    }
    for (int r = 0; r < rows; r++) {
        int i = loc.scroll + r;
        if (i >= static_cast<int>(loc.entries.size()))
            break;
        draw_row(ui, loc.entries[static_cast<size_t>(i)], list_top_ + r * ROW_H, i == loc.selected);
    }
    int n = static_cast<int>(loc.entries.size());
    if (n > rows) {
        float track = rows * ROW_H;
        float bh = std::max(24.0f, track * rows / n);
        float by = list_top_ + (track - bh) * loc.scroll / std::max(1, n - rows);
        ui.rect(ui.W() - 12, by, 3, bh, ui.col(th.dim), 1.5f);
    }

    std::vector<Hint> hints;
    const LibEntry* s = n ? &loc.entries[static_cast<size_t>(loc.selected)] : nullptr;
    bool book = s && s->kind == LibEntry::Book;
    if (s)
        hints.push_back({Glyph::A, book ? "Read" : "Open"});
    if (book)
        hints.push_back({Glyph::X, "From start"});
    hints.push_back({Glyph::B, stack_.size() > 1 ? "Back" : "Quit"});
    if (loc.type == Location::Folder)
        hints.push_back({Glyph::Y, "Sort"});
    if (book)
        hints.push_back({Glyph::View, "Info"});
    hints.push_back({Glyph::Menu, "Menu"});
    ui.hint_bar(hints);
}

void LibraryScreen::tap(float x, float y) {
    (void)x;
    Location& loc = stack_.back();
    if (y < list_top_)
        return;
    int r = static_cast<int>((y - list_top_) / ROW_H);
    if (r < 0 || r >= last_rows_)
        return;
    int i = loc.scroll + r;
    if (i >= static_cast<int>(loc.entries.size()))
        return;
    loc.selected = i;
    app_.redraw();
    enter(loc.entries[static_cast<size_t>(i)], false);
}

} // namespace dr
