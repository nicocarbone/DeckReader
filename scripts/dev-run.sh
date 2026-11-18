#!/bin/sh
# Run the development build in a window (inside the SDK, like the Flatpak).
#
#   scripts/dev-run.sh [deckreader options] [book.epub]
#   BOOKS=/path/to/books scripts/dev-run.sh --window
#
# Uses .dev/run/{config,data,cache} so your real settings and reading
# positions are never touched. Keyboard: arrows, Enter (A), Backspace (B),
# x, y, m (Menu), i (View), PgUp/PgDn (L1/R1), Home/End (L2/R2), Ctrl+Q.

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BOOKS=${BOOKS:-$root/.dev/sample_ebooks}
run=$root/.dev/run
if [ ! -x "$root/.dev/app/bin/deckreader" ]; then
    echo "Build first: scripts/dev-build.sh" >&2
    exit 1
fi
mkdir -p "$run/config" "$run/data" "$run/cache"
if [ ! -f "$run/config/config.ini" ]; then
    books_esc=$(printf '%s' "$BOOKS" | sed 's/[\\&|]/\\&/g')
    sed -e "s|^folder = .*|folder = $books_esc|" "$root/data/config.ini" > "$run/config/config.ini"
fi
[ $# -eq 0 ] && set -- --window
exec env GUI=1 "$root/scripts/sdk-run.sh" env \
    DECKREADER_CONFIG_DIR="$run/config" DECKREADER_DATA_DIR="$run/data" DECKREADER_CACHE_DIR="$run/cache" \
    DECKREADER_APPDIR="$root/.dev/app/share/deckreader" \
    "$root/.dev/app/bin/deckreader" "$@"
