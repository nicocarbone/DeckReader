#include "articles.h"

#include "app.h"
#include "fsutil.h"
#include "http.h"
#include "log.h"
#include "util.h"

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

Articles::Articles(App& app) : app_(app) {
    dir_ = path_join(app_.paths.data_dir, "articles");
    Uint32 ev = SDL_RegisterEvents(1);
    event_ = ev ? ev : static_cast<uint32_t>(SDL_EVENT_USER);
    if (configured()) {
        make_dirs(dir_);
        recount();
    }
}

bool Articles::configured() const {
    return app_.cfg.wallabag.configured();
}

bool Articles::is_article(const std::string& path) const {
    return configured() && path_dirname(path) == dir_ && article_id(path_basename(path)) > 0;
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
    SyncRequest req;
    req.settings = app_.cfg.wallabag;
    req.dir = dir_;
    req.archive = app_.state.wallabag_archive;
    req.local = local_ids();
    req.update = update;
    // Articles made by an older version come again (math, D36).
    req.rebuild = update && app_.state.wallabag_format < ARTICLE_FORMAT;
    req.temml_js = path_join(app_.paths.app_data, "js/temml.min.js");
    make_dirs(dir_);
    const uint32_t ev = event_;
    worker_.start(std::move(req), [ev] {
        SDL_Event e{};
        e.type = ev;
        SDL_PushEvent(&e); // thread-safe; wakes SDL_WaitEvent
    });
    LOG_I("wallabag: %s", update ? "updating articles" : "marking articles as read");
    app_.redraw();
}

void Articles::update() {
    if (!configured())
        return;
    if (busy()) {
        app_.toast("Already updating the articles");
        return;
    }
    start(true);
    app_.toast("Updating articles\xE2\x80\xA6");
}

void Articles::mark_read(const std::string& path) {
    if (!is_article(path))
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
    app_.redraw(); // progress on the Articles row
    SyncResult r;
    if (!worker_.take_result(r)) {
        // New articles appear in the Articles folder as they arrive.
        SyncProgress p = worker_.progress();
        if (p.phase == SyncProgress::Downloading && p.done > shown_done_ && app_.showing_articles()) {
            shown_done_ = p.done;
            app_.refresh_library();
        }
        return;
    }
    stopping_ = false;
    shown_done_ = 0;
    std::vector<int64_t>& pending = app_.state.wallabag_archive;
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
        app_.state.wallabag_updated = wall_time();
        app_.state.wallabag_format = ARTICLE_FORMAT; // the rebuild is done
        app_.state.mark_dirty();
    }
    recount();
    LOG_I("wallabag: %s%s; %d archived, %d unread, %d downloaded, %d rebuilt, %d failed, %zu removed",
          r.update ? "update" : "archive", r.cancelled ? " cancelled" : r.error.empty() ? " done" : " failed",
          static_cast<int>(r.archived.size()), r.unread, r.downloaded, r.rebuilt, r.failed,
          r.listed ? r.remove.size() : size_t(0));
    const bool again = archive_again_;
    archive_again_ = false;
    if (r.cancelled) { // stopped from the menu (quitting never gets here)
        app_.toast(pending.empty() ? std::string("Stopped updating the articles")
                                   : std::string("Stopped. Read articles are sent with the next update."));
        app_.refresh_library();
        return;
    }
    if (!r.error.empty()) {
        last_error_ = r.error;
        app_.toast(pending.empty() ? r.error : r.error + " Read articles are sent with the next update.", 6000);
    } else {
        last_error_.clear();
        if (r.update) {
            std::string msg = r.downloaded == 0 ? std::string("No new articles")
                              : r.downloaded == 1 ? std::string("1 new article")
                                                  : strf("%d new articles", r.downloaded);
            if (r.failed)
                msg += strf(" \xC2\xB7 %d couldn't be downloaded", r.failed);
            app_.toast(msg, r.failed ? 5000 : 2200);
        }
        if (again && !pending.empty())
            start(false);
    }
    app_.refresh_library();
}

std::string Articles::status() const {
    if (busy()) {
        if (stopping_)
            return "Stopping\xE2\x80\xA6";
        SyncProgress p = worker_.progress();
        switch (p.phase) {
        case SyncProgress::Starting:
            return "Connecting to " + url_host(app_.cfg.wallabag.server) + "\xE2\x80\xA6";
        case SyncProgress::Archiving:
            return strf("Marking as read in wallabag\xE2\x80\xA6 %d of %d", std::min(p.done + 1, p.total), p.total);
        case SyncProgress::Listing:
            return "Getting the list of articles\xE2\x80\xA6";
        case SyncProgress::Downloading:
            return p.total ? strf("Downloading\xE2\x80\xA6 %d of %d", std::min(p.done + 1, p.total), p.total)
                           : std::string("Checking\xE2\x80\xA6");
        }
    }
    if (!last_error_.empty())
        return "Update failed: " + last_error_;
    std::string n = count_ == 1 ? std::string("1 article") : strf("%d articles", count_);
    const int64_t t = app_.state.wallabag_updated;
    if (!t)
        return "wallabag \xC2\xB7 not updated yet";
    return "wallabag \xC2\xB7 " + n + " \xC2\xB7 updated " + when_text(t, wall_time(), app_.cfg.clock_24h);
}

std::string Articles::about() const {
    if (!configured())
        return "Not set up ([wallabag] in config.ini)";
    std::string s = url_host(app_.cfg.wallabag.server) + " \xC2\xB7 " + app_.cfg.wallabag.username;
    if (app_.state.wallabag_updated)
        s += " \xC2\xB7 updated " + when_text(app_.state.wallabag_updated, wall_time(), app_.cfg.clock_24h);
    return s;
}

} // namespace dr
