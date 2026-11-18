// Small string, path and time helpers shared by every module.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dr {

std::string trim(const std::string& s);
std::string to_lower(std::string s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with_ci(const std::string& s, const std::string& suffix);
std::vector<std::string> split(const std::string& s, char sep);

// Folds a UTF-8 string for sorting: ASCII lower case, and Latin-1 accented
// letters to their base letter ("Ángel" sorts with "angel", "Ñ" after "N").
std::string sort_key(const std::string& s);
// Natural, case- and accent-insensitive order ("Book 2" < "Book 10").
bool natural_less(const std::string& a, const std::string& b);

std::string path_basename(const std::string& path);
std::string path_dirname(const std::string& path);
std::string path_join(const std::string& a, const std::string& b);
std::string strip_extension(const std::string& name);
// "~" and "~/x" expanded with home; other paths unchanged.
std::string expand_user(const std::string& path, const std::string& home);

// Start index of the UTF-8 code point before byte index i (0 if none).
size_t utf8_prev(const std::string& s, size_t i);
// Removes invalid UTF-8 sequences (file names are not guaranteed UTF-8).
std::string utf8_sanitize(const std::string& s);

uint64_t fnv1a64(const std::string& s);
std::string hex64(uint64_t v);

// Seconds since the epoch.
int64_t wall_time();
// Milliseconds from a monotonic clock.
uint64_t mono_ms();

// printf into a std::string.
std::string strf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace dr
