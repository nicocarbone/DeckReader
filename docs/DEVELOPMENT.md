# Development

## 1. What you need

| Tool | Why |
|---|---|
| `flatpak` with `org.freedesktop.Sdk//26.08` | every build and test runs inside it (D7): `flatpak install --user flathub org.freedesktop.Sdk//26.08` |
| `git`, `curl`, `python3` | fetching the pinned sources, helper scripts |
| `org.flatpak.Builder` (or `flatpak-builder`) | the release bundle |
| `gamescope` (optional) | testing the game-mode path headless (`BACKEND=gamescope`) |
| `ffmpeg` with librsvg, `fontTools` (optional) | rendering artwork (`tools/make-artwork.py`), font checks |

Nothing else is installed on the host: compilers, SDL3, FreeType, HarfBuzz
etc. come from the SDK.

## 2. Build and run

```sh
scripts/dev-build.sh          # deps (first time: a few minutes) + app → .dev/app
scripts/dev-run.sh            # a window, books from .dev/sample_ebooks (BOOKS=dir to change)
scripts/dev-run.sh --window --size 3840x2160   # TV-sized window
tests/run.sh                  # all automated tests
scripts/build-flatpak.sh [--install]           # dist/DeckReader.flatpak
```

`scripts/dev-build.sh` reads the dependency pins from the Flatpak manifest
(`scripts/manifest_sources.py`), clones them into `.dev/src`, applies
`patches/`, builds them into `.dev/prefix` and the app into `.dev/build`,
installed like `/app` into `.dev/app`. `BUILD_TYPE=Debug` for a debug build;
`--clean` rebuilds the app from scratch.

Development runs use `.dev/run/{config,data,cache}`: your real settings and
reading positions are never touched.

Keyboard (same as DeckVideoPlayer): arrows, Enter = A, Backspace/Esc = B,
`x`, `y`, `m`/Tab = Menu, `i`/`v` = View, PgUp/PgDn = L1/R1, Home/End =
L2/R2, Ctrl+Q quits.

## 3. Headless runs and screenshots

```sh
tools/headless-run.sh -- "idle; shot:/tmp/library.png; quit"
tools/headless-run.sh -- "key:confirm; idle; key:down; key:confirm; idle; shot:/tmp/page.png; quit"
W=3840 H=2160 tools/headless-run.sh -- "..."        # TV size
BACKEND=gamescope tools/headless-run.sh -- "..."    # headless gamescope (X11 + OpenGL)
MODE=flatpak tools/headless-run.sh -- "..."         # the installed Flatpak
KEEP=1 tools/headless-run.sh -- "..."               # keep reading state from the last run
```

Script commands are documented in `src/testscript.h` (`key:`, `stick:`,
`tap:`, `wait:`, `idle`, `shot:`, `quit`); one runs per frame. The run uses
`.dev/headless/{config,data,cache}`; the cache is kept between runs (delete
`.dev/headless/cache` to test first-open times). The log is
`.dev/headless.log`.

**Visual changes:** take screenshots at 1280×800 **and** 3840×2160 and look
at them before finishing.

**README screenshots** (`docs/screenshots/`): `tools/make-screenshots.sh`.
It downloads eight public-domain books from Standard Ebooks (CC0) into
`.dev/screens/home/Books` once, runs the app with that folder as `HOME`
(so the library shows `~/Books`), reads five books to different points for
"Recently read", then takes the nine pictures and recompresses them. Look
at each one before committing: a change in the app can move what a key
press reaches. Never use `.dev/sample_ebooks` for pictures that are
published (copyrighted books).

## 4. Tests

`tests/run.sh` runs:

- `tests/unit_tests.cpp` (inside the SDK): config, settings, state JSON and
  rate-limited saving, atomic writes and corrupt files, archive checks,
  mountinfo, library sorting/listing, input mapping/repeat/triggers/stick,
  canvas blending; rendering tests on small generated books (variation
  selectors, line art on dark pages, D26); and, when `.dev/sample_ebooks`
  has an EPUB, integration tests that open, lay out, render and re-lay out
  real books.
- `tests/make-hostile-epubs.py`: writes the malformed archives and the
  small rendering books into `.dev/test-hostile` first.
- `tests/test_steam_shortcut.py` against a fake `HOME`.
- Consistency checks: the version in `CMakeLists.txt` has a metainfo
  `<release>` and a `CHANGELOG.md` section; manifest patches exist.

New pure logic gets unit tests. The unit tests' output goes to
`.dev/unit_tests.log`; the script fails when any check fails.

## 5. Measuring

```sh
# idle CPU and memory of a running instance (e.g. under a headless run)
PID=$(pgrep -x deckreader)
awk '{print $14+$15}' /proc/$PID/stat         # CPU ticks; sample twice
awk '/^Pss_Anon|^Pss_File/' /proc/$PID/smaps_rollup
```

