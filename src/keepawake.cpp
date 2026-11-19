#include "keepawake.h"

#include "fsutil.h"
#include "log.h"
#include "util.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace dr {

std::string parse_keep_awake_status(const std::string& text) {
    for (const std::string& line : split(text, '\n'))
        if (starts_with(line, "request:"))
            return trim(line.substr(8));
    return {};
}

KeepAwake::~KeepAwake() {
    stop();
    reap();
}

void KeepAwake::setup(const std::string& helper, const std::string& status_file, int minutes) {
    helper_ = helper;
    status_file_ = status_file;
    minutes_ = std::max(0, minutes);
}

void KeepAwake::activity(uint64_t now) {
    if (!enabled())
        return;
    release_at_ = now + static_cast<uint64_t>(minutes_) * 60000;
    reap(); // notices a helper that exited on its own
    if (pid_ <= 0 && error_.empty())
        start();
}

void KeepAwake::release() {
    stop();
    error_.clear(); // try again next time
    release_at_ = 0;
}

void KeepAwake::tick(uint64_t now) {
    if (release_at_ && now >= release_at_) {
        LOG_I("keep awake: %d min without input, letting the screen dim", minutes_);
        release();
    }
}

int64_t KeepAwake::next_due(uint64_t now) const {
    if (!release_at_)
        return -1;
    return release_at_ > now ? static_cast<int64_t>(release_at_ - now) : 0;
}

void KeepAwake::start() {
    std::remove(status_file_.c_str());
    const char* argv[] = {helper_.c_str(), "--status", status_file_.c_str(), nullptr};
    pid_t pid = -1;
    // posix_spawn: no fork of a large process, and the child execs at once.
    int rc = posix_spawn(&pid, helper_.c_str(), nullptr, nullptr, const_cast<char**>(argv), environ);
    if (rc != 0) {
        error_ = rc == ENOENT ? "deckreader-keep-awake not found" : strf("could not start (%s)", std::strerror(rc));
        LOG_W("keep awake: %s: %s", helper_.c_str(), error_.c_str());
        pid_ = -1;
        return;
    }
    pid_ = pid;
    LOG_D("keep awake: helper %d started", static_cast<int>(pid));
}

void KeepAwake::stop() {
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        zombies_.push_back(pid_);
        pid_ = -1;
        LOG_D("keep awake: helper stopped");
    }
    std::remove(status_file_.c_str());
    reap();
}

// Collects stopped helpers without waiting (one may still be finishing a
// D-Bus call) and notices if the running one exited on its own.
void KeepAwake::reap() {
    zombies_.erase(std::remove_if(zombies_.begin(), zombies_.end(),
                                  [](pid_t p) { return waitpid(p, nullptr, WNOHANG) != 0; }),
                   zombies_.end());
    if (pid_ <= 0)
        return;
    int st = 0;
    pid_t r = waitpid(pid_, &st, WNOHANG);
    if (r == 0)
        return; // still running
    // It should run until stopped: any exit means nothing is held. Exit 1 =
    // the request failed (the status file says why).
    std::string why = parse_keep_awake_status(([&] {
        std::string t;
        read_file(status_file_, t, 4096);
        return t;
    })());
    if (r > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 1 && !why.empty())
        error_ = why;
    else if (r > 0 && WIFEXITED(st))
        error_ = strf("stopped (exit %d)", WEXITSTATUS(st));
    else if (r > 0 && WIFSIGNALED(st))
        error_ = strf("stopped (signal %d)", WTERMSIG(st));
    else
        error_ = "stopped";
    LOG_W("keep awake: helper %s", error_.c_str());
    pid_ = -1;
}

std::string KeepAwake::status() {
    if (!enabled())
        return "Off (config.ini)";
    reap();
    if (!error_.empty())
        return error_;
    if (pid_ <= 0)
        return "Only while reading";
    std::string text;
    read_file(status_file_, text, 4096);
    std::string s = parse_keep_awake_status(text);
    return s.empty() ? "starting\xE2\x80\xA6" : s;
}

} // namespace dr
