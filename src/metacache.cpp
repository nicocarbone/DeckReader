#include "metacache.h"

#include "book.h"
#include "fsutil.h"
#include "image_io.h"
#include "log.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <unistd.h>

#include "nlohmann/json.hpp"

namespace dr {

using json = nlohmann::json;

MetaCache::MetaCache(std::string cache_dir) : dir_(std::move(cache_dir)) {}

void MetaCache::load() {
    std::string text;
    if (dir_.empty() || !read_file(path_join(dir_, "metadata.json"), text))
        return;
    json root = json::parse(text, nullptr, false);
    if (!root.is_object() || !root.contains("books") || !root["books"].is_object())
        return; // a cache: just rebuild it
    for (const auto& [path, e] : root["books"].items()) {
        if (!e.is_object())
            continue;
        BookMeta m;
        m.title = e.value("title", "");
        m.author = e.value("author", "");
        m.language = e.value("language", "");
        m.has_cover = e.value("cover", false);
        m.failed = e.value("failed", false);
        m.size = e.value("size", 0ull);
        m.mtime = e.value("mtime", 0ll);
        meta_[path] = m;
    }
}

void MetaCache::save_if_dirty() {
    if (!dirty_ || dir_.empty())
        return;
    json books = json::object();
    for (const auto& [path, m] : meta_) {
        json e = {{"title", m.title}, {"author", m.author}, {"language", m.language}, {"size", m.size}, {"mtime", m.mtime}};
        if (m.has_cover)
            e["cover"] = true;
        if (m.failed)
            e["failed"] = true;
        books[path] = e;
    }
    json root = {{"version", 1}, {"books", books}};
    make_dirs(dir_);
    write_file_atomic(path_join(dir_, "metadata.json"), root.dump(0, ' ', false, json::error_handler_t::replace));
    dirty_ = false;
}

std::string MetaCache::cover_file(const std::string& path) const {
    return path_join(path_join(dir_, "covers"), hex64(fnv1a64(path)) + ".png");
}

const BookMeta* MetaCache::get(const std::string& path) {
    auto it = meta_.find(path);
    if (it == meta_.end())
        return nullptr;
    uint64_t size = 0;
    int64_t mtime = 0;
    if (!file_stat(path, &size, &mtime) || size != it->second.size || mtime != it->second.mtime)
        return nullptr;
    return &it->second;
}

const BookMeta* MetaCache::fetch(const std::string& path) {
    BookMeta m;
    if (!file_stat(path, &m.size, &m.mtime))
        return nullptr;
    BookInfo info;
    Image cover;
    uint64_t t0 = mono_ms();
    if (engine::read_metadata(path, info, &cover, COVER_HEIGHT)) {
        m.title = utf8_sanitize(info.title);
        m.author = utf8_sanitize(info.author);
        m.language = info.language;
        if (!cover.empty()) {
            make_dirs(path_join(dir_, "covers"));
            m.has_cover = save_png(cover_file(path), cover);
            evict(covers_, MAX_FULL_COVERS - 1);
            covers_[path] = Cached{std::move(cover), ++clock_};
        }
    } else {
        m.failed = true;
    }
    LOG_D("metadata for %s in %llu ms", path_basename(path).c_str(), static_cast<unsigned long long>(mono_ms() - t0));
    // A changed file gets a new cover too.
    for (auto it = scaled_.begin(); it != scaled_.end();)
        it = starts_with(it->first, path + "@") ? scaled_.erase(it) : std::next(it);
    meta_[path] = m;
    dirty_ = true;
    return &meta_[path];
}

void MetaCache::forget(const std::string& path) {
    auto it = meta_.find(path);
    if (it != meta_.end()) {
        if (it->second.has_cover)
            ::unlink(cover_file(path).c_str());
        meta_.erase(it);
        dirty_ = true;
    }
    covers_.erase(path);
    for (auto s = scaled_.begin(); s != scaled_.end();)
        s = starts_with(s->first, path + "@") ? scaled_.erase(s) : std::next(s);
}

void MetaCache::evict(std::map<std::string, Cached>& cache, size_t keep) {
    while (cache.size() > keep) {
        auto oldest = cache.begin();
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->second.used < oldest->second.used)
                oldest = it;
        cache.erase(oldest);
    }
}

const Image* MetaCache::cover(const std::string& path, int height_px) {
    const BookMeta* m = get(path);
    if (!m || !m->has_cover || height_px <= 0)
        return nullptr;
    std::string key = path + "@" + std::to_string(height_px);
    auto s = scaled_.find(key);
    if (s != scaled_.end()) {
        s->second.used = ++clock_;
        return &s->second.img;
    }
    auto c = covers_.find(path);
    if (c == covers_.end()) {
        Image img;
        if (!load_png(cover_file(path), img))
            return nullptr;
        evict(covers_, MAX_FULL_COVERS - 1);
        c = covers_.emplace(path, Cached{std::move(img), 0}).first;
    }
    c->second.used = ++clock_;
    const Image& full = c->second.img;
    int w = std::max(1, static_cast<int>(std::lround(static_cast<double>(full.w) * height_px / full.h)));
    evict(scaled_, MAX_SCALED_COVERS - 1);
    return &scaled_.emplace(key, Cached{scale_image(full, w, height_px), ++clock_}).first->second.img;
}

} // namespace dr
