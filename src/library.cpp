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
    case SortMode::Newest: return "Sorted by newest";
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
                                const std::string& home, const std::vector<ArticleRoot>& articles) {
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
    for (const ArticleRoot& a : articles) {
        LibEntry e;
        e.kind = LibEntry::Source;
        e.path = a.dir;
        e.name = a.name;
        e.icon = a.source == ArticleSource::Wikipedia ? LibEntry::IconWikipedia : LibEntry::IconArticles;
        e.articles = a.source;
        e.tag = a.tag;
        out.push_back(e); // its second line is the sync status (library_screen.cpp)
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

std::vector<LibEntry> list_articles(const std::string& dir, const std::vector<ArticleList>& lists, int64_t list) {
    std::vector<LibEntry> files = list_folder(dir);
    if (lists.empty())
        return files;
    std::map<int64_t, const LibEntry*> by_id;
    for (const LibEntry& f : files)
        if (f.kind == LibEntry::Book)
            if (int64_t id = article_id(path_basename(f.path)))
                by_id[id] = &f;
    // The articles of a list that are here, in its order.
    auto here = [&](const ArticleList& l) {
        std::vector<LibEntry> out;
        for (int64_t id : l.articles)
            if (auto it = by_id.find(id); it != by_id.end())
                out.push_back(*it->second);
        return out;
    };
    if (list) {
        for (const ArticleList& l : lists)
            if (l.id == list)
                return here(l);
        return {}; // that list is gone
    }
    // A folder for each list with articles here; just one: its articles
    // straight away (V4-UI-1, D38).
    std::vector<LibEntry> folders;
    const ArticleList* only = nullptr;
    for (const ArticleList& l : lists) {
        const std::vector<LibEntry> in = here(l);
        if (in.empty())
            continue;
        only = &l;
        LibEntry e;
        e.kind = LibEntry::Folder;
        e.name = l.name;
        e.list = l.id;
        e.path = path_join(dir, strf("#list-%lld", static_cast<long long>(l.id))); // unique, never opened
        for (const LibEntry& b : in)
            e.mtime = std::max(e.mtime, b.mtime);
        e.detail = in.size() == 1 ? std::string("1 article") : strf("%zu articles", in.size());
        folders.push_back(e);
    }
    return folders.size() == 1 ? here(*only) : folders;
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
        k.t = mode == SortMode::Newest ? e.mtime : b ? b->last_read : 0;
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
        if ((mode == SortMode::Recent || mode == SortMode::Newest) && a.t != b.t)
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
