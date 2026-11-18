#!/bin/sh
# Print the CHANGELOG.md section of a release (default: the version in
# CMakeLists.txt), without its heading. CI uses it as the GitHub release
# text; fails if the section is missing or empty, so every release has one
# (D27, as DeckVideoPlayer's D41).
#   scripts/release-notes.sh [v0.1.0 | 0.1.0]
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=${1:-$(sed -n 's/^project(deckreader VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")}
version=${version#v}
# Sections start with "## 0.1.0 (2026-11-20)" ("## v0.1.0" is accepted too).
notes=$(awk -v v="$version" '
    /^## / { if (found) exit; found = ($2 == v || $2 == "v" v); next }
    found' "$root/CHANGELOG.md" | sed -e '/./,$!d')
if [ -z "$(printf '%s' "$notes" | tr -d '[:space:]')" ]; then
    echo "CHANGELOG.md has no section for $version" >&2
    exit 1
fi
printf '%s\n' "$notes"