The log prints the time to the first frame, engine start, book open and
layout times (`-v` adds per-page render times).

## 6. CI and releasing

`.github/workflows/build.yml` (D27), on every push to `main`, pull request
and tag:
- `tests`: installs Flatpak and the Freedesktop SDK on Ubuntu, then
  `scripts/dev-build.sh` and `tests/run.sh`. The engine tests on real books
  are skipped there (no sample books); the generated ones run.
- `flatpak`: builds the bundle in
  `ghcr.io/flathub-infra/flatpak-github-actions:freedesktop-26.08`.
- `release` (tags `v*`): checks that the tag is `v` + the version in
  `CMakeLists.txt`, then attaches to a GitHub release:
  `DeckReader.flatpak`; `DeckReader-<tag>.tar.gz`, what users download
  (bundle in `dist/`, installer scripts, Steam artwork, README, changelog,
  licence and notices); and `DeckReader-<tag>-sources.tar.gz`, the complete
  corresponding source the GPL requires (`scripts/make-source-archive.py`).
  The release text is the version's `CHANGELOG.md` section
  (`scripts/release-notes.sh`); the job fails if there is none.

To release:

1. Bump `project(... VERSION x.y.z)` in `CMakeLists.txt`; write the
   `## x.y.z (date)` section of `CHANGELOG.md` for users (new, changed,
   fixed: it becomes the release text; between releases it is collected
   under `## x.y.z (unreleased)`) and a `<release>` with the same date in
   the metainfo. Update `docs/ROADMAP.md` and the "Current state" of
   `AGENTS.md`.
2. `tests/run.sh` (fails while the changelog section or the metainfo
   entry is missing), screenshots at both sizes, `scripts/build-flatpak.sh`,
   `MODE=flatpak` headless run. `scripts/release-notes.sh` prints the text
   CI will publish.
3. Refresh the README screenshots if the interface changed
   (`tools/make-screenshots.sh`); the metainfo links them on GitHub.
4. Try the bundle on a Steam Deck (§7).
5. Commit, `git tag -a vx.y.z -m "DeckReader x.y.z"`, push the commit and
   the tag: CI publishes the release.

GitHub repository settings (set once, by hand; already done except the
social preview): the description "EPUB e-book reader for the Steam Deck and
SteamOS Game Mode: controller-first, OLED themes, two-page layout, Flatpak",
the topics `steam-deck`, `steamos`, `ebook-reader`, `epub`, `epub-reader`,
`ebooks`, `gamepad`, `controller`, `game-mode`, `flatpak`, `linux`,
`crengine`, `sdl3`, and a social preview image (Settings → Social
preview; `docs/screenshots/reading.png` works).

## 7. On-device checklist (Steam Deck)

Item 11 was confirmed by the owner for 0.2.0 (2026-11-18); the rest is
not yet done as a whole.

1. `scripts/install-steamos.sh` in Desktop Mode adds "DeckReader" to Steam
   with artwork; it starts in Game Mode, fullscreen, no cursor.
2. Controls: every button in REQUIREMENTS §5.1 does what the hint bar says;
   the left stick scrubs; touch zones turn pages.
3. Books on the microSD card appear as "SD Card".
4. Read a few pages, suspend with the power button, resume: same page. Hold
   the power button to force off mid-chapter, boot: at most the last page
   turn lost.
5. HUD shows the right battery and time.
6. Docked to a TV with Game Resolution = Native: pages are sharp; undock:
   the layout adapts and keeps the position.
7. Memory while reading (§5) under 100 MB of private memory; CPU near 0%
   with a page on screen; battery drain comparable to an idle Deck.
8. OLED themes: no visible smear when turning pages at low brightness.
9. Text rendering: compare Grayscale, Vertical RGB and Vertical BGR up
   close; keep Grayscale unless one vertical mode is clearly sharper (D21).
10. Change font, size and spacing several times in the middle of a book: the
    same words stay on screen.
11. Keep awake (D28): set Steam's "Dim screen after" to its shortest. With a
    book open and no input, the screen doesn't dim. Menu → About → *Keep
    awake* says "screensaver accepted". With `keep_awake_minutes = 1` in
    `config.ini`, the screen dims about a minute after the last press. In
    the library it dims as usual.

## 8. Gotchas

- `Segmentation fault` at the end of a `BACKEND=gamescope` run is gamescope
  tearing down (seen with NVIDIA, as in DeckVideoPlayer); the runner reports
  the app's own exit status.
- crengine logs "ERROR ... not found in cache index" on first opens: normal
  cache misses (hidden unless `-v`).
- The Flatpak's CMake modules need `builddir: true` (utf8proc refuses in-tree
  builds); post-install commands then run in the build folder (`../` paths).
- Fonts can map U+00AD to an empty glyph; crengine needed a patch (D3).
- Publishers' CSS colours: text colours are overridden (D14), or OLED themes
  show black-on-black pages.
