# Architecture

How DeckReader is put together. Read this first; then
[DECISIONS.md](DECISIONS.md) for the reasons, [DEVELOPMENT.md](DEVELOPMENT.md)
to build and test, and [../REQUIREMENTS.md](../REQUIREMENTS.md) for what it
must do.

## 1. Big picture

```
┌──────────────────────── SteamOS game mode ─────────────────────────┐
│ Steam ── launches the non-Steam shortcut ──┐                       │
│                                            ▼                       │
│   flatpak run io.github.nicocarbone.DeckReader                     │
│   └─ /app/bin/deckreader   (one process; one thread while reading) │
│        ├─ SDL3: window (X11 under gamescope), OpenGL renderer,     │
│        │        gamepad (Steam Input's virtual pad), touch         │
│        ├─ crengine-ng: EPUB → DOM → CSS → pages → pixels           │
│        ├─ FreeType + HarfBuzz: interface text                      │
│        ├─ while reading: deckreader-keep-awake ── D-Bus ──▶        │
│        │    org.freedesktop.ScreenSaver (Steam), Inhibit portal    │
│        └─ on request: articles thread ── HTTPS (libcurl) ──▶       │
│             wallabag (wallabag.it or the user's server)            │
│                                                                    │
│   page texture (opaque) + overlay texture (premultiplied alpha)    │
│        └─ gamescope ── panel / TV                                   │
└────────────────────────────────────────────────────────────────────┘
```

One process, no daemons. The process sleeps until input or a pending
timer (D1). While a book is being read, a small helper process holds the
screensaver inhibition that keeps the screen awake (D28); it sleeps too,
and is stopped in the library and after `keep_awake_minutes` without
input. When the user updates the wallabag articles or marks one as read,
a second thread talks to the server and wakes the loop with an SDL event
on progress and when done (D31); it touches no app state.

## 2. Modules (`src/`)

