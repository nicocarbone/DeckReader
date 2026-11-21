// wallabag client (D31): the unread articles of a wallabag account
// (wallabag.it or a server of one's own) downloaded as EPUBs into the app's
// Articles folder, and articles marked as read on the Deck archived there.
//
// API (wallabag 2.x): an OAuth2 token from POST /oauth/v2/token (password
// grant), GET /api/entries.json?archive=0 for the unread list (in pages),
// GET /api/entries/{id}.json for each article (its HTML and details),
// PATCH /api/entries/{id}.json archive=1 to mark one read. The EPUB is
// made here (epubwrite.h), with the pictures downloaded from their sites:
// wallabag's own export leaves them as links (seen on wallabag.it 2.6.17).
//
// A sync runs in one background thread (SyncWorker); it never touches the
// app's state: it gets everything it needs in a SyncRequest and reports a
// SyncResult that the main thread applies (articles.cpp).
#pragma once

#include "config.h"
#include "epubwrite.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dr {

struct WallabagEntry {
    int64_t id = 0;
    std::string title;
    int64_t created = 0; // saved in wallabag, seconds since the epoch (0 = unknown)
};

// One article, as GET /api/entries/{id}.json gives it.
struct WallabagArticle {
    int64_t id = 0;
    std::string title, url, domain, language, content, preview_picture;
    std::vector<std::string> authors;
    int64_t published = 0, created = 0; // seconds; 0 = unknown
    int reading_time = 0;                // minutes
};

// --- pure helpers (unit-tested) ---

// The server address without trailing slashes; "" if it isn't http(s).
std::string wallabag_base(const std::string& server);
// "2026-11-19T10:20:30+0100", "...+01:00", "...Z" -> seconds; 0 if not a date.
int64_t parse_iso8601(const std::string& s);
// The access token from the token answer; "" if there is none, or if it
// holds anything but printable ASCII (it goes into a header).
std::string parse_token(const std::string& body);
// Why a token request failed, for the user.
std::string token_error(long status, const std::string& body, const std::string& location);
// One page of the unread list, and how many pages there are. False if the
// answer isn't one.
bool parse_entries(const std::string& body, std::vector<WallabagEntry>& out, int* pages = nullptr);
// One article. False if the answer isn't one.
bool parse_article(const std::string& body, WallabagArticle& out);
// The book for an article, without pictures yet: title, author (its
// authors, else its site), language, the title line under the heading
// ("Ana Pérez · example.com · 27 Mar 2009 · 2 min read") and the HTML.
ArticleBook article_book(const WallabagArticle& a);
// Articles are stored as "<id>.epub".
std::string article_file_name(int64_t id);
// The id of an article file name; 0 if the name isn't one.
int64_t article_id(const std::string& name);

struct SyncPlan {
    std::vector<WallabagEntry> download; // unread, not on the Deck yet
    std::vector<int64_t> remove;         // on the Deck, no longer unread
};
// Articles waiting to be archived are neither downloaded again nor kept.
SyncPlan plan_sync(const std::vector<WallabagEntry>& unread, const std::vector<int64_t>& local,
                   const std::vector<int64_t>& archive);

// --- a sync ---

struct SyncRequest {
    WallabagSettings settings;
    std::string dir;              // the Articles folder
    std::vector<int64_t> archive; // marked read on the Deck, to archive first
    std::vector<int64_t> local;   // articles already in dir
    bool update = true;           // false: only archive
};

struct SyncProgress {
    enum Phase { Starting, Archiving, Listing, Downloading } phase = Starting;
    int done = 0, total = 0;
};

struct SyncResult {
    std::string error;             // "" when everything worked
    bool cancelled = false;
    std::vector<int64_t> archived; // archived on the server (or gone there)
    bool listed = false;           // the unread list came: `remove` is valid
    std::vector<int64_t> remove;   // local articles no longer unread
    int unread = 0;                // in the list (at most max_articles)
    int downloaded = 0, failed = 0;
    bool update = true;
};

// The whole sync, in the calling thread. Writes new articles into req.dir
// (atomically, as "<id>.epub", dated when they were saved); removes nothing.
SyncResult run_sync(const SyncRequest& req, const std::atomic<bool>& cancel,
                    const std::function<void(const SyncProgress&)>& progress);

// One sync at a time in a background thread. `wake` is called from that
// thread on progress and when it is done (the app pushes an SDL event).
class SyncWorker {
public:
    ~SyncWorker(); // cancels and waits
    bool start(SyncRequest req, std::function<void()> wake);
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

} // namespace dr
