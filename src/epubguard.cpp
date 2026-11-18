#include "epubguard.h"

#include "util.h"

namespace dr {

bool unsafe_entry_name(const std::string& name) {
    if (name.empty())
        return true;
    if (name.find('\0') != std::string::npos)
        return true;
    if (name[0] == '/' || name[0] == '\\')
        return true;
    if (name.size() >= 2 && name[1] == ':')
        return true; // drive letter
    // Normalise separators, then reject any ".." component.
    std::string n = name;
    for (char& c : n)
        if (c == '\\')
            c = '/';
    for (const std::string& part : split(n, '/'))
        if (part == "..")
            return true;
    return false;
}

bool nested_archive_name(const std::string& name) {
    for (const char* ext : {".epub", ".zip", ".fb3", ".docx", ".odt"})
        if (ends_with_ci(name, ext))
            return true;
    return false;
}

std::string check_archive(const std::vector<ArchiveEntry>& entries, const ArchiveLimits& limits) {
    if (entries.empty())
        return "The file is not a valid EPUB (empty or not a ZIP archive).";
    if (entries.size() > limits.max_entries)
        return strf("The book has too many files inside (%zu).", entries.size());
    uint64_t total = 0, total_packed = 0;
    for (const ArchiveEntry& e : entries) {
        if (unsafe_entry_name(e.name))
            return "The book contains a file with an unsafe name.";
        if (nested_archive_name(e.name))
            return "The book contains another archive, which EPUBs never do.";
        if (e.size > limits.max_entry)
            return strf("A file inside the book is too large (%llu MB).",
                        static_cast<unsigned long long>(e.size >> 20));
        if (e.size >= limits.ratio_min_size && e.packed > 0 && e.size / e.packed > limits.max_ratio)
            return "The book contains a suspiciously compressed file.";
        if (e.size >= limits.ratio_min_size && e.packed == 0)
            return "The book contains a suspiciously compressed file.";
        if (e.size > limits.max_total - total) // no overflow, whatever the limits
            return "The book is too large when unpacked.";
        total += e.size;
        total_packed += e.packed;
    }
    if (total >= limits.total_ratio_min_size && (total_packed == 0 || total / total_packed > limits.max_total_ratio))
        return "The book is suspiciously compressed.";
    return {};
}

} // namespace dr