| File | Role |
|---|---|
| `main.cpp` | Options, SDL init (X11 under gamescope), runs `App` |
| `app.*` | Owns the services, the main loop, input routing, menus, toasts, rendering, test scripts |
| `library_screen.*` | Start screen, folders, recent books: list, header, hint bar, covers |
| `library.*` | What the library lists: sources, folders, recents, sorting, display titles |
| `metacache.*` | Titles/authors/covers per book, cached in `$XDG_CACHE_HOME/deckreader` |
| `reader_screen.*` | Reading: page textures and pre-rendering, navigation, HUD, scrub, bookmarks, Contents/Bookmarks/Settings drawers |
| `book.*` | crengine-ng wrapper: engine init, archive check, metadata, layout settings → crengine props and CSS, render a screen, positions, TOC |
| `epubguard.*` | ZIP directory checks (D5) |
| `menu.*` | Right-side drawer menus (rows, ◀ ▶ values, section headers, scroll, taps) |
| `about.*` | The About screen: version, build, links, system details for bug reports, credits (D29) |
| `keepawake.*` | Starts and stops the keep-awake helper while reading (D28) |
| `articles.*` | The Articles source: starts wallabag syncs, applies their results (state, removed articles, messages), the Articles row's status (D31) |
| `wallabag.*` | wallabag API client: token, unread list, articles, archive; pictures; the sync plan; the background thread (`SyncWorker`) (D31) |
| `epubwrite.*` | ZIP writer (zlib) and the article EPUB: page, pictures, cover; the article HTML's pictures (find, rewrite, types) (D31) |
| `http.*` | HTTP(S) requests through libcurl: limits, cancelling, error messages (D31) |
| `ui.*` | DeckVideoPlayer's look on the 800-unit canvas: text, shapes, button glyphs, icons, hint bar |
| `canvas.*` | CPU drawing: anti-aliased rounded rects, circles, polygons, glyph masks, images |
| `text.*` | Interface text: HarfBuzz shaping, FreeType glyphs, ellipsis, wrap |
| `display.*` | SDL window/renderer, page and overlay textures, present, screenshots |
| `input.*` | Gamepad/keyboard → actions, repeats, triggers, left-stick scrub |
| `state.*` | `state.json`: positions, bookmarks, reading settings; rate-limited atomic saves (D6) |
| `settings.*` | Reading settings, themes, fonts, labels |
| `config.*` | `config.ini` parsing (DeckVideoPlayer's rules) |
| `fsutil.*` | Atomic writes, folder listing, mountinfo (removable media) |
| `system.*` | Battery (`/sys/class/power_supply`), clock |
| `image_io.*` | PNG read/write (covers, screenshots) |
| `testscript.*` | Scripted input for headless tests (D17) |
| `util.*`, `log.*` | Strings, paths, natural sort, logging |

`helper/deckreader-keep-awake.c` is a separate C program (sd-bus): it asks
for the screensaver inhibition, holds it while it runs and withdraws it
when stopped (D28).

## 3. Runtime model

```
             start          A on a book                         B / Close book
 ┌─────────┐ first frame ┌──────────┐ "Opening…" frame  ┌────────┐ save position ┌──────────┐
 │  App    │ ──────────▶ │ LIBRARY  │ ─────────────────▶ │ READER │ ────────────▶ │ LIBRARY  │
 └─────────┘             └──────────┘   then open+layout └────────┘               └──────────┘
```

- `App::screen_` is Library or Reader; a **menu stack** may sit on top of
  either and then receives all input. Toasts are drawn over everything.
- Input routing: `Input` turns SDL events into actions (`up`, `confirm`,
  `back`, `x`, `y`, `menu`, `view`, `l1`...). `App::dispatch`: quit → B
  dismisses a toast first → top menu → reader or library.
- Opening a book is two steps: a frame with "Opening …" is presented, then
  the next loop iteration parses and lays out the book, so feedback is
  immediate even for a slow first open.

### The loop (`App::loop`)

1. Compute the timeout: 0 if a frame is due or background work is pending;
   otherwise the nearest of: held-button repeat, reader timers (re-layout,
   HUD clock, scrub), toast expiry, cursor hide, debounced save, the
   keep-awake release (minutes away, D28), script.
   −1 (wait forever) when nothing is pending.
2. Wait for an event; handle it and every queued one (the articles
   thread's events included: progress, then the result, D31).
3. Run one test-script command, then timers (`tick`).
4. Render if something changed.
5. Open a pending book (after its "Opening" frame was shown).
6. Save the state if due; then one piece of background work if no input
   is waiting: the library reads one book's metadata/cover; the reader
   pre-renders the next or previous screen.

## 4. Drawing

- **Pages:** crengine draws a screen (one page or two side by side) into a
  CPU buffer, uploaded once into a static texture. Three textures are kept:
  current, next, previous (D16). A page turn with a cached neighbour is a
  present of an existing texture. Theme changes recolour without re-layout.
- **Overlay:** one premultiplied-ARGB canvas the size of the screen, drawn
  with `Ui` on the 800-unit canvas and uploaded only when something changed.
  In the reader it is skipped entirely unless the HUD, a menu, a toast, the
  scrub panel or a bookmark ribbon is visible.
- **Library backdrop:** a 1/8-size gradient texture stretched by the GPU.

## 5. Book engine integration (`book.cpp`)

- `engine::init`: crengine log level, bundled fonts only (D9), hyphenation
  patterns from `/app/share/crengine-ng/hyph`, crengine's `epub.css`, and its
  layout cache in `$XDG_CACHE_HOME/deckreader/layout` (fast reopen).
- `Book::configure(settings, w, h, unit)`: maps reading settings to crengine
  properties (font face/size, AA, hinting, HarfBuzz shaping, margins, two
  pages, embedded styles/fonts, hyphenation by the book's language, render
  DPI) and to CSS rules appended to `epub.css` (D14); re-lays out only when
  something that affects layout changed, keeping the position.
- Positions are crengine XPointers (stable across fonts and sizes); page
  numbers are only for display.

## 6. Files at runtime

| Path | Content |
|---|---|
| `~/.config/deckreader/config.ini` | User settings (created from the template on first start) |
| `$XDG_DATA_HOME/deckreader/state.json` (Flatpak: `~/.var/app/io.github.nicocarbone.DeckReader/data/deckreader/`) | Reading settings, positions, bookmarks, recent books, wallabag ids waiting to be archived and the last update time |
| `$XDG_DATA_HOME/deckreader/articles/<id>.epub` | wallabag articles (D31) |
| `$XDG_CACHE_HOME/deckreader/metadata.json`, `covers/*.png` | Library titles/authors/covers |
| `$XDG_CACHE_HOME/deckreader/layout/` | crengine's laid-out books |
| `/app/share/deckreader/` | `config.ini` template, `fonts/`, `build-info` |
| `/app/share/crengine-ng/` | `epub.css` and the other stylesheets, `hyph/` patterns |

`state.json`:

```json
{"version": 1,
 "settings": {"theme": "oled-black", "font": "literata", "font_size": 24, "line_spacing": 14, ...},
 "books": {"/home/deck/Books/Don Quijote.epub": {
    "xpointer": "/body/DocFragment[7]/body/div/p[12]/text().0",
    "percent": 3214, "page": 96, "pages": 282, "t": 1795450000123,
    "title": "Don Quijote de la Mancha", "author": "Miguel de Cervantes",
    "bookmarks": [{"xpointer": "...", "percent": 120, "chapter": "Prólogo", "t": 1795449000}]}}}
```

## 7. Packaging

- Flatpak `io.github.nicocarbone.DeckReader` on `org.freedesktop.Platform`
  26.08. Modules: libunibreak, utf8proc, crengine-ng (static, with five
  patches: visible soft hyphen D3, ZIP hardening and no external fonts D22,
  invisible variation selectors and line art on dark pages D26),
  the fonts, the app. SDL3, FreeType, HarfBuzz, FriBiDi, libpng, libjpeg,
  libwebp, zstd, zlib, libsystemd and libcurl come from the runtime.
- Sandbox (D4): X11 + Wayland, IPC, `--device=all` (GPU and controllers),
  book folders read-only, `xdg-config/deckreader`, the ScreenSaver bus
  name (D28), the network for wallabag only (D31).
- Releases: CI builds the bundle, the installer archive and the GPL
  source archive from a version tag (`.github/workflows/build.yml`, D27).
- SteamOS: `scripts/install-steamos.sh` installs the bundle per user and adds
  the non-Steam shortcut with artwork (`scripts/steam_shortcut.py`, shared
  with DeckVideoPlayer).
