#include "util.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace dr {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        e--;
    return s.substr(b, e - b);
}

std::string to_lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string to_upper(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size())
        return false;
    return to_lower(s.substr(s.size() - suffix.size())) == to_lower(suffix);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos)
            break;
        start = p + 1;
    }
    return out;
}

// Base letters for U+00C0..U+00FF ('\0' = keep as is). The letters with
// diacritics sort right after their base letter's run, which is what
// Spanish, Portuguese, French and German readers expect from a library.
static const char LATIN1_FOLD[64] = {
    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i', //
    'd', 'n', 'o', 'o', 'o', 'o', 'o', 0,   'o', 'u', 'u', 'u', 'u', 'y', 0,   's', //
    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i', //
    'd', 'n', 'o', 'o', 'o', 'o', 'o', 0,   'o', 'u', 'u', 'u', 'u', 'y', 0,   'y', //
};

std::string sort_key(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += static_cast<char>(std::tolower(c));
        } else if ((c == 0xC3) && i + 1 < s.size()) {
            unsigned char d = static_cast<unsigned char>(s[i + 1]);
            int cp = 0xC0 + (d & 0x3F);
            if (d >= 0x80 && d <= 0xBF && LATIN1_FOLD[cp - 0xC0]) {
                out += LATIN1_FOLD[cp - 0xC0];
                // "ñ" sorts after every "n" word: keep a marker after the n.
                if (cp == 0xD1 || cp == 0xF1)
                    out += '\x7f';
                i++;
                continue;
            }
            out += s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

bool natural_less(const std::string& a_in, const std::string& b_in) {
    const std::string a = sort_key(a_in), b = sort_key(b_in);
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] == '0')
                i2++;
            while (j2 < b.size() && b[j2] == '0')
                j2++;
            size_t ie = i2, je = j2;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie])))
                ie++;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je])))
                je++;
            if (ie - i2 != je - j2)
                return ie - i2 < je - j2;
            int c = a.compare(i2, ie - i2, b, j2, je - j2);
            if (c != 0)
                return c < 0;
            i = ie;
            j = je;
            continue;
        }
        if (ca != cb)
            return ca < cb;
        i++;
        j++;
    }
    if (a.size() - i != b.size() - j)
        return a.size() - i < b.size() - j;
    return a_in < b_in; // stable tie-break
}

std::string path_basename(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string path_dirname(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    size_t s = p.rfind('/');
    if (s == std::string::npos)
        return ".";
    if (s == 0)
        return "/";
    return p.substr(0, s);
}

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty())
        return b;
    if (a.back() == '/')
        return a + b;
    return a + "/" + b;
}

std::string strip_extension(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return name;
    return name.substr(0, dot);
}

std::string expand_user(const std::string& path, const std::string& home) {
    if (path == "~")
        return home;
    if (starts_with(path, "~/"))
        return path_join(home, path.substr(2));
    return path;
}

size_t utf8_prev(const std::string& s, size_t i) {
    if (i == 0)
        return 0;
    i--;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
        i--;
    return i;
}

std::string utf8_sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + len <= s.size();
        for (int k = 1; ok && k < len; k++)
            ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (ok) {
            out.append(s, i, len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD"; // U+FFFD
            i++;
        }
    }
    return out;
}

uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

int64_t wall_time() {
    return static_cast<int64_t>(std::time(nullptr));
}

int64_t wall_time_ms() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

uint64_t mono_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

std::string strf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::string out;
    if (n >= 0 && static_cast<size_t>(n) < sizeof buf) {
        out.assign(buf, static_cast<size_t>(n));
    } else if (n >= 0) {
        out.resize(static_cast<size_t>(n) + 1);
        std::vsnprintf(&out[0], out.size(), fmt, ap2);
        out.resize(static_cast<size_t>(n));
    }
    va_end(ap2);
    return out;
}

} // namespace dr
