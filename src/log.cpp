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
    std::fprintf(stderr, "[deckreader] %s: ", names[static_cast<int>(level)]);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

} // namespace dr
