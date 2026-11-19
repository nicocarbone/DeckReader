/*
 * deckreader-keep-awake: keeps the screen awake while a book is read (D28).
 *
 * DeckVideoPlayer's dvp-keep-awake (its D43), renamed. SteamOS game mode
 * dims and sleeps on Steam's own idle timer, which ignores SDL's
 * screensaver handling but honours org.freedesktop.ScreenSaver inhibitions
 * (verified on a Steam Deck with DeckVideoPlayer). This helper asks for one
 * there, and through the org.freedesktop.portal.Inhibit portal (desktop
 * sessions), then stays connected: both inhibitions last until it exits.
 *
 *   --status FILE   write the outcome to FILE ("request: …"), for the
 *                   About screen
 *
 * The app starts it while a book is being read and stops it (SIGTERM) when
 * the book is closed or after a while without input; it also exits when
 * its parent dies (PR_SET_PDEATHSIG). On SIGTERM it withdraws the
 * screensaver inhibition with UnInhibit before disconnecting, so the
 * release doesn't depend on the service noticing the disconnection.
 *
 * Unlike dvp-keep-awake it never wakes up while it holds the inhibition
 * (DeckReader's D1: no work while idle; it runs for whole reading
 * sessions): SIGTERM and SIGINT are blocked and read from a signalfd that
 * is polled together with the bus, so no periodic check is needed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

static const char *status_file;
static char portal_result[96] = "";
static char screensaver_result[96] = "";
static sigset_t stop_signals;   /* SIGTERM, SIGINT: blocked, read from a signalfd */
static pid_t parent;

/* Told to stop (or orphaned) while a call was in flight: the app may have
 * removed the status file already, and a new helper may be using it. */
static int stopping(void)
{
    sigset_t pending;
    sigpending(&pending);
    return sigismember(&pending, SIGTERM) || sigismember(&pending, SIGINT) || getppid() != parent;
}

static void write_status(void)
{
    if (!status_file || stopping()) return;
    char tmp[4096];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", status_file);
    if (n < 0 || (size_t)n >= sizeof tmp) return;
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "request: screensaver %s, portal %s\n", screensaver_result, portal_result);
    if (fclose(f) == 0) rename(tmp, status_file);
}

/* poll() timeout until sd-bus's next deadline (usually none: -1). */
static int bus_timeout_ms(sd_bus *bus)
{
    uint64_t until;
    if (sd_bus_get_timeout(bus, &until) < 0 || until == UINT64_MAX) return -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
    if (until <= now) return 0;
    uint64_t ms = (until - now + 999) / 1000;
    return ms > 60000 ? 60000 : (int)ms;
}

static const char *short_error(const sd_bus_error *e, int r)
{
    if (e && e->name) {
        if (strcmp(e->name, SD_BUS_ERROR_SERVICE_UNKNOWN) == 0) return "not running";
        if (strcmp(e->name, SD_BUS_ERROR_UNKNOWN_METHOD) == 0 ||
            strcmp(e->name, SD_BUS_ERROR_UNKNOWN_INTERFACE) == 0 ||
            strcmp(e->name, SD_BUS_ERROR_UNKNOWN_OBJECT) == 0) return "not supported";
        if (strcmp(e->name, SD_BUS_ERROR_ACCESS_DENIED) == 0) return "access denied";
        if (strcmp(e->name, SD_BUS_ERROR_TIMEOUT) == 0 ||
            strcmp(e->name, SD_BUS_ERROR_NO_REPLY) == 0) return "timed out";
        return e->name;
    }
    return strerror(-r);
}

/* The portal answers through a Response signal on the request object if
 * the backend fails; 0 means the inhibition is in place. */
static int on_portal_response(sd_bus_message *m, void *data, sd_bus_error *err)
{
    (void)data; (void)err;
    uint32_t code = 0;
    if (sd_bus_message_read(m, "u", &code) >= 0) {
        snprintf(portal_result, sizeof portal_result, code == 0 ? "accepted" : "refused (%u)", code);
        write_status();
    }
    return 0;
}

