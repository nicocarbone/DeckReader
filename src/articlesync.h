// What the article sources share (D31, D37): the list entries and the sync
// plan, the pictures, saving the book, and the background thread. wallabag.h
// and wikipedia.h each get their list of articles and each article's HTML;
// the rest is here.
//
// A sync runs in one background thread per source (SyncWorker); it never
// touches the app's state: it gets everything it needs when it starts and
// reports a SyncResult that the main thread applies (articles.cpp).
#pragma once

#include "epubwrite.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dr {

class HttpClient;

// One article in a source's list.
struct ArticleEntry {
    int64_t id = 0; // its file is "<id>.epub"
    std::string title;
    int64_t created = 0; // saved, seconds since the epoch (0 = unknown)
};

// Articles are stored as "<id>.epub".
std::string article_file_name(int64_t id);
// The id of an article file name; 0 if the name isn't one.
int64_t article_id(const std::string& name);

// A named group of a source's articles, shown as a folder in it
// (Wikipedia's reading lists, D37). An article may be in several.
struct ArticleList {
    int64_t id = 0;
    std::string name;
    std::vector<int64_t> articles; // newest first
};
// The source's lists, kept as lists.json in its folder: written by the
// sync, read by the library. False if the text isn't one.
constexpr const char* ARTICLE_LISTS_FILE = "lists.json";
std::string article_lists_json(const std::vector<ArticleList>& lists);
bool parse_article_lists(const std::string& text, std::vector<ArticleList>& out);

struct SyncPlan {
    std::vector<ArticleEntry> download; // wanted, not on the Deck yet (or all, to rebuild)
    std::vector<int64_t> remove;        // on the Deck, not wanted any more
};
// `wanted`: the list, in order. Ids in `skip` (waiting to be archived) are
// neither downloaded again nor kept. `rebuild`: the wanted articles already
// on the Deck are downloaded again (made by an older version).
SyncPlan plan_sync(const std::vector<ArticleEntry>& wanted, const std::vector<int64_t>& local,
                   const std::vector<int64_t>& skip, bool rebuild = false);

struct SyncProgress {
    enum Phase { Starting, Archiving, Listing, Downloading } phase = Starting;
    int done = 0, total = 0;
};

struct SyncResult {
    std::string error;             // "" when everything worked
    bool cancelled = false;
    std::vector<int64_t> archived; // archived on the server, or gone there (wallabag)
    bool listed = false;           // the list came: `remove` is valid
    std::vector<int64_t> remove;   // local articles no longer in the list
    int unread = 0;                // in the list (at most max_articles)
    int downloaded = 0, failed = 0;
    int rebuilt = 0;               // local articles made again (rebuild)
    // A rebuild didn't reach every article (one failed; no math engine):
    // the new format isn't recorded, so the next update tries again
    // (V4-BUG-3, D38).
    bool redo = false;
    bool update = true;            // false: only archived
};

using SyncProgressFn = std::function<void(const SyncProgress&)>;
// A whole sync, run in the worker's thread.
using SyncJob = std::function<SyncResult(const std::atomic<bool>& cancel, const SyncProgressFn& progress)>;

// One sync at a time in a background thread. `wake` is called from that
// thread on progress and when it is done (the app pushes an SDL event).
class SyncWorker {
public:
    ~SyncWorker(); // cancels and waits
    bool start(SyncJob job, std::function<void()> wake);
    bool busy() const;
    SyncProgress progress() const;
    // The result, once, after the thread finished. False until then.
    bool take_result(SyncResult& out);
    void cancel() { cancel_ = true; }

private:
    std::thread thread_;
    mutable std::mutex mu_;
    bool running_ = false;  // started, result not taken yet
    bool finished_ = false; // the thread is done
    SyncProgress progress_;
    SyncResult result_;
    std::atomic<bool> cancel_{false};
};

// Downloads the pictures of `html` (a page at `base_url`), the cover first
// when there is one, into the book, and puts the HTML in book.html with the
// pictures pointing at them (rewrite_html). A picture that can't be had is
// left out. Pictures come only from the internet (D31), except from
// `trusted_host` (wallabag's own server; a test server). False only when
// cancelled.
bool add_pictures(HttpClient& http, const std::string& html, const std::string& base_url,
                  const std::string& cover_url, const std::string& trusted_host, ArticleBook& book,
                  const std::atomic<bool>& cancel, int* left_out);

// Writes the book as dir/<id>.epub (atomically, D6), dated `created` (when
// it was saved: "Sorted by newest" uses it). `bytes`: the EPUB's size.
bool save_article(const std::string& dir, int64_t id, int64_t created, ArticleBook book, size_t* bytes,
                  std::string* err);

} // namespace dr
