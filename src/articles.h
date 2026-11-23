// An articles source in the library: wallabag's unread articles (D31) or
// the articles saved in Wikipedia's reading lists (D37), one instance each.
// The main-thread side of their sync: starts it ("Update articles", "Mark
// as read in wallabag"), applies its result (state, removed articles,
// messages) and says what the library shows on the source's row. The
// network work runs in a background thread (articlesync.h), which wakes the
// main loop with an SDL event (D1).
#pragma once

#include "library.h"
#include "wallabag.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dr {

class App;

class Articles {
public:
    // After SDL is initialised (registers the wake-up event).
    Articles(App& app, ArticleSource source);

    ArticleSource source() const { return source_; }
    bool configured() const;
    const std::string& dir() const { return dir_; }
    // The library row: "Articles" or "Wikipedia", and the tag of its books.
    ArticleRoot root() const;
    // A file in this source's folder named like an article.
    bool is_article(const std::string& path) const;
    // Its lists, shown as folders (Wikipedia's, D37); none for wallabag.
    std::vector<ArticleList> lists() const;
    bool busy() const { return worker_.busy(); }
    uint32_t event_type() const { return event_; }
    int count() const { return count_; }

    // Downloads what is new and removes what left the list (wallabag:
    // archives what was marked as read first). False if it isn't set up or
    // is already updating (App::update_articles says so).
    bool update();
    // wallabag: removes the article from the Deck now; wallabag is told at
    // once, or with the next update if that fails.
    void mark_read(const std::string& path);
    // Stops the update (from the menu); what was downloaded stays.
    void stop();
    // The sync thread reported progress or finished.
    void on_wake();

    // Second line of the source's row: progress, the last error, or how
    // many articles there are and when they were updated.
    std::string status() const;
    // What an empty folder says.
    std::string empty_message() const;
    // For the About screen.
    std::string about() const;

private:
    void start(bool update);
    void recount();
    std::vector<int64_t> local_ids() const;
    int64_t& updated();
    int64_t updated() const;

    App& app_;
    ArticleSource source_;
    std::string dir_;
    uint32_t event_ = 0;
    SyncWorker worker_;
    int count_ = 0;
    std::string last_error_;
    bool archive_again_ = false; // marked as read while a sync was running (wallabag)
    int shown_done_ = 0;         // downloads the library has been refreshed for
    bool stopping_ = false;      // stop() was asked, the thread is finishing
};

// "today 10:42", "yesterday 9:05 PM", "18 Nov" (for "updated ...").
std::string when_text(int64_t t, int64_t now, bool clock_24h);

} // namespace dr
