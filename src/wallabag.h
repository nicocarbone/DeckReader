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
// The sync thread, the plan, the pictures and saving are shared with the
// Wikipedia articles (articlesync.h).
#pragma once

#include "articlesync.h"
#include "config.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace dr {

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
bool parse_entries(const std::string& body, std::vector<ArticleEntry>& out, int* pages = nullptr);
// One article. False if the answer isn't one.
bool parse_article(const std::string& body, WallabagArticle& out);
// The book for an article, without pictures yet: title, author (its
// authors, else its site), language, the title line under the heading
// ("Ana Pérez · example.com · 27 Mar 2009 · 2 min read") and the HTML.
ArticleBook article_book(const WallabagArticle& a);
// What the article EPUBs hold, by version: 1 = text and pictures (0.3.0),
// 2 = math as MathML (0.3.2, D36). Articles made with an older format are
// looked at again at the next update (state.json keeps the format): their
// text comes again, and those with TeX are made again, pictures included.
constexpr int ARTICLE_FORMAT = 2;
// Time to convert one article's formulas; the rest stay as TeX (D36).
constexpr uint64_t ARTICLE_MATH_MS = 20000;

// --- a sync ---

struct SyncRequest {
    WallabagSettings settings;
    std::string dir;              // the Articles folder
    std::vector<int64_t> archive; // marked read on the Deck, to archive first
    std::vector<int64_t> local;   // articles already in dir
    bool update = true;           // false: only archive
    bool rebuild = false;         // look at the local unread articles again (ARTICLE_FORMAT)
    std::string temml_js;         // temml.min.js: TeX becomes MathML (D36); "" = no math
};

// The whole sync, in the calling thread (a SyncWorker's job). Writes new
// articles into req.dir (atomically, as "<id>.epub", dated when they were
// saved); removes nothing.
SyncResult run_sync(const SyncRequest& req, const std::atomic<bool>& cancel, const SyncProgressFn& progress);

} // namespace dr
