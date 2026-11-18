// File system helpers: atomic writes, folder listings, removable media.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dr {

// False on failure, with errno set (ENOENT: the file doesn't exist).
bool read_file(const std::string& path, std::string& out, size_t max_bytes = 64u << 20);

// Writes data to path so that a crash or power loss at any moment leaves
// either the old or the new file, never a mix or an empty file: write to a
// temporary file in the same folder, fsync it, rename over the target, fsync
// the folder (D6). The file is created owner-only (0600).
bool write_file_atomic(const std::string& path, const std::string& data, std::string* error = nullptr);

bool make_dirs(const std::string& path);
bool is_dir(const std::string& path);
bool is_file(const std::string& path);
bool file_stat(const std::string& path, uint64_t* size, int64_t* mtime);

struct DirEntry {
    std::string name;
    bool is_dir = false;
    uint64_t size = 0;
    int64_t mtime = 0;
};
// Lists a folder (no "." or "..", no hidden entries). Symlinks are followed.
bool list_dir(const std::string& path, std::vector<DirEntry>& out);

struct Mount {
    std::string mount_point;
    std::string source; // e.g. /dev/mmcblk0p1
};
// Parses /proc/self/mountinfo text.
std::vector<Mount> parse_mountinfo(const std::string& text);
// Mounts of /dev devices strictly below /run/media or /media (microSD
// cards, USB drives), as SteamOS and desktop distributions mount them (D10).
std::vector<Mount> removable_mounts(const std::string& mountinfo_text);
// "SD Card" for mmcblk devices, "USB drive" otherwise (as DeckVideoPlayer
// D33; the library adds the volume name when two would look the same).
std::string removable_label(const Mount& m);

} // namespace dr
