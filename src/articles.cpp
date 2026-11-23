#include "articles.h"

#include "app.h"
#include "fsutil.h"
#include "http.h"
#include "log.h"
#include "util.h"
#include "wikipedia.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <unistd.h>

namespace dr {

std::string when_text(int64_t t, int64_t now, bool clock_24h) {
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t tt = static_cast<time_t>(t), nn = static_cast<time_t>(now);
    std::tm a{}, b{};
    localtime_r(&tt, &a);
    localtime_r(&nn, &b);
    auto clock = [&] {
        if (clock_24h)
            return strf("%02d:%02d", a.tm_hour, a.tm_min); // as the reading clock
        int h = a.tm_hour % 12 == 0 ? 12 : a.tm_hour % 12;
        return strf("%d:%02d %s", h, a.tm_min, a.tm_hour < 12 ? "AM" : "PM");
    };
    // Calendar days between them (midday avoids daylight-saving edges).
    std::tm da = a, db = b;
    da.tm_hour = db.tm_hour = 12;
    da.tm_min = db.tm_min = da.tm_sec = db.tm_sec = 0;
    da.tm_isdst = db.tm_isdst = -1;
    long days = std::lround(std::difftime(std::mktime(&db), std::mktime(&da)) / 86400.0);
    if (days == 0)
        return "today " + clock();
    if (days == 1)
        return "yesterday " + clock();
    if (a.tm_year == b.tm_year)
        return strf("%d %s", a.tm_mday, months[a.tm_mon]);
    return strf("%d %s %d", a.tm_mday, months[a.tm_mon], a.tm_year + 1900);
}

Articles::Articles(App& app, ArticleSource source) : app_(app), source_(source) {
    dir_ = path_join(app_.paths.data_dir, source_ == ArticleSource::Wikipedia ? "wikipedia" : "articles");
    Uint32 ev = SDL_RegisterEvents(1);
    event_ = ev ? ev : static_cast<uint32_t>(SDL_EVENT_USER);
    if (configured()) {
        make_dirs(dir_);
        recount();
    }
}

bool Articles::configured() const {
    return source_ == ArticleSource::Wikipedia ? app_.cfg.wikipedia.configured() : app_.cfg.wallabag.configured();
}

ArticleRoot Articles::root() const {
    if (source_ == ArticleSource::Wikipedia)
        return {source_, dir_, "Wikipedia", "Wikipedia"};
    return {source_, dir_, "Articles", "Article"};
}

int64_t& Articles::updated() {
    return source_ == ArticleSource::Wikipedia ? app_.state.wikipedia_updated : app_.state.wallabag_updated;
}

int64_t Articles::updated() const {
    return source_ == ArticleSource::Wikipedia ? app_.state.wikipedia_updated : app_.state.wallabag_updated;
}

bool Articles::is_article(const std::string& path) const {
    return configured() && path_dirname(path) == dir_ && article_id(path_basename(path)) > 0;
}

std::vector<ArticleList> Articles::lists() const {
    std::vector<ArticleList> out;
    std::string text;
    if (read_file(path_join(dir_, ARTICLE_LISTS_FILE), text, 4u << 20))
        parse_article_lists(text, out);
    return out;
}

std::vector<int64_t> Articles::local_ids() const {
    std::vector<int64_t> ids;
    std::vector<DirEntry> entries;
    list_dir(dir_, entries);
    for (const DirEntry& d : entries)
        if (!d.is_dir)
            if (int64_t id = article_id(d.name))
                ids.push_back(id);
    return ids;
}

void Articles::recount() {
    count_ = static_cast<int>(local_ids().size());
}

void Articles::start(bool update) {
    make_dirs(dir_);
    SyncJob job;
    if (source_ == ArticleSource::Wikipedia) {
        WikiSyncRequest req;
        req.settings = app_.cfg.wikipedia;
        req.dir = dir_;
        req.local = local_ids();
        req.rebuild = app_.state.wikipedia_format < WIKI_FORMAT;
        job = [req = std::move(req)](const std::atomic<bool>& cancel, const SyncProgressFn& progress) {
            return run_wiki_sync(req, cancel, progress);
        };
    } else {
        SyncRequest req;
        req.settings = app_.cfg.wallabag;
        req.dir = dir_;
        req.archive = app_.state.wallabag_archive;
        req.local = local_ids();
        req.update = update;
        // Articles made by an older version come again (math, D36).
        req.rebuild = update && app_.state.wallabag_format < ARTICLE_FORMAT;
        req.temml_js = path_join(app_.paths.app_data, "js/temml.min.js");
        job = [req = std::move(req)](const std::atomic<bool>& cancel, const SyncProgressFn& progress) {
            return run_sync(req, cancel, progress);
        };
    }
    const uint32_t ev = event_;
    worker_.start(std::move(job), [ev] {
        SDL_Event e{};
        e.type = ev;
        SDL_PushEvent(&e); // thread-safe; wakes SDL_WaitEvent
    });
    LOG_I("%s: %s", source_ == ArticleSource::Wikipedia ? "wikipedia" : "wallabag",
          update ? "updating articles" : "marking articles as read");
    app_.redraw();
}

bool Articles::update() {
    if (!configured() || busy())
        return false;
    start(true);
    return true;
}

void Articles::mark_read(const std::string& path) {
    if (source_ != ArticleSource::Wallabag || !is_article(path))
        return;
    const int64_t id = article_id(path_basename(path));
    std::vector<int64_t>& pending = app_.state.wallabag_archive;
    if (std::find(pending.begin(), pending.end(), id) == pending.end())
        pending.push_back(id);
    ::unlink(path.c_str());
    app_.state.forget(path);
    app_.meta.forget(path);
    app_.state.mark_dirty();
    recount();
    if (busy())
        archive_again_ = true;
    else
        start(false);
    app_.toast("Marked as read");
    app_.refresh_library();
}

void Articles::stop() {
    if (!busy())
        return;
    worker_.cancel();
    stopping_ = true;
    app_.redraw();
}

void Articles::on_wake() {
    app_.redraw(); // progress on the source's row
    SyncResult r;
    if (!worker_.take_result(r)) {
        // New articles appear in the folder as they arrive.
        SyncProgress p = worker_.progress();
        if (p.phase == SyncProgress::Downloading && p.done > shown_done_ && app_.showing_articles(source_)) {
            shown_done_ = p.done;
            app_.refresh_library();
        }
        return;
    }
    stopping_ = false;
    shown_done_ = 0;
    const bool wiki = source_ == ArticleSource::Wikipedia;
    // Only wallabag has articles waiting to be archived.
    std::vector<int64_t> none;
    std::vector<int64_t>& pending = wiki ? none : app_.state.wallabag_archive;
    for (int64_t id : r.archived)
        pending.erase(std::remove(pending.begin(), pending.end(), id), pending.end());
    if (!r.archived.empty())
        app_.state.mark_dirty();
    if (r.listed) {
        const std::string open = app_.open_book_path();
        for (int64_t id : r.remove) {
            std::string path = path_join(dir_, article_file_name(id));
            if (path == open)
                continue; // removed next time
            ::unlink(path.c_str());
            app_.state.forget(path);
            app_.meta.forget(path);
        }
        app_.state.mark_dirty();
    }
    // "Updated" means the whole update worked.
    if (r.update && r.listed && r.error.empty() && !r.cancelled) {
        updated() = wall_time();
        // The rebuild is done, unless it left some articles in the old
        // format: then again next time (V4-BUG-3, D38).
        if (!r.redo && wiki)
            app_.state.wikipedia_format = WIKI_FORMAT;
        else if (!r.redo)
            app_.state.wallabag_format = ARTICLE_FORMAT;
        app_.state.mark_dirty();
    }
    recount();
    LOG_I("%s: %s%s; %d archived, %d listed, %d downloaded, %d rebuilt, %d failed, %zu removed",
          wiki ? "wikipedia" : "wallabag", r.update ? "update" : "archive",
          r.cancelled ? " cancelled" : r.error.empty() ? " done" : " failed", static_cast<int>(r.archived.size()),
          r.unread, r.downloaded, r.rebuilt, r.failed, r.listed ? r.remove.size() : size_t(0));
    const bool again = archive_again_;
    archive_again_ = false;
    // With both sources set up, their messages say whose they are.
    const std::string who = app_.articles->configured() && app_.wikipedia->configured()
                                ? std::string(wiki ? "Wikipedia: " : "wallabag: ")
                                : std::string();
    if (r.cancelled) { // stopped from the menu (quitting never gets here)
        app_.toast(who + (pending.empty() ? std::string("Stopped updating the articles")
                                          : std::string("Stopped. Read articles are sent with the next update.")));
        app_.refresh_library();
        return;
    }
    if (!r.error.empty()) {
        last_error_ = r.error;
        app_.toast(who + r.error + (pending.empty() ? "" : " Read articles are sent with the next update."), 6000);
    } else {
        last_error_.clear();
        if (r.update) {
            std::string msg = r.downloaded == 0 ? std::string("No new articles")
                              : r.downloaded == 1 ? std::string("1 new article")
                                                  : strf("%d new articles", r.downloaded);
            if (r.failed)
                msg += strf(" \xC2\xB7 %d couldn't be downloaded", r.failed);
            app_.toast(who + msg, r.failed ? 5000 : 2200, true); // not over the other source's error
        }
        if (again && !pending.empty())
            start(false);
    }
    app_.refresh_library();
}

std::string Articles::status() const {
    const bool wiki = source_ == ArticleSource::Wikipedia;
    if (busy()) {
        if (stopping_)
            return "Stopping\xE2\x80\xA6";
        SyncProgress p = worker_.progress();
        switch (p.phase) {
        case SyncProgress::Starting:
            return wiki ? std::string("Connecting to Wikipedia\xE2\x80\xA6")
                        : "Connecting to " + url_host(app_.cfg.wallabag.server) + "\xE2\x80\xA6";
        case SyncProgress::Archiving:
            return strf("Marking as read in wallabag\xE2\x80\xA6 %d of %d", std::min(p.done + 1, p.total), p.total);
        case SyncProgress::Listing:
            return wiki ? "Getting your reading lists\xE2\x80\xA6" : "Getting the list of articles\xE2\x80\xA6";
        case SyncProgress::Downloading:
            return p.total ? strf("Downloading\xE2\x80\xA6 %d of %d", std::min(p.done + 1, p.total), p.total)
                           : std::string("Checking\xE2\x80\xA6");
        }
    }
    if (!last_error_.empty())
        return "Update failed: " + last_error_;
    const std::string label = wiki ? "Reading lists \xC2\xB7 " : "wallabag \xC2\xB7 ";
    std::string n = count_ == 1 ? std::string("1 article") : strf("%d articles", count_);
    const int64_t t = updated();
    if (!t)
        return label + "not updated yet";
    return label + n + " \xC2\xB7 updated " + when_text(t, wall_time(), app_.cfg.clock_24h);
}

std::string Articles::empty_message() const {
    if (source_ == ArticleSource::Wikipedia) {
        if (busy())
            return "Downloading the articles saved in your Wikipedia reading lists\xE2\x80\xA6 They appear here as "
                   "they arrive.";
        return updated() ? "No saved articles. Save some in the Wikipedia app (with Sync reading lists on), then "
                           "choose Update articles in the menu."
                         : "No articles yet. Choose Update articles in the menu to download the articles saved in "
                           "your Wikipedia reading lists.";
    }
    if (busy())
        return "Downloading your unread wallabag articles\xE2\x80\xA6 They appear here as they arrive.";
    return updated() ? "No unread articles. Save some with wallabag, then choose Update articles in the menu."
                     : "No articles yet. Choose Update articles in the menu to download your unread wallabag articles.";
}

std::string Articles::about() const {
    if (source_ == ArticleSource::Wikipedia) {
        if (!configured())
            return "Not set up ([wikipedia] in config.ini)";
        std::string lists;
        for (const std::string& l : app_.cfg.wikipedia.lists)
            lists += (lists.empty() ? "" : ", ") + l;
        std::string s = "Reading lists" + (lists.empty() ? std::string(" (all)") : ": " + lists);
        if (app_.state.wikipedia_updated)
            s += " \xC2\xB7 updated " + when_text(app_.state.wikipedia_updated, wall_time(), app_.cfg.clock_24h);
        return s;
    }
    if (!configured())
        return "Not set up ([wallabag] in config.ini)";
    std::string s = url_host(app_.cfg.wallabag.server) + " \xC2\xB7 " + app_.cfg.wallabag.username;
    if (app_.state.wallabag_updated)
        s += " \xC2\xB7 updated " + when_text(app_.state.wallabag_updated, wall_time(), app_.cfg.clock_24h);
    return s;
}

} // namespace dr
