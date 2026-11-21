#include "log.h"

#include <cstdarg>
#include <cstdio>

namespace dr {

static LogLevel g_level = LogLevel::Info;

void set_log_level(LogLevel level) {
    g_level = level;
}

LogLevel log_level() {
    return g_level;
}

void logf(LogLevel level, const char* fmt, ...) {
    if (level > g_level)
        return;
    static const char* names[] = {"error", "warn", "info", "debug"};
    // One write per line: the articles thread logs too (D31).
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[deckreader] %s: %s\n", names[static_cast<int>(level)], msg);
}

} // namespace dr
