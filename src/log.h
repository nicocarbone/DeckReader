// Logging to stderr ("[deckreader] ..."), like DeckVideoPlayer's "[deckplayer]".
#pragma once

namespace dr {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void set_log_level(LogLevel level);
LogLevel log_level();
void logf(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace dr

#define LOG_E(...) ::dr::logf(::dr::LogLevel::Error, __VA_ARGS__)
#define LOG_W(...) ::dr::logf(::dr::LogLevel::Warn, __VA_ARGS__)
#define LOG_I(...) ::dr::logf(::dr::LogLevel::Info, __VA_ARGS__)
#define LOG_D(...) ::dr::logf(::dr::LogLevel::Debug, __VA_ARGS__)
