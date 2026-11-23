// Wikipedia reading lists (D37): the articles saved in the Wikipedia apps
// (the "Saved" list and the user's other lists), downloaded as EPUBs into
// the app's Wikipedia folder, like the wallabag articles (D31).
//
// API: the ReadingLists extension's REST routes on meta.wikimedia.org
// (GET .../readinglists/v0/lists and .../lists/{id}/entries, in pages),
// with a personal OAuth 2 token: an owner-only consumer with only the
// "Access private information" grant, so it can read the lists and change
// nothing. Each article's HTML comes from its wiki's REST API (Parsoid's
// HTML), without the token, and is cleaned for a book: text, tables,
// figures, formulas (MathML) and references (marked as notes, for D35);
// no navigation boxes, editor data or links to other pages.
#pragma once

#include "articlesync.h"
#include "config.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace dr {

struct WikiList {
    int64_t id = 0;
    std::string name;
    bool is_default = false; // the apps' "Saved"
};

struct WikiEntry {
    int64_t id = 0, list = 0;
    std::string project; // "https://en.wikipedia.org"
    std::string title;   // with spaces
    int64_t created = 0; // saved, seconds since the epoch
};

// An article to have on the Deck: id (from wiki_article_id), title, when it
// was first saved, and its wiki.
struct WikiArticle : ArticleEntry {
    std::string host; // "en.wikipedia.org"
};

// --- pure helpers (unit-tested) ---

// One page of lists or of a list's entries, and the token for the next
// page ("" on the last). False if the answer isn't one.
bool parse_wiki_lists(const std::string& body, std::vector<WikiList>& out, std::string* next);
bool parse_wiki_entries(const std::string& body, std::vector<WikiEntry>& out, std::string* next);
// Why the reading-list API didn't answer, for the user.
std::string wiki_error(long status, const std::string& body);
// "https://es.wikipedia.org" -> "es.wikipedia.org": only Wikipedia's
// language sites over https; "" for anything else.
std::string wiki_host(const std::string& project);
// Whether a list is one of `names` (case-insensitive; "Saved" and "default"
// also name the default list). No names: every list.
bool wiki_list_wanted(const WikiList& list, const std::vector<std::string>& names);
// A stable id for a page (its file name): 1 .. 2^53-1.
int64_t wiki_article_id(const std::string& host, const std::string& title);
// What the Deck keeps: the newest `max_per_list` articles of each list
// (each page once, dated when it was first saved), and each list's own,
// shown as a folder. Lists keep the given order, without the empty ones;
// the default one is "Saved", as in the apps. Entries not on a Wikipedia
// site are left out and counted in `skipped`.
struct WikiShelf {
    std::vector<WikiArticle> articles; // each page once, newest first
    std::vector<ArticleList> lists;
};
WikiShelf wiki_choose(const std::vector<WikiList>& lists, const std::vector<WikiEntry>& entries, int max_per_list,
                      int* skipped = nullptr);
// "Euler's identity" -> "Euler%27s_identity" (for addresses).
std::string wiki_title_path(const std::string& title);

// An article's page, cleaned for the book.
struct WikiPage {
    std::string title;    // the page's own title ("" if it has none)
    std::string language; // "en"
    int64_t modified = 0; // the revision's date
    std::string html;     // the body, cleaned; pictures still at their addresses
    std::vector<ArticleSection> toc; // h2 and h3 headings
};
// Parsoid's HTML of a page made into the book's. False if it isn't a page.
bool wiki_clean_page(const std::string& parsoid, WikiPage& out);
// The book for an article, without pictures yet.
ArticleBook wiki_book(const WikiArticle& a, const WikiPage& p);

// What the article EPUBs hold, by version (like wallabag.h's ARTICLE_FORMAT).
constexpr int WIKI_FORMAT = 1;

// --- a sync ---

struct WikiSyncRequest {
    WikipediaSettings settings;
    std::string dir;            // the Wikipedia folder
    std::vector<int64_t> local; // articles already in dir
    bool rebuild = false;       // download again the ones made by an older version
    // Where the lists come from, and the pages: "" = each article's own
    // wiki over https. Tests point both at a fake server.
    std::string lists_api = "https://meta.wikimedia.org/w/rest.php/readinglists/v0";
    std::string pages_server;
};

// The whole sync, in the calling thread (a SyncWorker's job). Writes new
// articles into req.dir; removes nothing.
SyncResult run_wiki_sync(const WikiSyncRequest& req, const std::atomic<bool>& cancel, const SyncProgressFn& progress);

} // namespace dr
