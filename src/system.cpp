#include "system.h"

#include "fsutil.h"
#include "util.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <sys/time.h>

namespace dr {

Battery read_battery() {
    Battery b;
    const std::string root = "/sys/class/power_supply";
    std::vector<DirEntry> entries;
    // list_dir skips names starting with "." only, and supplies are symlinks
    // to folders, which it follows.
    if (!list_dir(root, entries))
        return b;
    for (const DirEntry& e : entries) {
        std::string dir = path_join(root, e.name);
        std::string type, cap, status, scope;
        if (!read_file(path_join(dir, "type"), type, 64) || trim(type) != "Battery")
            continue;
        // Skip controller/peripheral batteries ("Device" scope).
        if (read_file(path_join(dir, "scope"), scope, 64) && trim(scope) == "Device")
            continue;
        if (!read_file(path_join(dir, "capacity"), cap, 64))
            continue;
        b.present = true;
        b.percent = std::atoi(trim(cap).c_str());
        if (read_file(path_join(dir, "status"), status, 64)) {
            std::string s = trim(status);
            b.charging = s == "Charging" || s == "Full";
        }
        return b;
    }
    return b;
}

std::string clock_text(bool h24) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    if (h24)
        return strf("%02d:%02d", tm.tm_hour, tm.tm_min);
    int h = tm.tm_hour % 12;
    return strf("%d:%02d %s", h == 0 ? 12 : h, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}

int ms_to_next_minute() {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    std::tm tm{};
    std::time_t t = tv.tv_sec;
    localtime_r(&t, &tm);
    // tm_sec is 60 during a leap second.
    int ms = (60 - std::min(tm.tm_sec, 59)) * 1000 - static_cast<int>(tv.tv_usec / 1000) + 50;
    return std::max(ms, 50);
}

} // namespace dr
