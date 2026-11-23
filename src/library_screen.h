// The library: start screen, folders and recent books, drawn like
// DeckVideoPlayer's library (header, rows with a selection pill, hint
// bar) and driven with the same buttons (DeckVideoPlayer D10).
#pragma once

#include "input.h"
#include "library.h"
#include "ui.h"

#include <string>
#include <vector>

namespace dr {

class App;

class LibraryScreen {
public:
    explicit LibraryScreen(App& app);

    // Back to the start screen (rebuilt: recent books may have changed).
    void show_root();
    // Re-reads the current location (after returning from a book, Refresh).
    void refresh();
    bool in_articles(ArticleSource source) const { return !stack_.empty() && stack_.back().articles == source; }
    void handle(const InputEvent& e);
    void draw(Ui& ui);
    void tap(float x, float y); // units
    // Reads one missing book's metadata/cover. True if it did something.
    bool idle_work();
    bool has_idle_work() const;

private:
    struct Location {
        enum Type { Root, Folder } type = Root;
        ArticleSource articles = ArticleSource::None; // an articles folder (D31, D37)
        int64_t list = 0; // in it, one of its reading lists (D37)
        std::string path, title;
        std::vector<LibEntry> entries;
        int selected = 0;
        int scroll = 0;
    };
    void enter(const LibEntry& e, bool from_start);
    // Nearest row from i that is not a section header (dir first).
    int selectable(const Location& loc, int i, int dir) const;
    // Moves the cursor one selectable row (wrapping or not).
    void step(Location& loc, int dir, bool wrap);
    void list(Location& loc);
    // Book folders share one sort order; the Articles folder has its own.
    SortMode& sort_for(const Location& loc) { return loc.articles != ArticleSource::None ? article_sort_ : sort_; }
    void open_menu();
    void open_info(const LibEntry& e);
    int rows_visible(const Ui& ui) const;
    void keep_visible(int rows);
    std::string breadcrumb() const;
    void draw_row(Ui& ui, const LibEntry& e, float y, bool selected);

    App& app_;
    std::vector<Location> stack_;
    SortMode sort_ = SortMode::Name;
    SortMode article_sort_ = SortMode::Newest; // the reading list order
    int last_rows_ = 8;
    float list_top_ = 124;
    // Next index to check for missing metadata in the current location.
    size_t meta_cursor_ = 0;
    bool meta_pending_ = true;
    bool fetched_any_ = false; // re-sort when the folder's metadata is complete
};

} // namespace dr
