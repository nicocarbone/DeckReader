#!/bin/sh
# Make the README screenshots (docs/screenshots/) at the Steam Deck's
# 1280x800, headless, with the development build (scripts/dev-build.sh).
#
#   tools/make-screenshots.sh
#
# The books are public-domain ebooks from Standard Ebooks
# (standardebooks.org, CC0), downloaded once into .dev/screens/home/Books.
# The app runs with HOME=.dev/screens/home, so the library shows "~/Books".
# A first run reads five of them to different points (the start screen's
# "Recently read"), a second one takes the pictures. Look at every picture
# before committing: a change in the app can move what a key press reaches.
# Needs curl and Python with Pillow on the host.

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
home=$root/.dev/screens/home
books=$home/Books
shots=$root/.dev/screens/shots
out=$root/docs/screenshots

# Standard Ebooks paths (author/title[/translator or illustrator]).
BOOKS="herman-melville/moby-dick
jane-austen/pride-and-prejudice
mary-shelley/frankenstein
bram-stoker/dracula
miguel-de-cervantes-saavedra/don-quixote/john-ormsby
lewis-carroll/alices-adventures-in-wonderland/john-tenniel
arthur-conan-doyle/the-adventures-of-sherlock-holmes
h-g-wells/the-time-machine"

mkdir -p "$books" "$shots" "$out"
for p in $BOOKS; do
    f=$(printf '%s' "$p" | tr / _).epub
    [ -s "$books/$f" ] && continue
    echo "Downloading $f"
    curl -fsSL -o "$books/$f.part" "https://standardebooks.org/ebooks/$p/downloads/$f?source=download"
    mv "$books/$f.part" "$books/$f"
done
rm -f "$shots"/*.png

export HOME_DIR="$home" BOOKS="$books" TIMEOUT=240

# One command per action; "n key" repeats a key n times.
rep() { s=""; i=0; while [ "$i" -lt "$1" ]; do s="$s key:$2;"; i=$((i + 1)); done; printf '%s' "$s"; }
# In the Books folder (sorted by title: 0 Alice, 1 Don Quixote, 2 Dracula,
# 3 Frankenstein, 4 Moby Dick, 5 Pride and Prejudice, 6 Sherlock Holmes,
# 7 The Time Machine): open book n.
open_book() { printf 'key:l2; %s key:confirm; wait:1000; idle;' "$(rep "$1" down)"; }
# Back to the library once the chapter name pill is gone (B would close it).
close_book="wait:2500; key:back; idle;"
# Waits until a theme or chapter pill is gone.
settle="wait:2500; idle;"

# 1. Reading history, oldest first; R2 = next chapter.
tools/headless-run.sh -- "idle; key:confirm; idle;
    $(open_book 1) $(rep 6 r2) $close_book
    $(open_book 2) $(rep 20 r2) $close_book
    $(open_book 3) $(rep 8 r2) $close_book
    $(open_book 5) $(rep 40 r2) $close_book
    $(open_book 4) $(rep 45 r2) $close_book
    quit"

# 2. The pictures.
KEEP=1 tools/headless-run.sh -- "idle; shot:$shots/start.png;
    key:up; key:confirm; idle; key:l2; $(rep 4 down) idle; shot:$shots/library.png;
    key:confirm; wait:1000; idle; $settle shot:$shots/reading.png;
    key:confirm; idle; shot:$shots/overlay.png; key:back;
    key:view; idle; shot:$shots/contents.png; key:back;
    key:menu; idle; shot:$shots/settings.png; key:back; $settle
    $close_book
    $(open_book 5) key:menu; $(rep 3 down) key:right; key:back; key:y; key:y; wait:1500; $settle
    shot:$shots/sepia.png;
    $close_book
    $(open_book 0) key:y; $(rep 4 r2) $(rep 2 right) $settle shot:$shots/day.png;
    stick:0.45; wait:1500; shot:$shots/scrub.png; stick:0;
    quit"

# 3. Into docs/screenshots, losslessly recompressed.
python3 - "$shots" "$out" <<'PY'
import os, sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
for name in ("start", "library", "reading", "overlay", "contents", "settings", "sepia", "day", "scrub"):
    Image.open(os.path.join(src, name + ".png")).convert("RGB").save(os.path.join(dst, name + ".png"), optimize=True)
    print("%-13s %4d KB" % (name + ".png", os.path.getsize(os.path.join(dst, name + ".png")) // 1024))
PY
