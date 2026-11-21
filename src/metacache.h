// Book titles, authors and cover thumbnails for the library, cached in
// $XDG_CACHE_HOME/deckreader (metadata.json + covers/*.png), keyed by path
// and checked against the file's size and modification time. Filled lazily,
// one book per idle moment, so folders open instantly (requirements 6.1).
#pragma once

#include "canvas.h"

#include <cstdint>
#include <map>
#include <string>

namespace dr {

struct BookMeta {
    std::string title, author, language;
    bool has_cover = false;
    bool failed = false; // not a readable EPUB (shown with its file name)
    uint64_t size = 0;
    int64_t mtime = 0;
};

class MetaCache {
public:
    static constexpr int COVER_HEIGHT = 240; // pixels stored on disk
    // Full-size thumbnails kept in memory (~150 KB each); scaled copies for
    // the rows on screen are kept separately and are much smaller.
    static constexpr size_t MAX_FULL_COVERS = 48;
    static constexpr size_t MAX_SCALED_COVERS = 200;

    explicit MetaCache(std::string cache_dir = {});
    void load();
    void save_if_dirty();

    // Cached metadata if still valid for the file on disk, else null.
    const BookMeta* get(const std::string& path);
    // Reads the book's metadata and cover now (tens of milliseconds).
    const BookMeta* fetch(const std::string& path);
    // The cover scaled to height_px (scaled copies are kept in memory).
    const Image* cover(const std::string& path, int height_px);
    // A book that is gone for good (an article removed): its entry and
    // cover thumbnail are deleted.
    void forget(const std::string& path);

private:
    std::string cover_file(const std::string& path) const;

    // In-memory images, evicted least recently used first (D22).
    struct Cached {
        Image img;
        uint64_t used = 0;
    };
    static void evict(std::map<std::string, Cached>& cache, size_t keep);

    std::string dir_;
    std::map<std::string, BookMeta> meta_;
    std::map<std::string, Cached> covers_; // full thumbnails by path
    std::map<std::string, Cached> scaled_; // path + "@" + height
    uint64_t clock_ = 0;
    bool dirty_ = false;
};

} // namespace dr
