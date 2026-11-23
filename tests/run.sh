#!/bin/sh
# Run every automated test (must stay green):
#   * C++ unit tests (tests/unit_tests.cpp) inside the SDK; with books in
#     .dev/sample_ebooks the engine integration tests open real ones,
#     tests/make-hostile-epubs.py makes malformed archives to refuse, and
#     the wallabag sync runs against tests/fake-wallabag.py (no network: the
#     SDK sandbox has only its own loopback)
#   * the Steam shortcut tool's tests (Python, against a fake HOME)
#   * consistency checks between the version, the metainfo and the manifest
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"

if [ ! -x .dev/build/unit_tests ]; then
    echo "Build first: scripts/dev-build.sh" >&2
    exit 1
fi
# Real books for the engine integration tests (the owner's, never in the
# repository; CI has none and those tests are skipped).
samples=${SAMPLE_BOOKS:-$root/.dev/sample_ebooks}
book=$(ls "$samples"/*.epub 2>/dev/null | head -1 || true)
hostile=$root/.dev/test-hostile
rm -rf "$hostile"
python3 "$here/make-hostile-epubs.py" "$hostile"
# The output goes through a file: in a pipe, the exit status would be grep's
# and failures would pass unnoticed.
log=$root/.dev/unit_tests.log
status=0
scripts/sdk-run.sh env \
    DECKREADER_TEST_HOSTILE_DIR="$hostile" \
    DECKREADER_TEST_BOOK="$book" \
    DECKREADER_TEST_BOOKS_DIR="$( [ -n "$book" ] && echo "$samples" )" \
    DECKREADER_TEST_FONTS="$root/.dev/app/share/deckreader/fonts" \
    DECKREADER_TEST_ENGINE_DATA="$root/.dev/prefix/share/crengine-ng" \
    DECKREADER_TEST_FAKE_WALLABAG="$here/fake-wallabag.py" \
    DECKREADER_TEST_FAKE_WIKIPEDIA="$here/fake-wikipedia.py" \
    DECKREADER_TEST_WIKI_PAGES="$( [ -d "$root/.dev/wiki-pages" ] && echo "$root/.dev/wiki-pages" )" \
    DECKREADER_TEST_TEMML="$root/.dev/app/share/deckreader/js/temml.min.js" \
    .dev/build/unit_tests > "$log" 2>&1 || status=$?
grep -v -E '^[0-9/]+ [0-9:.]+ (WARN|ERROR) ' "$log" || true
[ "$status" = 0 ] || { echo "unit tests FAILED (exit status $status)" >&2; exit 1; }

python3 -m unittest -q "$here/test_steam_shortcut.py"

# The version lives in CMakeLists.txt; the metainfo must list it.
version=$(sed -n 's/^project(deckreader VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
grep -q "release version=\"$version\"" packaging/flatpak/io.github.nicocarbone.DeckReader.metainfo.xml ||
    { echo "metainfo.xml has no <release version=\"$version\">" >&2; exit 1; }
# ...and the changelog a section for it (the GitHub release text, D27).
scripts/release-notes.sh "$version" > /dev/null
# Every manifest patch must exist.
python3 scripts/manifest_sources.py packaging/flatpak/io.github.nicocarbone.DeckReader.yml |
    awk -F'\t' '$1 == "patch" {print $3}' | while read -r p; do
        [ -f "$p" ] || { echo "missing patch: $p" >&2; exit 1; }
    done
echo "all tests passed"
