// What the library shows: sources (configured folders, wallabag articles,
// microSD cards and USB drives), folder listings, and the start screen's recently read books
// below a section header, as DeckVideoPlayer lists recently played videos
// (D23). No drawing here.
#pragma once

#include "config.h"
#include "metacache.h"
#include "state.h"

#include <string>
#include <vector>

namespace dr {

struct LibEntry {
    enum Kind { Book, Folder, Source, Header } kind = Book;
    std::string path;    // file or folder
    std::string name;    // folder or file name, or source label
    std::string detail;  // second line (source path, "Not found"...)
    enum Icon { IconFolder, IconCard, IconUsb, IconArticles } icon = IconFolder;
    bool missing = false; // configured folder that doesn't exist
    bool recent = false;  // a recently read book on the start screen
    bool articles = false; // the Articles source (wallabag, D31)
    std::string tag;      // where a recent book is: "Local", "Article", "SD Card", "USB"
    int64_t mtime = 0;    // file date (seconds); an article's is when it was saved
};

enum class SortMode { Name, Author, Recent, Newest };
const char* sort_label(SortMode m);

bool is_book_file(const std::string& name);

// The start screen: each source (the Articles folder after the configured
// ones, when `articles_dir` isn't empty), then a "Recently read" header and
// up to cfg.recently_read books (files that are gone or on a drive that
// isn't inserted are left out).
std::vector<LibEntry> list_root(const Config& cfg, StateStore& state, const std::string& mountinfo_text,
                                const std::string& home, const std::string& articles_dir = "");
// Subfolders first, then .epub files.
std::vector<LibEntry> list_folder(const std::string& dir);
// Folders stay first; books ordered by title/author/last read/file date
// (newest first). Titles come
// from the metadata cache when known, else the file name.
void sort_entries(std::vector<LibEntry>& entries, SortMode mode, MetaCache& meta, StateStore& state);

// Title and author to show for a book (metadata, else a cleaned file name).
std::string display_title(const LibEntry& e, const BookMeta* m);

} // namespace dr
