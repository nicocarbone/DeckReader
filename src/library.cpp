#include "library.h"

#include "fsutil.h"
#include "util.h"

#include <algorithm>
#include <map>

namespace dr {

const char* sort_label(SortMode m) {
    switch (m) {
    case SortMode::Name: return "Sorted by title";
    case SortMode::Author: return "Sorted by author";
    case SortMode::Recent: return "Sorted by last read";
    }
    return "";
}

bool is_book_file(const std::string& name) {
    return ends_with_ci(name, ".epub");
}

static std::string tilde(const std::string& path, const std::string& home) {
    if (!home.empty() && starts_with(path, home + "/"))
        return "~" + path.substr(home.size());
    return path;
}

std::vector<LibEntry> list_root(const Config& cfg, StateStore& state, const std::string& mountinfo_text,
                                const std::string& home) {
    std::vector<LibEntry> out;
    for (const std::string& folder : cfg.folders) {
        LibEntry e;
        e.kind = LibEntry::Source;
        e.path = folder;
        e.name = utf8_sanitize(path_basename(folder));
        e.icon = LibEntry::IconFolder;
        e.missing = !is_dir(folder);
        e.detail = e.missing ? "Not found: " + tilde(folder, home) : tilde(folder, home);
        e.tag = "Local";
        out.push_back(e);
    }
    if (cfg.removable_media) {
        std::vector<Mount> mounts = removable_mounts(mountinfo_text);
        std::map<std::string, int> count;
        for (const Mount& m : mounts)
            count[removable_label(m)]++;
        for (const Mount& m : mounts) {
            LibEntry e;
            e.kind = LibEntry::Source;
            e.path = m.mount_point;
            std::string label = removable_label(m);
            e.name = count[label] > 1 ? label + " \xC2\xB7 " + utf8_sanitize(path_basename(m.mount_point)) : label;
            e.icon = label == "SD Card" ? LibEntry::IconCard : LibEntry::IconUsb;
            e.detail = m.mount_point;
            e.tag = label == "SD Card" ? "SD Card" : "USB";
            out.push_back(e);
        }
    }
    const size_t sources = out.size();
    // Where a recent book is: the source whose folder holds it (longest match).
    auto tag_for = [&](const std::string& path) {
        std::string tag;
        size_t best = 0;
        for (size_t i = 0; i < sources; i++) {
            const std::string& root = out[i].path;
            if (root.size() > best && starts_with(path, root + "/")) {
                best = root.size();
                tag = out[i].tag;
            }
        }
        return tag;
    };
    std::vector<LibEntry> recent;
    if (cfg.recently_read > 0) {
        // Ask for more than shown: some may be gone or on a removed card.
        for (const std::string& p : state.recent(static_cast<size_t>(cfg.recently_read) * 4)) {
            if (recent.size() >= static_cast<size_t>(cfg.recently_read))
                break;
            if (!is_file(p))
                continue;
            LibEntry e;
            e.kind = LibEntry::Book;
            e.path = p;
            e.name = utf8_sanitize(path_basename(p));
            e.recent = true;
            e.tag = tag_for(p);
            recent.push_back(e);
        }
    }
    if (!recent.empty()) {
        LibEntry h;
        h.kind = LibEntry::Header;
        h.name = "Recently read";
        out.push_back(h);
        out.insert(out.end(), recent.begin(), recent.end());
    }
    return out;
}

std::vector<LibEntry> list_folder(const std::string& dir) {
    std::vector<LibEntry> out;
    std::vector<DirEntry> entries;
    list_dir(dir, entries);
    for (const DirEntry& d : entries) {
        if (!d.is_dir && !is_book_file(d.name))
            continue;
        LibEntry e;
        e.kind = d.is_dir ? LibEntry::Folder : LibEntry::Book;
        e.path = path_join(dir, d.name);
        e.name = utf8_sanitize(d.name);
        e.mtime = d.mtime;
        out.push_back(e);
    }
    return out;
}

std::string display_title(const LibEntry& e, const BookMeta* m) {
    if (m && !m->title.empty())
        return m->title;
    // "Some_Book_Title.epub" -> "Some Book Title"
    std::string t = strip_extension(e.name);
    for (char& c : t)
        if (c == '_')
            c = ' ';
    return t;
}

void sort_entries(std::vector<LibEntry>& entries, SortMode mode, MetaCache& meta, StateStore& state) {
    struct Key {
        int group;
        std::string a, b;
        int64_t t;
    };
    std::vector<std::pair<Key, LibEntry>> keyed;
    for (LibEntry& e : entries) {
        Key k;
        k.group = e.kind == LibEntry::Book ? 1 : 0;
        const BookMeta* m = e.kind == LibEntry::Book ? meta.get(e.path) : nullptr;
        std::string title = e.kind == LibEntry::Book ? display_title(e, m) : e.name;
        std::string author = m ? m->author : std::string();
        const BookState* b = e.kind == LibEntry::Book ? state.find(e.path) : nullptr;
        k.t = b ? b->last_read : 0;
        if (mode == SortMode::Author && k.group == 1) {
            k.a = author.empty() ? "\x7f" : author; // unknown authors last
            k.b = title;
        } else {
            k.a = title;
        }
        keyed.emplace_back(k, std::move(e));
    }
    std::stable_sort(keyed.begin(), keyed.end(), [mode](const auto& x, const auto& y) {
        const Key& a = x.first;
        const Key& b = y.first;
        if (a.group != b.group)
            return a.group < b.group;
        if (mode == SortMode::Recent && a.t != b.t)
            return a.t > b.t;
        if (a.a != b.a)
            return natural_less(a.a, b.a);
        return natural_less(a.b, b.b);
    });
    entries.clear();
    for (auto& [k, e] : keyed)
        entries.push_back(std::move(e));
}

} // namespace dr
