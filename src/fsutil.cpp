#include "fsutil.h"

#include "util.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dr {

bool read_file(const std::string& path, std::string& out, size_t max_bytes) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    out.clear();
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return false;
        }
        if (n == 0)
            break;
        if (out.size() + static_cast<size_t>(n) > max_bytes) {
            ::close(fd);
            errno = EFBIG;
            return false;
        }
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return true;
}

static bool write_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& data, std::string* error) {
    const std::string dir = path_dirname(path);
    std::string tmp = path + ".tmp-XXXXXX";
    std::vector<char> tmpl(tmp.begin(), tmp.end());
    tmpl.push_back('\0');
    int fd = ::mkostemp(tmpl.data(), O_CLOEXEC); // created 0600
    if (fd < 0) {
        if (error)
            *error = strf("cannot create %s: %s", tmpl.data(), std::strerror(errno));
        return false;
    }
    tmp = tmpl.data();
    bool ok = write_all(fd, data.data(), data.size()) && ::fsync(fd) == 0;
    int saved_errno = errno;
    ok = (::close(fd) == 0) && ok;
    if (ok && ::rename(tmp.c_str(), path.c_str()) != 0) {
        saved_errno = errno;
        ok = false;
    }
    if (!ok) {
        ::unlink(tmp.c_str());
        if (error)
            *error = strf("cannot write %s: %s", path.c_str(), std::strerror(saved_errno));
        return false;
    }
    // Make the rename itself durable.
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

bool make_dirs(const std::string& path) {
    if (path.empty())
        return false;
    if (is_dir(path))
        return true;
    std::string parent = path_dirname(path);
    if (parent != path && !is_dir(parent) && !make_dirs(parent))
        return false;
    return ::mkdir(path.c_str(), 0700) == 0 || (errno == EEXIST && is_dir(path));
}

bool is_dir(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool file_stat(const std::string& path, uint64_t* size, int64_t* mtime) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return false;
    if (size)
        *size = static_cast<uint64_t>(st.st_size);
    if (mtime)
        *mtime = static_cast<int64_t>(st.st_mtime);
    return true;
}

bool list_dir(const std::string& path, std::vector<DirEntry>& out) {
    out.clear();
    DIR* d = ::opendir(path.c_str());
    if (!d)
        return false;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.')
            continue; // ".", ".." and hidden files
        DirEntry entry;
        entry.name = e->d_name;
        struct stat st{};
        if (::fstatat(dirfd(d), e->d_name, &st, 0) != 0)
            continue; // dangling symlink, permission problem
        if (S_ISDIR(st.st_mode))
            entry.is_dir = true;
        else if (!S_ISREG(st.st_mode))
            continue;
        entry.size = static_cast<uint64_t>(st.st_size);
        entry.mtime = static_cast<int64_t>(st.st_mtime);
        out.push_back(std::move(entry));
    }
    ::closedir(d);
    return true;
}

// mountinfo escapes space, tab, newline and backslash as \ooo.
static std::string unescape_mountinfo(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        auto octal = [&](size_t k) { return s[k] >= '0' && s[k] <= '7'; };
        if (s[i] == '\\' && i + 3 < s.size() && octal(i + 1) && octal(i + 2) && octal(i + 3)) {
            int v = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
            out += static_cast<char>(v);
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<Mount> parse_mountinfo(const std::string& text) {
    std::vector<Mount> out;
    for (const std::string& line : split(text, '\n')) {
        std::vector<std::string> f;
        for (const std::string& tok : split(line, ' '))
            if (!tok.empty())
                f.push_back(tok);
        if (f.size() < 7)
            continue;
        size_t dash = 6;
        while (dash < f.size() && f[dash] != "-")
            dash++;
        if (dash + 2 >= f.size())
            continue;
        Mount m;
        m.mount_point = unescape_mountinfo(f[4]);
        m.source = unescape_mountinfo(f[dash + 2]);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<Mount> removable_mounts(const std::string& mountinfo_text) {
    const std::vector<Mount> all = parse_mountinfo(mountinfo_text);
    std::vector<Mount> out;
    for (const Mount& m : all) {
        const std::string& p = m.mount_point;
        if (!starts_with(p, "/run/media/") && !starts_with(p, "/media/"))
            continue;
        // Real drives only: a tmpfs or bind mount of the per-user folder
        // (/run/media/deck in a Flatpak) is not a drive (D22).
        if (!starts_with(m.source, "/dev/"))
            continue;
        // Skip the per-user folder itself (/run/media/deck), which in a
        // Flatpak can be a bind mount of its own.
        bool parent_of_other = false;
        for (const Mount& o : all)
            if (o.mount_point.size() > p.size() && starts_with(o.mount_point, p + "/"))
                parent_of_other = true;
        if (parent_of_other)
            continue;
        bool dup = false;
        for (const Mount& o : out)
            dup = dup || o.mount_point == p;
        if (!dup)
            out.push_back(m);
    }
    return out;
}

std::string removable_label(const Mount& m) {
    return m.source.find("mmcblk") != std::string::npos ? "SD Card" : "USB drive";
}

} // namespace dr
