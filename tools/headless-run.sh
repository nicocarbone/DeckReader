#!/bin/sh
# Run DeckReader without a visible window, drive it with a test script
# (src/testscript.h), then quit. Used for screenshots and smoke tests.
#
#   tools/headless-run.sh [-- "script"]
#
# Options (environment variables):
#   BACKEND=offscreen|gamescope  offscreen = SDL's offscreen driver with the
#                      software renderer (default; needs nothing else),
#                      gamescope = a headless gamescope, like game mode
#   MODE=dev|flatpak   dev = .dev/app (scripts/dev-build.sh, default),
#                      flatpak = the installed io.github.nicocarbone.DeckReader
#   BOOKS=DIR          library folder (default: .dev/sample_ebooks)
#   HOME_DIR=DIR       HOME for the app (dev mode), so that a BOOKS folder
#                      inside it is shown as "~/..." (screenshots)
#   W=1280 H=800       screen size (try W=3840 H=2160 for a TV)
#   KEEP=1             keep reading state from the previous run
#   EXTRA_CONFIG=FILE  appended to the test config.ini, e.g. a [wallabag]
#                      section (the run has the network, like the Flatpak)
#   LOG=FILE           log file (default: .dev/headless.log)
#   TIMEOUT=60         hard time limit in seconds
#
# Examples:
#   tools/headless-run.sh -- "idle; shot:/tmp/library.png; quit"
#   tools/headless-run.sh -- "key:down; key:confirm; idle; shot:/tmp/page.png; quit"
#
# The test config/data folders are .dev/headless/{config,data}; the cache
# (covers, layouts) in .dev/headless/cache is kept between runs.

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BACKEND=${BACKEND:-offscreen}
MODE=${MODE:-dev}
BOOKS=${BOOKS:-$root/.dev/sample_ebooks}
W=${W:-1280}; H=${H:-800}
LOG=${LOG:-$root/.dev/headless.log}
TIMEOUT=${TIMEOUT:-60}
CFG=$root/.dev/headless

[ "${1:-}" = "--" ] && shift
script=${1:-"idle; shot:$root/.dev/shot.png; quit"}

if [ "${KEEP:-0}" = 1 ]; then rm -rf "$CFG/config"; else rm -rf "$CFG/config" "$CFG/data"; fi
mkdir -p "$CFG/config" "$CFG/data" "$CFG/cache" "$(dirname "$LOG")"
books_esc=$(printf '%s' "$BOOKS" | sed 's/[\\&|]/\\&/g')
sed -e "s|^folder = .*|folder = $books_esc|" \
    -e "s|^removable_media = .*|removable_media = no|" \
    "$root/data/config.ini" > "$CFG/config/config.ini"
if [ -n "${EXTRA_CONFIG:-}" ]; then
    cat "$EXTRA_CONFIG" >> "$CFG/config/config.ini"
fi

envs="DECKREADER_CONFIG_DIR=$CFG/config DECKREADER_DATA_DIR=$CFG/data DECKREADER_CACHE_DIR=$CFG/cache"
[ -n "${HOME_DIR:-}" ] && envs="$envs HOME=$HOME_DIR"
args="--window --size ${W}x$H"
if [ "$BACKEND" = offscreen ]; then
    envs="$envs SDL_VIDEO_DRIVER=offscreen"
    args="$args --software"
fi

if [ "$MODE" = flatpak ]; then
    flags=""
    for e in $envs; do flags="$flags --env=$e"; done
    # shellcheck disable=SC2086
    set -- flatpak run --filesystem="$root/.dev" --filesystem="$BOOKS:ro" $flags \
        io.github.nicocarbone.DeckReader $args --script "$script"
else
    # shellcheck disable=SC2086
    export NET=1 # the Flatpak has the network (D31)
    set -- "$root/scripts/sdk-run.sh" env $envs DECKREADER_APPDIR="$root/.dev/app/share/deckreader" \
        "$root/.dev/app/bin/deckreader" $args --script "$script"
    [ "$BACKEND" = gamescope ] && export GUI=1
fi

status=0
if [ "$BACKEND" = gamescope ]; then
    # gamescope's own exit status says nothing: on some machines it crashes
    # while tearing down after the app quit ("Primary child shut down", as
    # DeckVideoPlayer's DEVELOPMENT.md notes). The app's status goes to a file.
    status_file=$LOG.status
    rm -f "$status_file"
    wrapper='"$@"; st=$?; echo "$st" > "$DR_STATUS"; exit "$st"'
    DR_STATUS=$status_file timeout "$TIMEOUT" gamescope --backend headless -W "$W" -H "$H" -- \
        sh -c "$wrapper" sh "$@" > "$LOG" 2>&1 || true
    status=$(cat "$status_file" 2>/dev/null || echo 124)
else
    timeout "$TIMEOUT" "$@" > "$LOG" 2>&1 || status=$?
fi
grep -E '^\[deckreader\]' "$LOG" || true
case "$status" in
    0) echo "deckreader exit status: 0" ;;
    124) echo "FAILED: still running after TIMEOUT=$TIMEOUT s" >&2; exit 1 ;;
    *) echo "FAILED: exit status $status (139 = segfault); log: $LOG" >&2; exit 1 ;;
esac
