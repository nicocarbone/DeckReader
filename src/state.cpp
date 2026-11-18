#include "state.h"

#include "fsutil.h"
#include "log.h"
#include "util.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "nlohmann/json.hpp"

namespace dr {

using json = nlohmann::json;

template <typename T> static void get_to(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end())
        return;
    try {
        out = it->get<T>();
    } catch (const json::exception&) {
        // wrong type: keep the default
    }
}

static json settings_to_json(const ReaderSettings& s) {
    return json{
        {"theme", THEMES[s.theme].id},
        {"font", s.font},
        {"font_size", s.font_size},
        {"line_spacing", s.line_spacing},
        {"para_spacing", s.para_spacing},
        {"side_margin", s.side_margin},
        {"vert_margin", s.vert_margin},
        {"align", s.align},
        {"pages", s.pages},
        {"hyphenation", s.hyphenation},
        {"text_aa", s.text_aa},
        {"book_styles", s.book_styles},
    };
}

static ReaderSettings settings_from_json(const json& j) {
    ReaderSettings s;
    std::string theme;
    get_to(j, "theme", theme);
    for (int i = 0; i < THEME_COUNT; i++)
        if (theme == THEMES[i].id)
            s.theme = i;
    get_to(j, "font", s.font);
    get_to(j, "font_size", s.font_size);
    get_to(j, "line_spacing", s.line_spacing);
    get_to(j, "para_spacing", s.para_spacing);
    get_to(j, "side_margin", s.side_margin);
    get_to(j, "vert_margin", s.vert_margin);
    get_to(j, "align", s.align);
    get_to(j, "pages", s.pages);
    get_to(j, "hyphenation", s.hyphenation);
    get_to(j, "text_aa", s.text_aa);
    get_to(j, "book_styles", s.book_styles);
    s.clamp();
    return s;
}

std::string StateStore::to_json() const {
    json books = json::object();
    for (const auto& [path, b] : books_) {
        json marks = json::array();
        for (const Bookmark& m : b.bookmarks)
            marks.push_back({{"xpointer", m.xpointer}, {"percent", m.percent}, {"chapter", m.chapter}, {"t", m.time}});
        json e = {{"xpointer", b.xpointer}, {"percent", b.percent}, {"page", b.page}, {"pages", b.pages},
                  {"t", b.last_read}, {"title", b.title}, {"author", b.author}};
        if (b.finished)
            e["finished"] = true;
        if (!marks.empty())
            e["bookmarks"] = marks;
        books[path] = e;
    }
    json root = {{"version", 1}, {"settings", settings_to_json(settings)}, {"books", books}};
    // Invalid UTF-8 (odd file names) is replaced rather than throwing.
    return root.dump(1, ' ', false, json::error_handler_t::replace) + "\n";
}

bool StateStore::from_json(const std::string& text) {
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return false;
    books_.clear();
    if (root.contains("settings") && root["settings"].is_object())
        settings = settings_from_json(root["settings"]);
    if (root.contains("books") && root["books"].is_object()) {
        for (const auto& [path, e] : root["books"].items()) {
            if (!e.is_object())
                continue;
            BookState b;
            get_to(e, "xpointer", b.xpointer);
            get_to(e, "percent", b.percent);
            get_to(e, "page", b.page);
            get_to(e, "pages", b.pages);
            get_to(e, "t", b.last_read);
            get_to(e, "title", b.title);
            get_to(e, "author", b.author);
            get_to(e, "finished", b.finished);
            b.percent = std::clamp(b.percent, 0, 10000);
            b.page = std::max(0, b.page);
            b.pages = std::max(0, b.pages);
            if (e.contains("bookmarks") && e["bookmarks"].is_array()) {
                for (const json& m : e["bookmarks"]) {
                    if (!m.is_object())
                        continue;
                    Bookmark bm;
                    get_to(m, "xpointer", bm.xpointer);
                    get_to(m, "percent", bm.percent);
                    get_to(m, "chapter", bm.chapter);
                    get_to(m, "t", bm.time);
                    bm.percent = std::clamp(bm.percent, 0, 10000);
                    if (!bm.xpointer.empty())
                        b.bookmarks.push_back(bm);
                }
            }
            books_[path] = std::move(b);
        }
    }
    return true;
}

bool StateStore::load() {
    std::string text;
    if (!read_file(path_, text)) {
        if (errno == ENOENT) {
            LOG_D("no state file at %s (first run)", path_.c_str());
            return true;
        }
        // It exists but can't be read now (permissions, I/O error): saving
        // would replace the reader's positions with an empty file (D22).
        LOG_E("cannot read %s (%s): reading positions will not be saved this time", path_.c_str(),
              std::strerror(errno));
        writable_ = false;
        return false;
    }
    if (from_json(text))
        return true;
    std::string aside = path_ + ".corrupt";
    std::rename(path_.c_str(), aside.c_str());
    LOG_W("state file could not be read; moved to %s and starting fresh", aside.c_str());
    return false;
}

bool StateStore::save() {
    if (path_.empty() || !writable_)
        return false;
    // Keep the file small: forget the books read longest ago.
    if (books_.size() > MAX_BOOKS) {
        std::vector<std::pair<int64_t, std::string>> order;
        for (const auto& [p, b] : books_)
            order.emplace_back(b.last_read, p);
        std::sort(order.begin(), order.end());
        for (size_t i = 0; i + MAX_BOOKS < order.size(); i++)
            books_.erase(order[i].second);
    }
    std::string err;
    if (!write_file_atomic(path_, to_json(), &err)) {
        LOG_E("saving state: %s", err.c_str());
        return false;
    }
    dirty_ = false;
    return true;
}

BookState* StateStore::find(const std::string& book) {
    auto it = books_.find(book);
    return it == books_.end() ? nullptr : &it->second;
}

BookState& StateStore::at(const std::string& book) {
    return books_[book];
}

std::vector<std::string> StateStore::recent(size_t max) const {
    std::vector<std::pair<int64_t, std::string>> order;
    for (const auto& [p, b] : books_)
        if (b.last_read > 0)
            order.emplace_back(b.last_read, p);
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::vector<std::string> out;
    for (size_t i = 0; i < order.size() && i < max; i++)
        out.push_back(order[i].second);
    return out;
}

int64_t StateStore::save_due_in(uint64_t now_ms) const {
    if (!dirty_ || !writable_)
        return -1;
    // After failed writes (disk full...) retry less and less often: 2 s,
    // 4 s ... up to about a minute, never in a tight loop.
    const uint64_t interval = SAVE_INTERVAL_MS << std::min(failures_, 5);
    if (!saved_once_ || now_ms >= last_save_ms_ + interval)
        return 0;
    return static_cast<int64_t>(last_save_ms_ + interval - now_ms);
}

void StateStore::save_if_due(uint64_t now_ms) {
    if (save_due_in(now_ms) == 0) {
        failures_ = save() ? 0 : failures_ + 1;
        last_save_ms_ = now_ms;
        saved_once_ = true;
    }
}

} // namespace dr