/* Returns the screensaver cookie (0 if refused). */
static uint32_t request_inhibit(sd_bus *bus)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;

    /* A service being started on demand can be slow: don't wait the D-Bus
     * default of 25 s per call. */
    sd_bus_set_method_call_timeout(bus, 5 * 1000000ULL);

    /* The screensaver service first: what SteamOS game mode honours. The
     * status is written after each call, so the About screen shows it at
     * once. */
    uint32_t cookie = 0;
    snprintf(portal_result, sizeof portal_result, "pending");
    r = sd_bus_call_method(bus, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                           "org.freedesktop.ScreenSaver", "Inhibit", &err, &reply,
                           "ss", "DeckReader", "Reading a book");
    if (r >= 0 && sd_bus_message_read(reply, "u", &cookie) >= 0)
        snprintf(screensaver_result, sizeof screensaver_result, "accepted");
    else
        snprintf(screensaver_result, sizeof screensaver_result, "%s", short_error(&err, r));
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    reply = NULL;
    write_status();
    if (stopping()) return cookie;

    /* Portal (desktop sessions): subscribe to the predictable request path
     * first. */
    const char *unique = NULL;
    char sender[128] = "", path[256];
    if (sd_bus_get_unique_name(bus, &unique) >= 0 && unique && unique[0] == ':') {
        size_t j = 0;
        for (const char *p = unique + 1; *p && j < sizeof sender - 1; p++) sender[j++] = (*p == '.') ? '_' : *p;
        sender[j] = 0;
    }
    snprintf(path, sizeof path, "/org/freedesktop/portal/desktop/request/%s/deckreader_keep_awake", sender);
    sd_bus_match_signal(bus, NULL, "org.freedesktop.portal.Desktop", path,
                        "org.freedesktop.portal.Request", "Response", on_portal_response, NULL);
    /* flags: 4 = suspend, 8 = idle */
    r = sd_bus_call_method(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.Inhibit", "Inhibit", &err, &reply,
                           "sua{sv}", "", (uint32_t)(4 | 8), 2,
                           "handle_token", "s", "deckreader_keep_awake",
                           "reason", "s", "Reading a book");
    /* Unless the Response signal already arrived with the outcome. */
    if (strcmp(portal_result, "pending") == 0)
        snprintf(portal_result, sizeof portal_result, "%s", r < 0 ? short_error(&err, r) : "requested");
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    write_status();
    return cookie;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--status") == 0 && i + 1 < argc) status_file = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--status FILE]\n", argv[0]);
            return 2;
        }
    }
    /* The stop signals are only read from the signalfd: blocked first, so
     * one that arrives early (PDEATHSIG included) waits there. */
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGINT);
    sigprocmask(SIG_BLOCK, &stop_signals, NULL);
    int sfd = signalfd(-1, &stop_signals, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0) return 1;
    /* Exit with the app: PDEATHSIG covers a crash; the parent check covers
     * the window before prctl (the parent is then already gone). */
    parent = getppid();
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent) return 0;

    sd_bus *bus = NULL;
    int r = sd_bus_open_user(&bus);
    if (r < 0) {
        snprintf(portal_result, sizeof portal_result, "no session bus");
        snprintf(screensaver_result, sizeof screensaver_result, "no session bus");
        write_status();
        return 1;
    }
    uint32_t cookie = request_inhibit(bus);

    /* Sleep until the bus or a stop signal needs attention: no wake-ups
     * while the inhibition is held. */
    while (!stopping()) {
        while ((r = sd_bus_process(bus, NULL)) > 0) {}
        if (r < 0) break;   /* bus gone (e.g. session ended): nothing to hold */
        struct pollfd fds[2] = {
            {.fd = sd_bus_get_fd(bus), .events = (short)sd_bus_get_events(bus)},
            {.fd = sfd, .events = POLLIN},
        };
        r = poll(fds, 2, bus_timeout_ms(bus));
        if (r < 0 && errno != EINTR) break;
        if (fds[1].revents) break;   /* SIGTERM / SIGINT / parent died */
    }
    if (cookie) {
        /* Short: the app is waiting for nothing, but a hung service must
         * not keep this process around. */
        sd_bus_set_method_call_timeout(bus, 1000000ULL);
        sd_bus_call_method(bus, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                           "org.freedesktop.ScreenSaver", "UnInhibit", NULL, NULL, "u", cookie);
    }
    sd_bus_flush_close_unref(bus);   /* drops the portal inhibition too */
    return 0;
}
