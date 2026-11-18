// Archive hardening (requirements 7.2): every EPUB's ZIP directory is checked
// before crengine parses the book. crengine reads entries into memory and
// never extracts files to disk, so "Zip Slip" can't write anywhere; the path
// checks still reject archives that are malformed on purpose, and the size
// limits stop decompression bombs from exhausting memory (D5).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dr {

struct ArchiveEntry {
    std::string name;
    uint64_t size = 0;   // uncompressed
    uint64_t packed = 0; // compressed
};

struct ArchiveLimits {
    uint64_t max_entry = 256ull << 20;  // one file inside the book
    uint64_t max_total = 1024ull << 20; // all files together
    size_t max_entries = 20000;
    // Text compresses ~3-10x, images not at all; deflate bombs reach ~1000x.
    // Entries from ratio_min_size up may not exceed max_ratio, and the whole
    // archive, from total_ratio_min_size up, may not exceed max_total_ratio
    // (many entries just under the per-entry threshold, D22).
    uint64_t max_ratio = 200;
    uint64_t ratio_min_size = 1ull << 20;
    uint64_t max_total_ratio = 50;
    uint64_t total_ratio_min_size = 16ull << 20;
};

// Returns "" when the archive looks safe, or a short reason for the user.
std::string check_archive(const std::vector<ArchiveEntry>& entries, const ArchiveLimits& limits = {});
// True for names like "../x", "/etc/x", "a/../../x", "C:\x" or with NUL bytes.
bool unsafe_entry_name(const std::string& name);
// True for archives inside the archive (.epub, .zip, .fb3, .docx, .odt):
// EPUBs never contain them, and crengine would open one, unchecked, if the
// outer file is not recognised as an EPUB (D22).
bool nested_archive_name(const std::string& name);

} // namespace dr
