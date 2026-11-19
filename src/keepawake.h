// Keeps the screen from dimming and the system from sleeping while a book
// is being read (D28), as DeckVideoPlayer does during playback (its D43).
//
// SteamOS game mode dims and sleeps on Steam's own idle timer, which only
// honours org.freedesktop.ScreenSaver inhibitions, and an inhibition lasts
// as long as the D-Bus connection that asked for it. So a small helper
// program, deckreader-keep-awake (helper/), holds it: it runs while a book
// is open and in use, and is stopped when the book is closed or after
// `keep_awake_minutes` without input, so a Deck left on the table still
// dims and sleeps on Steam's timers. Nothing runs in the library.
#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace dr {

// Pure: "request: screensaver accepted, portal not running" (the helper's
// status file) -> "screensaver accepted, portal not running"; "" if absent.
std::string parse_keep_awake_status(const std::string& text);

class KeepAwake {
public:
    ~KeepAwake();
    // helper: path of deckreader-keep-awake; status_file: where it reports
    // (private: XDG_RUNTIME_DIR); minutes: 0 disables keeping awake.
    void setup(const std::string& helper, const std::string& status_file, int minutes);

    // The reader was used at `now` (ms): hold the inhibition (start the
    // helper if needed) until `minutes` after this.
    void activity(uint64_t now);
    // Not reading any more (library, quit): let the system's timers apply.
    void release();
    // Releases once the time without input has passed.
    void tick(uint64_t now);
    // Milliseconds until tick() has something to do; -1 when nothing is held.
    int64_t next_due(uint64_t now) const;

    bool enabled() const { return minutes_ > 0; }
    bool held() const { return pid_ > 0; }
    int minutes() const { return minutes_; }
    // What the helper achieved, for the About screen: "screensaver
    // accepted, portal not running", "starting…", or why it stopped.
    std::string status();

private:
    void start();
    void stop();
    void reap();
    std::string helper_, status_file_;
    int minutes_ = 0;
    pid_t pid_ = -1;
    uint64_t release_at_ = 0;
    std::string error_;          // the helper exited on its own
    std::vector<pid_t> zombies_; // stopped helpers not reaped yet
};

} // namespace dr
