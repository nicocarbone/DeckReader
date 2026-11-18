# AGENTS.md: orientation for AI agents and new contributors

DeckReader is a controller-driven EPUB reader for the Steam Deck and
SteamOS game mode, companion to DeckVideoPlayer
(`../DeckVideoPlayer`, github.com/nicocarbone/DeckVideoPlayer). It is
**C++17 + crengine-ng (book engine) + SDL3**, shipped as a Flatpak. Read
`REQUIREMENTS.md` and `docs/ARCHITECTURE.md` before changing anything
non-trivial.

## Where things are

| Need to change… | Look at |
|---|---|
| Library screen (rows, header, hints, covers) | `src/library_screen.cpp` |
| What the library lists, sorting, titles | `src/library.cpp`, `src/metacache.cpp` |
| Reader: navigation, HUD, scrub, bookmarks, drawers | `src/reader_screen.cpp` |
| How settings reach the book (props, CSS overrides) | `src/book.cpp` (`Book::configure`, `make_css`) |
| Reading settings, themes, fonts, labels | `src/settings.cpp` |
| Menus/drawers component | `src/menu.cpp` |
| Drawing primitives, glyphs, icons, interface colours (teal accent, D20) | `src/ui.h` (`UiTheme`), `src/ui.cpp`, `src/canvas.cpp`, `src/text.cpp` |
| Button → action mapping | `src/input.cpp` |
| Main loop, routing, toasts, timers, About | `src/app.cpp` |
| Saving (state.json), atomic writes | `src/state.cpp`, `src/fsutil.cpp` |
| EPUB archive checks | `src/epubguard.cpp` |
| config.ini | `src/config.cpp`, `data/config.ini`, `docs/CONFIGURATION.md` |
| Flatpak, dependency pins, engine patch | `packaging/flatpak/io.github.nicocarbone.DeckReader.yml`, `patches/` |
| Steam integration | `scripts/install-steamos.sh`, `scripts/steam_shortcut.py` (shared with DeckVideoPlayer), `packaging/steam/`, `tools/make-artwork.py` |

Reasons behind each choice: `docs/DECISIONS.md` (add an entry when you make
a new one; cite it as Dn in comments). What comes next: `docs/ROADMAP.md`.

## Commands

```sh
scripts/dev-build.sh                       # build deps + app inside the SDK → .dev/app
scripts/dev-run.sh                         # run in a window (books: .dev/sample_ebooks)
tests/run.sh                               # all tests (must stay green)
tools/headless-run.sh -- "idle; shot:/tmp/lib.png; quit"   # screenshot, no display
W=3840 H=2160 tools/headless-run.sh -- "..."               # at TV size
BACKEND=gamescope tools/headless-run.sh -- "..."           # headless gamescope
scripts/build-flatpak.sh [--install]       # dist/DeckReader.flatpak
MODE=flatpak tools/headless-run.sh -- "..."                # test the installed Flatpak
```

Everything builds and runs inside `org.freedesktop.Sdk//26.08`
(`scripts/sdk-run.sh`); install nothing on the host.

Visual changes: take screenshots at 1280×800 **and** `W=3840 H=2160` and
look at them before finishing.

## Rules of the codebase

- **No work while idle** (battery): the loop waits for events; a timer runs
  only while something needs it; frames are drawn only on change (D1).
- **Consistency with DeckVideoPlayer**: same buttons for shared actions
  (D8), same look (colours, Inter, pills, hint bar, drawers), same packaging
  and installer. If you change a button, update the hint bars, the
  requirements table (§5.1) and D8.
- Positions are crengine XPointers, never page numbers (layouts change).
- Every write of user data goes through `write_file_atomic` (D6).
- Books are untrusted input: every file goes through `check_book_file`
  first (D5), and crengine-ng is patched so the sizes checked are the sizes
  read (D22). Hostile fixtures: `tests/make-hostile-epubs.py`.
- Menus may be closed from their own callbacks: use `App::pop_menu` /
  `close_menus`, never destroy a menu directly (D18).
- New pure logic gets unit tests in `tests/unit_tests.cpp`.
- Every config key: `src/config.cpp`, the `data/config.ini` template and
  `docs/CONFIGURATION.md`.
- Dependency versions live only in the Flatpak manifest; adding or
  upgrading one: update `THIRD_PARTY_NOTICES.md`.
- Don't touch the user's real `~/.config/deckreader` or Steam files in
  tests: the dev and headless runs use `.dev/`; `steam_shortcut.py` is
  tested against a fake `HOME`.
- `.dev/sample_ebooks` holds the owner's books for testing: never commit
  them or their contents.

## Current state (2026-11-17, v0.1.0, not released)

- First version written and tested headless (offscreen and gamescope, at
  1280×800 and 4K) with nine real EPUBs (Spanish and English): all open
  (first open 0.1–0.25 s on the dev PC, reopen ~25 ms from the cache).
- Idle reading: ~0% CPU. Memory: ~88 MB private on the dev PC (NVIDIA);
  not yet measured on a Deck.
- Flatpak bundle builds (3 MB) and runs (`MODE=flatpak`).
- First tried by the owner on a Steam Deck OLED (2026-11-17, test1):
  reported font changes moving the position / not applying (fixed: anchor
  and font bias, D14), no visible effect of subpixel modes (expected; D21),
  too-subtle sepia and too-similar blue (D20). The rest of the on-device
  checklist (docs/DEVELOPMENT.md §7) is still open.
- First audit (ISSUES.md, Gemini) reviewed in D22: fixed except BUG-1 and
  BUG-2 (measured/verified not to happen) and three minor declines.
  crengine-ng now carries three patches (`patches/`).
