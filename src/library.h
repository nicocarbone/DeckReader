// What the library shows: sources (configured folders, microSD cards and
// USB drives), folder listings, recent books. No drawing here.
#pragma once

#include "config.h"
#include "metacache.h"
#include "state.h"

#include <string>
#include <vector>

namespace dr {

struct LibEntry {
    enum Kind { Book, Folder, Recent, Source } kind = Book;
    std::string path;    // file or folder
    std::string name;    // folder or file name, or source label
    std::string detail;  // second line (source path, "Not found"...)
    enum Icon { IconFolder, IconCard, IconUsb } icon = IconFolder;
    bool missing = false;    // configured folder that doesn't exist
    bool continue_row = false; // "Continue reading" shortcut on the start screen
    int64_t mtime = 0;
};

enum class SortMode { Name, Author, Recent };
const char* sort_label(SortMode m);

bool is_book_file(const std::string& name);

// The start screen: "Continue reading", "Recent books", then each source.
std::vector<LibEntry> list_root(const Config& cfg, StateStore& state, const std::string& mountinfo_text,
                                const std::string& home);
// Subfolders first, then .epub files.
std::vector<LibEntry> list_folder(const std::string& dir);
std::vector<LibEntry> list_recent(StateStore& state, size_t max);
// Folders stay first; books ordered by title/author/last read. Titles come
// from the metadata cache when known, else the file name.
void sort_entries(std::vector<LibEntry>& entries, SortMode mode, MetaCache& meta, StateStore& state);

// Title and author to show for a book (metadata, else a cleaned file name).
std::string display_title(const LibEntry& e, const BookMeta* m);

} // namespace dr
