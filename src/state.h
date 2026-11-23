// Everything DeckReader remembers between runs, in one JSON file in the data
// folder ($XDG_DATA_HOME/deckreader/state.json): the reading position and
// bookmarks of every book, and the reading settings (D6, D11).
#pragma once

#include "settings.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dr {

struct Bookmark {
    std::string xpointer; // crengine position (survives font/size changes)
    int percent = 0;      // 0..10000, for sorting and display
    std::string chapter;
    int64_t time = 0;
};

struct BookState {
    std::string xpointer;  // position of the first word on screen
    int percent = 0;       // 0..10000
    int page = 0;          // as last laid out (display only; layouts differ)
    int pages = 0;
    bool finished = false; // reached the last page
    int64_t last_read = 0; // wall time in ms (books opened within a second keep their order); 0 = never
    bool hidden = false;   // removed from the start screen's recent books
    std::string title, author;
    std::vector<Bookmark> bookmarks;
};

class StateStore {
public:
    static constexpr size_t MAX_BOOKS = 1000;
    static constexpr uint64_t SAVE_INTERVAL_MS = 2000;

    explicit StateStore(std::string path = {}) : path_(std::move(path)) {}

    // A missing file gives an empty store. A file that cannot be parsed is
    // renamed to state.json.corrupt; one that exists but cannot be read
    // turns saving off for this run. Never silently overwritten.
    bool load();
    bool save();

    std::string to_json() const;
    bool from_json(const std::string& text);

    BookState* find(const std::string& book);
    BookState& at(const std::string& book);
    // Its position, bookmarks and place in the recent list (a removed article).
    void forget(const std::string& book);
    // "Mark as read" / "Mark as unread" in the library (D34, as
    // DeckVideoPlayer's D42). Both clear the position: a book marked as
    // read is finished (✓) and opens at its last page; one marked as
    // unread starts again at 0%. Bookmarks and the place among the recent
    // books stay.
    void mark_read(const std::string& book, bool read);
    // Which of the two the library offers: a partly read book both.
    static bool can_mark_read(const BookState* b) { return !b || !b->finished; }
    // (A few pages into a long book are still 0%: the page counts too.)
    static bool can_mark_unread(const BookState* b) { return b && (b->finished || b->percent > 0 || b->page > 0); }
    // Books opened before, most recent first (not the hidden ones).
    std::vector<std::string> recent(size_t max) const;

    ReaderSettings settings;

    // wallabag (D31): articles marked as read on the Deck that wallabag
    // hasn't been told about yet, and when the articles were last updated
    // (seconds since the epoch, 0 = never).
    std::vector<int64_t> wallabag_archive;
    int64_t wallabag_updated = 0;
    // The format of the article EPUBs on the Deck (wallabag.h,
    // ARTICLE_FORMAT; 0 = before 0.3.2): older ones are downloaded again.
    int wallabag_format = 0;

    // Saving is rate-limited: the first change after a quiet period is
    // written at once, further ones at most every SAVE_INTERVAL_MS, so
    // flipping through pages doesn't write the file dozens of times.
    void mark_dirty() { dirty_ = true; }
    bool dirty() const { return dirty_; }
    // Milliseconds until a pending save is due; -1 when nothing is pending.
    int64_t save_due_in(uint64_t now_ms) const;
    void save_if_due(uint64_t now_ms);

private:
    std::string path_;
    std::map<std::string, BookState> books_;
    bool dirty_ = false;
    bool writable_ = true;
    int failures_ = 0; // consecutive failed saves (back-off)
    uint64_t last_save_ms_ = 0;
    bool saved_once_ = false;
};

} // namespace dr
