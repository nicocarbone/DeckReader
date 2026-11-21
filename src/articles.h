// The Articles source (D31): the main-thread side of the wallabag sync.
// Starts syncs ("Update articles", "Mark as read in wallabag"), applies
// their results (state, removed articles, messages) and says what the
// library shows on the Articles row. The network work itself runs in
// wallabag.cpp's thread, which wakes the main loop with an SDL event (D1).
#pragma once

#include "wallabag.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dr {

class App;

class Articles {
public:
    // After SDL is initialised (registers the wake-up event).
    explicit Articles(App& app);

    bool configured() const;
    const std::string& dir() const { return dir_; }
    // A file in the Articles folder named like an article.
    bool is_article(const std::string& path) const;
    bool busy() const { return worker_.busy(); }
    uint32_t event_type() const { return event_; }
    int count() const { return count_; }

    // Archive what was marked as read, then download the unread articles
    // and remove the ones that aren't unread any more.
    void update();
    // Removes the article from the Deck now; wallabag is told at once, or
    // with the next update if that fails.
    void mark_read(const std::string& path);
    // Stops the update (from the menu); what was downloaded stays.
    void stop();
    // The sync thread reported progress or finished.
    void on_wake();

    // Second line of the Articles row: progress, the last error, or how
    // many articles there are and when they were updated.
    std::string status() const;
    // For the About screen.
    std::string about() const;

private:
    void start(bool update);
    void recount();
    std::vector<int64_t> local_ids() const;

    App& app_;
    std::string dir_;
    uint32_t event_ = 0;
    SyncWorker worker_;
    int count_ = 0;
    std::string last_error_;
    bool archive_again_ = false; // marked as read while a sync was running
    int shown_done_ = 0;         // downloads the library has been refreshed for
    bool stopping_ = false;      // stop() was asked, the thread is finishing
};

// "today 10:42", "yesterday 9:05 PM", "18 Nov" (for "updated ...").
std::string when_text(int64_t t, int64_t now, bool clock_24h);

} // namespace dr
