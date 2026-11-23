# Decisions

Why DeckReader is built the way it is. Each entry: the decision, the
reason, and what was considered instead. Add an entry when you make a new
choice; refer to it as Dn in code comments. DeckVideoPlayer's decisions are
cited as "DeckVideoPlayer Dn".

## D1. Event-driven loop; a frame is two textured quads

The main loop sleeps in `SDL_WaitEventTimeout` with a timeout only when a
timer is really pending (a held button's repeat, a toast, the HUD clock,
a debounced save, a re-layout, the left-stick scrub, the keep-awake
release of D28). The wallabag thread (D31) wakes it with an event, not a
timer. Nothing renders while
a page is just being read: measured 1 CPU tick (10 ms) in 5 s with a page
on screen. A frame is the page texture (opaque) plus, only when something
is shown on top, the interface overlay (premultiplied alpha). Requirements
3.3 and 7.1.

## D2. crengine-ng as the book engine

crengine-ng (the CoolReader engine, GPL-2.0-or-later) parses EPUB 2/3,
applies CSS, lays out pages with hyphenation and draws them into a buffer.
It is what KOReader uses for reflowable books.
Considered: MuPDF (AGPL, excluded by requirements 3.2; weaker CSS and
hyphenation for reflowable text), litehtml (HTML/CSS only: pagination, EPUB
and hyphenation would be ours to write), WebKit (requirements 3.1).

## D3. crengine-ng pinned after 0.9.15, with one patch

The manifest pins the commit after the 0.9.15 tag because it fixes a buffer
overflow in the CSS attribute parser (books are untrusted input).
`patches/crengine-ng-visible-soft-hyphen.patch`: Literata maps U+00AD (soft
hyphen) to an empty, zero-width glyph, which is correct inside words, but
crengine also uses that glyph for the hyphen it draws when it breaks a word
at a line end: hyphenated lines had no visible hyphen. The patch uses the
font's "-" when the soft hyphen glyph has no width. Worth sending upstream.
Both the Flatpak and `scripts/dev-build.sh` apply it.

## D4. Flatpak sandbox: book folders read-only, not the home folder

`~/Books`, Documents, Downloads and removable media, read-only (v0.1
never writes there); settings in the real `~/.config/deckreader`. As in
DeckVideoPlayer D39: a malicious book exploiting a parser bug must not be
able to read SSH keys or other apps' data. No network permission until
v0.3, which adds it for the wallabag articles only (D31). Other folders:
`flatpak override` (documented in `config.ini`).

## D5. EPUB archives are checked before parsing

`src/epubguard.cpp` reads the ZIP directory (through crengine's archive
code, not a parser of our own) and refuses names with `..`, absolute paths
or drive letters, nested archives, entries over 256 MB, over 1 GB in total,
over 20 000 entries, entries from 1 MB compressed more than 200:1, and
archives from 16 MB compressed more than 50:1 overall (D22). crengine is
patched so these central-directory sizes are the ones it decodes with, and
never more (D22). crengine reads
entries into memory and never extracts to disk, so "Zip Slip" cannot write
anywhere; the checks keep deliberately malformed files out and stop
decompression bombs from exhausting memory. Metadata reading (covers,
titles) goes through the same check.

## D6. Saving: atomic, fsync'd, rate-limited, on every page turn

`write_file_atomic`: temporary file in the same folder, `fsync`, rename,
`fsync` of the folder. A rename without `fsync` can leave an empty file
after a power cut on ext4. The state file is written on the first change
after a quiet moment and then at most every 2 s, so flipping pages doesn't
write dozens of times; also on focus loss (Steam overlay), closing a book
and exit. An unreadable state file is renamed `state.json.corrupt`, never
overwritten. JSON like DeckVideoPlayer, not SQLite: one small file, easy to
inspect.

## D7. Development builds inside the Freedesktop SDK

`scripts/sdk-run.sh` runs build tools and the app inside
`org.freedesktop.Sdk//26.08`, the SDK the Flatpak is built with. Nothing to
install on the host, and the development build links exactly the libraries
users get (SDL3 3.4, FreeType, HarfBuzz...). Dependency versions are pinned
once, in the Flatpak manifest; `scripts/dev-build.sh` reads them through
`scripts/manifest_sources.py`.

## D8. Controller map

Shared actions keep DeckVideoPlayer's buttons (its D10): A confirm / show
the overlay, B back (dismissing what is on screen first, its D63), Menu for
menus, View for the info/contents panel, L2/R2 for chapters, the same
keyboard keys. Reader-only actions use the buttons that playback-only
actions use in DeckVideoPlayer: X bookmark (subtitles there), Y theme (audio
there). D-pad ↑↓ also skip chapters (the original requirement; volume has
no meaning in a reader). The left stick scrubs in the reader (hold:
accelerating, 2 to 62 pages per second; a flick turns one screen); in lists
it acts as the D-pad, as in DeckVideoPlayer. Taps work (reader zones,
list rows), unlike DeckVideoPlayer, because a reader's tap zones are
unambiguous. On the start screen Y does nothing except on the Articles
row, where it updates the articles (D31); in folders it sorts.

## D9. Bundled fonts only

Literata (serif, made for e-reading), Inter (sans-serif; DeckVideoPlayer's
interface font) and Atkinson Hyperlegible, all OFL. crengine registers only
these (`InitFontManager(..., false)`): system fonts would make the look
depend on the host and fontconfig scans slow down the start. Literata and
Inter are the fallbacks for missing glyphs. Not covered yet: CJK, Arabic,
Hebrew scripts (roadmap).

## D10. Removable media from /proc/self/mountinfo

As DeckVideoPlayer D14/D33: mounts strictly below `/run/media` or `/media`,
named "SD Card" (mmcblk devices) or "USB drive", with the volume name only
when two would look the same. The per-user folder that the Flatpak
bind-mounts (`/run/media/deck`) is skipped.

## D11. config.ini for the library, state.json for reading choices

`~/.config/deckreader/config.ini` holds what users set by editing a file
(folders, removable media, interface scale, clock, accent, tap zones), with
DeckVideoPlayer's parsing rules. Reading settings change often (theme, font,
size...), so the reader's menu changes them and they are remembered in
`state.json` with positions and bookmarks. One place each: the menu never
rewrites the user's config file.

## D12. Sizes in units of the 800-unit canvas

Font sizes and margins are in units of a canvas 800 units tall (pixels on
the Deck), multiplied by screen height / 800 for the page, and crengine's
render DPI is scaled the same way (96 × factor) so CSS absolute units follow.
Docking to a 4K TV keeps the same text per screen, like DeckVideoPlayer's
interface (its D11). "14–36 pt" in the first requirements meant this.

## D13. C++, not Rust

crengine-ng's API is C++ (classes, its own strings and smart pointers) with
no Rust bindings; Rust would need a C wrapper plus bindings, an extra layer
to maintain, and the risky parsing would still be C++ inside crengine. Our
own code stays small, uses no manual memory management, and its logic is
unit-tested.

## D14. Typography defaults and how settings reach the book

- Two pages side by side by default: on a 16:10 screen one column would be
  ~95 characters wide; two give ~50, like a book. crengine falls back to one
  page when the text is too large for two.
- Justified lines may shrink spaces to 75% (crengine's default of 50% glued
  words together; KOReader uses 75%).
- Line spacing, paragraph spacing and alignment are CSS rules appended to
  crengine's stock `epub.css` with `!important`, so they win over the book's
  CSS. Each has a "Book" value that adds nothing. Alignment applies to `p`
  and `li` only, so headings keep the book's alignment.
- The chosen font also replaces the book's generic families (serif,
  sans-serif) and embedded fonts are off, unless "Book's fonts" is chosen.
- Settings changes that need a new layout are applied 250 ms after the last
  press, so holding ▶ on the font size doesn't lay the book out ten times;
  "Updating pages…" shows meanwhile and during the layout.
- **The reading position is an anchor** (`Book::anchor_`): the start of the
  screen the reader navigated to. Re-layouts restore that same anchor
  instead of re-reading the start of whatever page they landed on: in
  two-page mode crengine snaps to the left page of a spread, so re-reading
  lost up to a page per change and font changes in a row drifted backwards
  (up to six pages on the sample books; found on the first Deck test). The
  `engine_position_stability` test checks every sample book.
- **The chosen font is preferred** (`SetAsPreferredFontWithBias`, bias
  25600): books name fonts they embed ("Garamond", no generic family); with
  embedded fonts off, crengine scored our fonts by family type, so Literata
  (serif) won over a chosen sans font and the font menu seemed to do
  nothing. The bias beats the family-type difference (192 × 100) but not a
  name match (256 × 1000), so "Book's fonts" still uses embedded fonts.

## D15. Interface drawn on a CPU canvas

Library, HUD, menus and toasts are drawn in software into one premultiplied
ARGB canvas (rounded shapes anti-aliased with distance functions, text with
FreeType + HarfBuzz) and uploaded only when they change. It reproduces
DeckVideoPlayer's libass-drawn look without a GUI toolkit; the same code
runs identically on the GPU renderer and SDL's software renderer (tests).

## D16. Memory: static textures, a small backdrop

Textures are static (updated whole), so SDL keeps no CPU copy of them
(streaming textures keep a lock buffer each: 4 MB at 1280×800, 33 MB at
4K). The library's gradient is a 1/8-size texture stretched by the GPU.
Measured on the development PC (NVIDIA): ~88 MB of private memory while
reading, part of it the NVIDIA driver's; the requirement's < 100 MB must be
confirmed on a Deck (Mesa).

## D17. Test scripts and the headless runner

`--script "key:right; idle; shot:/tmp/x.png; quit"` drives the app; one
command runs per frame, so screenshots show the effect of what came before.
`tools/headless-run.sh` runs it with SDL's offscreen driver and software
renderer (no display needed) or in a headless gamescope (`BACKEND=gamescope`,
the game-mode path: X11 + OpenGL). gamescope may crash while tearing down
after the app quit (seen with NVIDIA, as in DeckVideoPlayer); the runner
reports the app's own exit status.

## D18. Menus are destroyed after the input that closed them

A menu item's callback often closes its own menu ("Close book", a contents
entry). `App::pop_menu`/`close_menus` move menus aside and the loop
destroys them once the current input is handled, so a callback never runs
inside a destroyed object.

## D19. No sleep signal needed

Under game mode an app gets no notice before SteamOS suspends unless it
holds a logind inhibitor lock, which needs system-bus access from the
sandbox. Saving on every page turn (D6) makes that unnecessary.

## D20. Teal accent, not DeckVideoPlayer's blue

Everything else in the look is shared, but with the same blue the two apps
were hard to tell apart (owner's feedback after the first Deck test). The
accent is `#10A38F`: white text on it scores 3.15:1 (DeckVideoPlayer's blue
2.82:1), and it reads at 6:1 on the dark background. The Steam artwork uses
the same teal. `accent_color` is commented out in the `config.ini` template
so future default changes reach users who never set it.

## D21. Text rendering modes

Grayscale is the default (requirements 4.2). RGB/BGR subpixel rendering
works (colour fringes appear in screenshots) but on the Deck's internal
screens, which are rotated portrait panels, it brings no sharpness, only
faint fringes: the owner saw no difference on a Deck OLED. Vertical RGB/BGR
(FreeType's LCD_V, supported by crengine) are offered to find out on the
device whether either matches a Deck panel's subpixel order.

## D22. First audit (ISSUES.md, Gemini, v0.1.0 test2) reviewed

Every item was checked against the code (and crengine-ng's) before acting.

**Fixed**

- **SEC-1** (crengine used the local header's sizes, unchecked by us, and
  inflated past the declared size) and **SEC-5** (ZIP64 extra field read
  past the data read): `patches/crengine-ng-zip-hardening.patch` makes the
  central directory's sizes win, cuts inflated output at the declared size,
  and bounds the ZIP64 field scan. Test: `hostile_epubs` reads an entry
  whose local header claims 2 GB and whose data inflates to 20 MB: at most
  the 1 000 bytes the central directory lists come out.
- **SEC-2** (a ZIP not recognised as EPUB makes crengine open a nested
  archive unchecked): archives containing `.epub`, `.zip`, `.fb3`, `.docx`
  or `.odt` are refused (EPUBs never contain them).
- **SEC-3**: per-entry ratio check from 1 MB (was 8 MB), and a whole-archive
  ratio limit (50:1 from 16 MB: real books measure 1.3–2.8:1); overflow-safe
  total; covers above 40 Mpixel are skipped before decoding.
- **SEC-4**: `patches/crengine-ng-no-external-fonts.patch`: `file://` and
  `res://` font URLs in a book's CSS are ignored.
- **BUG-3** gamepad handles closed on removal; **BUG-4** held buttons and
  axes per controller; **BUG-5** chapter lookups scan the whole TOC
  (out-of-order TOCs) and pick the nearest next chapter, Contents highlights
  the chapter the HUD shows (first visible page); **BUG-6** books load
  through a stream (no "@/" archive parsing of paths), the command-line
  path is canonicalised; **BUG-7** no preferred font with "Book's fonts";
  **BUG-8** X removes every bookmark on the screen, a new one records its own
  page's chapter; **BUG-9** an existing but unreadable state file turns
  saving off instead of being overwritten, bookmark fields clamped;
  **BUG-10** strict hex for `accent_color`; **BUG-11** clock timer clamped
  (leap second); **BUG-12** removable media must be `/dev` devices, strict
  octal escapes; **BUG-13** `make_dirs` checks the result is a folder;
  **BUG-14** a drag is not a tap (a horizontal swipe turns the page),
  settings indexes clamped where used.
- **UI-1** bookmark ribbon drawn from convex pieces (it lost its notch);
  **UI-2** HUD details skipped when there is no room, `ellipsize` returns
  nothing when not even "…" fits; **UI-3** toast below the HUD's top bar,
  drawers at most 85% of the screen, no reading hints under a drawer;
  **UI-4** folders re-sorted once their titles are read, a USB icon, start
  screen names sanitised; **UI-5** 100% on the last screen, menus stop at
  the ends while a direction is held; **UI-6** negative glyph pitch handled,
  glyph size cache bounded.
- **OPT-1** the page pixel buffer is freed once neighbours are pre-rendered,
  the overlay buffer while just reading; **OPT-2** rounded-rectangle fills
  use solid spans away from the corners; **OPT-3** cover caches evict least
  recently used instead of clearing; **OPT-5** the stylesheet is built once.
- **PKG-1** crengine headers found with `find_path`; **PKG-3** only this
  user's Steam counts as running; **PKG-4** test file handles closed (both
  ported to DeckVideoPlayer, its D82, so the shared scripts stay identical); the
  regression tests listed were added (`tests/make-hostile-epubs.py`).
- Saving: after failed writes the retry interval doubles (2 s up to ~1 min).

**Declined**

- **BUG-1** (vertical subpixel modes filtering horizontally with Harmony
  FreeType): measured not to be the case. In "Vertical RGB" screenshots the
  colour fringes sit above and below horizontal strokes (22 049 fringed
  pixels on horizontal edges, 9 930 on vertical ones; horizontal RGB is the
  reverse: 29 960 vs 7 736): FreeType's LCD_V rendering applies the
  geometry to its vertical bands.
- **BUG-2** (100% CPU loop when saving fails): `save_if_due` records the
  attempt time whether the write succeeds or not, so a failing disk was
  retried every 2 s, never in a loop. Now with back-off; test
  `state_save_failure_backs_off`.
- **OPT-4**: `stat()` of up to 50 recent books when the start screen opens
  takes microseconds on local storage (and fails fast for an unmounted
  card); one extra loop pass to find that every cover is cached is
  negligible. Network folders are not supported.
- **PKG-2**: `.git` is copied into the Flatpak build on purpose (build
  info for the About screen), as in DeckVideoPlayer.
- Not done yet (ROADMAP): pruning cache entries of deleted books (OPT-3),
  faster re-layouts (OPT-5).

## D23. Recently read books on the start screen, as in DeckVideoPlayer

The start screen lists the sources, then a "Recently read" section header
(small capitals over a thin line, not selectable) and up to
`[interface] recently_read` books (5; 0 hides them), each tagged with where
it is ("Local", "SD Card", "USB") and its progress, like DeckVideoPlayer's
"Recently played". Missing files are left out; Menu → *Remove from recently
read* hides one until it is read again. On a fresh start the cursor waits on
the book read last, so A resumes it. Replaces v0.1 test builds' "Continue
reading" row and "Recent books" folder. Last-read times are in milliseconds
so books opened within a second keep their order.

## D24. Optional progress line

*Reading → Progress line* (off by default) adds a band under the text with
the pages left in the chapter (left) and the percentage and pages left in
the book (right), in the theme's ink at 60%. It is drawn into each page
image when the page is rendered, so page turns cost nothing more; the
book's bottom margin grows by the band (30 units). Pages are counted one by
one; the shown pages count as read. The HUD and the library use the same
counting.

## D25. The screen size goes to crengine before the settings

crengine caps page margins at a fifth of the view's size when they are
applied, and a new view is 400x200: `Book::configure` applied the settings
before the first `Resize`, so on every book's first layout the margins were
cut to 40 px vertically and 80 px horizontally (at 4K every margin was too
small; on the Deck the progress line's band was only partly reserved), and
the first settings change then shifted the layout. `Resize` now comes first.
Found while measuring the progress line at 4K; test: `engine_open_render`
checks that the bottom margin stays blank on the first layout at 4K.

## D26. Two engine fixes found with Standard Ebooks books

Making the README screenshots with Standard Ebooks editions (the best
source of free classics, so a common case) showed two bugs, both now
crengine-ng patches with tests on small generated books
(`tests/make-hostile-epubs.py`):

- **"↩?" after every note back-link.** Standard Ebooks writes back-links
  as U+21A9 followed by U+FE0E (variation selector: "text style"). With
  HarfBuzz, the last font of the fallback chain passes each character
  through `filterChar()`, which replaced the selector (no glyph in any
  bundled font) with `?`. Variation selectors now go to HarfBuzz
  unchanged; it applies and hides them
  (`crengine-ng-variation-selectors.patch`; test
  `engine_variation_selectors_invisible` compares the page with and
  without them, for every font).
- **Line art invisible on the dark themes.** Engravings, diagrams and
  ornaments are mostly black ink on a transparent background; on OLED Black
  the ink was drawn black on black (the default theme). On a page whose
  background is darker than its text, transparent and translucent pixels
  are now drawn over a plate of the text colour; opaque images (photos,
  covers) and light pages are unchanged (`crengine-ng-image-plate.patch`;
  test `engine_line_art_plate_in_dark_themes`).
  Alternatives: inverting such images (Standard Ebooks' CSS does this for
  diagrams in dark mode) needs to know the whole image is monochrome before
  drawing it, which crengine's line-by-line decoder doesn't, and inverts
  photos with transparency too; crengine's own `setInvertImages` inverts
  every image for a night mode that inverts the whole screen. A plate in
  the text colour is what Standard Ebooks' CSS does for engravings, keeps
  the art as printed, and is no brighter than the text.

Also found: `tests/run.sh` piped the unit tests through `grep`, so the
script's status was grep's and failing checks still printed "all tests
passed". The output now goes through a log file and the unit tests' own
status decides.

## D27. CI and releases, as in DeckVideoPlayer

`.github/workflows/build.yml` has DeckVideoPlayer's three jobs (its D36,
D39, D41), adapted:

- **tests** on Ubuntu: DeckReader builds and tests inside the Freedesktop
  SDK (D7), so the job installs Flatpak and `org.freedesktop.Sdk//26.08`
  from Flathub, lifts Ubuntu's AppArmor limit on unprivileged user
  namespaces (bubblewrap needs them), then runs `scripts/dev-build.sh` and
  `tests/run.sh`. The engine tests on real books skip without the owner's
  sample books; the generated ones (hostile archives, variation selectors,
  line art) run. An empty test setting now counts as unset, which CI hit.
- **flatpak**: the same flatpak-builder action and container as
  DeckVideoPlayer, branch `stable` like `scripts/build-flatpak.sh`; the
  cache key also covers `patches/`.
- **release** on `v*` tags, the only job allowed to write: fails unless the
  tag is `v` + the version in `CMakeLists.txt`, then publishes the bundle,
  the installer archive (bundle in `dist/`, the two installer scripts,
  Steam artwork, README, changelog, licence, notices) and the corresponding
  source archive, with the version's changelog section as the text.

**Corresponding source:** the bundle contains GPL binaries (DeckReader,
and crengine-ng statically linked), so pointing at upstream is not enough.
`scripts/make-source-archive.py` follows DeckVideoPlayer's script but reads
the manifest with `scripts/manifest_sources.py` (no PyYAML) and also copies
the patches, in the order they are applied, next to the module they patch
(~60 MB, mostly the font archives). The runtime comes from Flathub, which
publishes its source.

The third-party actions are pinned to the same commits as
DeckVideoPlayer's; updated to the Node.js 24 versions in D33.

## D28. Keep the screen awake while reading, as DeckVideoPlayer

- **Problem:** in game mode the screen dims and the Deck sleeps on
  Steam's own idle timer, which only counts input Steam sees. Reading a
  page without touching anything is exactly that. SDL's own screensaver
  handling doesn't reach Steam: in a Flatpak it uses the Inhibit portal,
  which game mode doesn't offer (DeckVideoPlayer D43).
- **Mechanism:** DeckVideoPlayer's, verified there on a Steam Deck: an
  `org.freedesktop.ScreenSaver.Inhibit` request, which Steam honours (no
  dimming, no sleep), plus the Inhibit portal for desktop sessions. An
  inhibition lasts as long as the D-Bus connection that made it, so a
  small C helper, `deckreader-keep-awake` (`helper/`, sd-bus from the
  runtime's libsystemd), holds it while it runs. Flatpak permission:
  `--talk-name=org.freedesktop.ScreenSaver`. The helper writes the outcome
  to a status file in `XDG_RUNTIME_DIR`; the About screen shows it
  (*Keep awake*), so a failure is visible.
- **Policy (different from a video player):** a reader is used in short
  bursts, and a Deck left on the table must still dim and sleep. The
  helper runs while a book is open and is stopped after
  `keep_awake_minutes` (default 10) without input, in the library, and on
  quit; the next button press or tap while reading starts it again.
  `[reader] keep_awake = no` turns it off. The release is one wake-up of
  the main loop, minutes away (D1). Monotonic time doesn't advance during
  sleep, so after a resume the countdown continues where it was.
- **Changes from dvp-keep-awake:**
  - On SIGTERM it calls `UnInhibit` with its cookie before disconnecting,
    so the release doesn't depend on Steam noticing the disconnection.
  - It never wakes up while holding the inhibition (D1; it runs for whole
    reading sessions, a video player's helper only during playback).
    SIGTERM and SIGINT are blocked and read from a signalfd, polled with
    the bus. dvp-keep-awake wakes every second to catch a signal that
    lands just before its wait. Measured: 0 context switches in 3 s while
    holding.
  - Its parent dying (PR_SET_PDEATHSIG) also ends it with `UnInhibit`.

  Tested against a stand-in `org.freedesktop.ScreenSaver` on a private
  bus: Inhibit, UnInhibit on stop and on the parent's SIGKILL, nothing
  held afterwards. Confirmed on the owner's Steam Deck (test bundle
  v0.2.0-test1, 2026-11-18): no dimming while reading, About shows
  "screensaver accepted", the screen dims after `keep_awake_minutes`
  without input and in the library as usual.
- **App side:** `KeepAwake` (`src/keepawake.*`) starts the helper with
  `posix_spawn`, stops it with SIGTERM without waiting, and reaps stopped
  helpers later (WNOHANG), so the interface never waits for D-Bus. A
  helper that exits on its own (no session bus, not installed) is
  reported in About and not restarted on every page turn, only after the
  book is reopened. Tests: the life cycle with a stand-in helper script,
  and the real helper without a session bus.
- **SDL's own inhibition** is turned off (`SDL_HINT_VIDEO_ALLOW_SCREENSAVER`):
  on a desktop it kept the screen on for as long as the app ran, even idle
  in the library.

## D29. A complete About screen, as DeckVideoPlayer's

The About drawer follows DeckVideoPlayer's (its D34), from the library
menu and now also the reading menu (just above *Close book*):

- **The project:** version, status (beta), build date and commit
  (`scripts/build-info.sh`, "development build" from source), developer,
  source code, licence, copyright, the no-warranty note.
- **Support the project:** the Ko-fi link, as in the README.
- **System**, what a bug report needs:
  - the engine version (crengine-ng, patched), and the SDL, FreeType and
    HarfBuzz versions actually loaded (from the runtime);
  - the device (DMI: `Galileo` = Steam Deck OLED, `Jupiter` = LCD,
    else PC);
  - the screen size, session (game mode or the desktop's name) and video
    driver;
  - the keep-awake status (D28);
  - the book folders, and the settings, data and cache paths, written
    with `~`.
- **Credits:** crengine-ng, SDL3, FreeType, HarfBuzz, FriBiDi, libunibreak,
  utf8proc, nlohmann/json, the three fonts (OFL), the hyphenation
  patterns; the companion app.
- The **trademark notice** (not affiliated with Valve), then *Close*.

The pure parts (build info, device name, `~` paths) are in `src/about.*`
with unit tests. Menus gained **section headers** and **separators** that
the cursor skips. Scrolling up onto a section's first row shows its
header, as in DeckVideoPlayer.

Menu rows now share their width as DeckVideoPlayer's (its D32): label and
value each get their full width when both fit; otherwise the label keeps
up to 45% and the value the rest, both shortened with "…". Before, a value
never took more than 45% of the panel, which cut paths short. The drawer
is 760 units wide, so the Flatpak's data path fits at 1280×800.

## D30. CI: the Flatpak job updates a copy of the image's runtimes

- **Problem:** the `flatpak` job failed now and then with "Failed to
  update org.freedesktop.Sdk: Invalid cross-device link", before anything
  was built. The release of v0.2.0 needed three attempts; DeckVideoPlayer
  hit it too. The container image
  (`ghcr.io/flathub-infra/flatpak-github-actions:freedesktop-26.08`) has
  the SDK installed system-wide in its read-only lower layer, and the
  image was 8 weeks old. flatpak-builder (`--install-deps-from=flathub`)
  updates the SDK first, and finishing an update renames the old
  deployment's folder, which overlayfs refuses for lower-layer folders on
  some hosts.
- **Reproduced** with the same image under Docker on the dev PC (overlay2
  on ext4): `flatpak --system update org.freedesktop.Sdk//26.08` fails
  with that error every time.
- **Fix:** a step before the build copies `/var/lib/flatpak` to
  `$RUNNER_TEMP` (the job's folder, a host directory, not the overlay;
  `cp -a` keeps the hard links, 3.2 GB in a few seconds) and sets
  `FLATPAK_SYSTEM_DIR` to it for the following steps. That variable is
  flatpak's documented override for the system installation; every flatpak
  command the action runs is a subprocess and inherits it. The runtimes
  stay pre-installed, so only the update is downloaded.
- **Verified:**
  - With the copy, the same update succeeds in the image.
  - A full action-style build in the image (`xvfb-run flatpak-builder
    --install-deps-from=flathub …`, then `flatpak build-bundle`) works.
- The same step is in DeckVideoPlayer's workflow (its D83).

## D31. Web articles from wallabag

- **What:** articles saved with [wallabag](https://wallabag.org) (the
  hosted wallabag.it or a server of one's own) appear in the library as an
  **Articles** source, after the book folders, and read like books.
  Saving happens on the phone (wallabag's apps or the browser's share
  menu); the Deck only downloads. wallabag extracts the article text on
  the server, so DeckReader needs no extraction of its own and no web
  engine (requirements 3.1). Considered: a local file of URLs read by the
  app (it would need a readability extractor and per-site fetching in the
  app, and no phone → Deck path).
- **API**, read in wallabag 2.6.17's source (`EntryRestController`,
  `EntriesExport`), the version wallabag.it reported (`/api/info`,
  2026-11-20):
  - `POST /oauth/v2/token`, password grant (client ID and secret from
    the account's *API clients management* page, plus username and
    password). A new token for each sync: nothing to store, and the
    password would be needed anyway once a refresh token expires (two
    weeks by default).
  - `GET /api/entries.json?archive=0&sort=created&order=desc&detail=metadata&page=P&perPage=N`:
    the newest `max_articles` unread articles (default 50, 1–500),
    without their content, page after page (at most 100 a page) until
    `max_articles` or the last page (D32: a server that limits pages must
    not make the later ones look read).
  - `GET /api/entries/{id}.json`: one article, its HTML and details
    (title, address, site, authors, language, dates, reading time,
    preview picture).
  - `PATCH /api/entries/{id}.json` with `archive=1`: marked as read.
- **The EPUB is made by DeckReader** (`src/epubwrite.*`), not by
  wallabag. Its export (`/api/entries/{id}/export.epub`) was the first
  plan, but on wallabag.it (2.6.17) it leaves the pictures as links to
  their sites, which the engine draws as empty squares, has no author (an
  empty "Published by"), and adds a title page and a "Produced by
  wallabag" page. Our book: one page with the title, a line "authors ·
  site · date · N min read", the article and the original address; the
  pictures inside; the preview picture as the cover (library thumbnails);
  author = the authors, else the site (the library's second line);
  language from wallabag (hyphenation). A small ZIP writer on zlib; the
  same article gives the same file.
- **Pictures** are downloaded from the sites that host them, as the
  phone's browser does (`images = no` turns this off: text only). A plain
  GET: no wallabag token, no cookies; redirects followed (pictures only).
  JPEG, PNG, GIF, WebP and SVG, recognised from their first bytes (what
  the engine shows); other formats (AVIF), missing pictures and lazy-load
  placeholders are left out, and their tags removed, so no broken
  squares. Lazy-loading attributes (`data-src`...) win over `src`;
  `srcset` is split as HTML does (image CDNs put commas in addresses).
  Only from the internet: no connection to this device, the local network
  or link-local addresses, checked on the address actually connected to
  (redirects included), except for the wallabag server's own host
  (pictures wallabag stored itself) (D32). Limits per article: 60
  pictures (the cover first), 10 MB each, 32 MB in all, 60 s each, given
  up below 1 KB/s for 15 s; a host that can't be reached (not found,
  refused, too slow, TLS failed) is skipped for the rest of the article. One
  libcurl handle per sync keeps connections open: on wallabag.it, 9
  articles with 97 pictures took about 45 s (one slow site: 33 s for 20
  SVGs), against over 90 s for 5 with a new connection per request.
- **Settings:** `[wallabag]` in `config.ini` (server, client_id,
  client_secret, username, password, max_articles, images). Nothing appears until
  all five are set; a half-filled section is reported in the log with the
  missing keys. The password is in the file as written (the file is the
  user's, as DeckVideoPlayer keeps its OpenSubtitles key there); it never
  appears in the log.
- **Storage:** `<data>/deckreader/articles/<id>.epub`, written atomically
  (D6), dated when the article was saved in wallabag. The Articles folder
  opens sorted by newest (a new sort order, also offered in book folders:
  file date); recently read articles are tagged "Article". File names come
  only from the numeric ids; the books themselves are checked like any
  other (D5).
- **Sync** ("Update articles" in the library menu, or Y on the Articles
  row): articles marked as read on the Deck are archived first; then the
  unread list; the ones not on the Deck are downloaded (an article that
  fails is counted and skipped); the ones no longer unread are removed
  (not the one being read; it goes next time), with their reading state.
  **Mark as read in wallabag** (library menu on an article, reading menu)
  removes the article from the Deck at once and records its id in
  `state.json` (`wallabag.archive`); a sync that only archives starts
  right away, and if it can't reach the server the id is sent with the
  next update. Ids waiting to be archived are never downloaded again.
  While a sync runs, the menu offers **Stop updating articles** instead
  (D32); what was downloaded stays, and "updated" is recorded only when a
  whole update worked.
- **Threading:** one background thread (`SyncWorker`, `src/wallabag.*`)
  gets everything in a `SyncRequest` and never touches the app's state;
  the main thread applies the `SyncResult` (`src/articles.*`). The thread
  wakes the main loop with an SDL user event, on progress and when done:
  no timers, no polling (D1). New articles appear in the list as they
  arrive; the Articles row shows the phase ("Downloading… 3 of 12"), then
  the count and when it was updated, or the last error. Quitting cancels
  the sync (libcurl checks about once a second) and waits for the thread.
- **HTTP:** libcurl from the Freedesktop runtime (OpenSSL, the runtime's
  CA certificates, verification on). Only `http://` and `https://`.
  Redirects are not followed (a POST would lose its body): the message
  names the address to use instead. Limits: 16 MB for JSON (32 MB for
  one article), 20 s to connect, 60 s per API call, 120 s per article.
  Errors are short
  sentences for the user: refused username or password, refused client,
  host not found, no connection, certificate problem, HTTP status.
- **Network permission:** `--share=network` in the Flatpak (D4 changes).
  A Flatpak can't limit it to one host. The app uses the network only when
  the user updates the articles (wallabag, and the sites of the articles'
  pictures) or marks one as read, and only with `[wallabag]` set; the
  README's privacy line says so. Feeds and OPDS catalogues
  (roadmap) will use the same permission.
- **Tests:** the pure parts (addresses, dates, token and list answers,
  error messages, file names, the sync plan, the settings, the state) in
  the unit tests; the whole sync, including the thread and cancelling,
  against `tests/fake-wallabag.py`, a fake server on the SDK sandbox's
  own loopback (the tests have no network). `tools/headless-run.sh` takes
  `EXTRA_CONFIG=file` to add a `[wallabag]` section; dev and headless runs
  now get the network, as the Flatpak does (`NET=1` in `sdk-run.sh`).
- **Tried against wallabag.it** with the owner's account (2026-11-20,
  headless on the dev PC): 9 unread articles downloaded with their
  pictures (charts, photos, SVG figures), covers and sites in the list,
  a second update downloads nothing.
- **Confirmed on the owner's Steam Deck** (2026-11-21, test bundle
  v0.3.0-test2, after D32's fixes); released in v0.3.0.
- **Found on the way:** crengine's `LVStreamFragment::Read` doesn't stop
  at the end of a stored (uncompressed) ZIP entry; a reader that reads
  until end of stream gets the rest of the file. The engine's own readers
  use the entry's size and books display correctly, so it only showed in
  a test helper. Patched in D32 (`crengine-ng-stream-fragment.patch`);
  to report upstream (roadmap).

## D32. Second audit (ISSUES.md, Gemini, v0.3.0 wallabag work) reviewed

Every item was checked against the code (and crengine-ng's) before acting;
749 unit checks after (639 before). The real wallabag.it account was
downloaded again afterwards (headless, dev PC): 11 articles, 97 pictures,
none left out, about 14 s.

**Fixed**

- **V3-SEC-1** (pictures from untrusted pages could make the Deck send
  requests to itself or the home network): a picture request
  (`HttpRequest::public_only`) refuses loopback, private, shared,
  link-local, multicast and reserved addresses, IPv4 and IPv6, and
  IPv4-mapped forms of them. The check is in libcurl's
  `CURLOPT_OPENSOCKETFUNCTION`, on the address actually connected to, so
  names that resolve there and redirects are covered. Pictures on the
  wallabag server's own host are allowed (a server at home stores
  pictures too). Test: a picture at `localhost` is left out while the
  fake server at `127.0.0.1` serves the others.
- **V3-SEC-2**: the access token must be printable ASCII without spaces
  (it goes into a header).
- **V3-SEC-3**: picture requests give up below 1 KB/s for 15 s and wait
  10 s (not 20) to connect; test against a stalling endpoint.
- **V3-SEC-4** (part): `server = http://` on a host that isn't
  `localhost` or a local address is reported in the log (the password
  would travel unencrypted); CONFIGURATION.md says so. `config.ini` is
  created 0600 already (D6).
- **V3-SEC-5**: `patches/crengine-ng-stream-fragment.patch`: reads stop
  at a stored entry's end, `LVSEEK_END` uses its offset, `LVSEEK_CUR` is
  relative to the fragment. The test helper now reads entries to their
  end, and seeks are checked. Upstream report: roadmap.
- **V3-BUG-1**: `srcset` split as HTML does (the address runs to white
  space): Substack's and other CDNs' addresses hold commas. Matters for
  Substack posts saved to wallabag.
- **V3-BUG-2**: `data-src`, `data-lazy-src`, `data-original` and
  `data-srcset` before `src`.
- **V3-BUG-3**: addresses get spaces, control characters and non-ASCII
  bytes as `%XX` (curl refused spaces); only a host that is unreachable
  (`HttpResponse::unreachable`) is skipped for the rest of the article,
  not one with a bad address or a picture too big.
- **V3-BUG-4**: `resolve_url` handles a base without a path
  (`https://x.example?p=1`), references that are only a query, and `.`
  and `..` segments.
- **V3-BUG-5** (parts 2 and 3): comments, `<script>` and `<style>` hold
  no pictures; numeric references decode to UTF-8 (strictly; ones XML
  can't hold are dropped), named ones the engine knows (`&mdash;`) are
  kept rather than double-escaped.
- **V3-BUG-6**: NUL and the control characters XML can't hold are left
  out of every text the book gets; comments, `<script>` and `<style>`
  elements are removed from the article (the book has its own style).
- **V3-BUG-7**: the preview picture is fetched first, so the cover is
  never the one left out; pictures past 60 are counted as left out. SVG
  is recognised only when its first element is `<svg>`.
- **V3-BUG-8**: the unread list is read page after page (above).
- **V3-BUG-9**: "updated" is recorded only when the whole update worked
  (removals still apply once the list came: they are right either way).
- **V3-BUG-10**: dates with signs or out-of-range offsets are refused;
  lower-case `z` accepted.
- **V3-BUG-11**: `server = https://` (no host) is refused when reading
  `config.ini`.
- **V3-BUG-12**: "updated 09:05" with two digits, as the reading clock.
- **V3-BUG-13**: the header's sync line has a width limit; an empty
  Articles folder during an update says it is downloading; **Stop
  updating articles** in the menu while a sync runs (it was only
  cancellable by quitting).
- **V3-OPT-2**: the pictures move into the archive instead of being
  copied (they were in memory three times: the book, the ZIP items and
  the archive), each is freed once written, and the total is now 32 MB:
  the extra memory while making an article is about the size of its
  pictures. Default deflate level.
- **V3-OPT-3**: `MetaCache::forget`: removed articles leave no cached
  title or cover thumbnail.
- **V3-OPT-4** (part 1): while downloading, the library is re-read for
  new articles only when it shows the Articles folder.
- **V3-TEST-1**: the regression tests listed were added (unit tests and
  `tests/fake-wallabag.py`: a per-page limit, a stalling endpoint).

**Declined**

- **V3-SEC-1, user name in a picture address**: libcurl would send the
  page's own `user:pass` to that same host; none of our secrets are
  involved, and redirects to other hosts don't carry it (libcurl's
  default).
- **V3-SEC-4, warning about the file's permissions**: the file is created
  0600; an editor that changes that is the user's choice, and a log line
  nobody sees on the Deck wouldn't help.
- **V3-BUG-5 part 1** (`<img src=/a.png/>` read as `/a.png/`): that is
  what HTML says (a `/` in an unquoted value belongs to it), and what
  browsers do.
- **V3-BUG-6, stray `</body>` in the article**: wallabag's content is
  serialised from a parsed document, so its tags are balanced.
- **V3-OPT-1** (parallel picture downloads with `curl_multi`): one
  connection per site is already reused; 11 articles took about 14 s on
  the dev PC. Updates run only on request and articles appear as they
  arrive. Worth it only if the Deck shows slow updates (roadmap).
- **V3-OPT-2, streaming the archive to the file**: with the copies gone
  the archive is the only large buffer; not worth a second writer.
- **V3-OPT-4 part 2** (closing an article before marking it read writes
  its layout cache and refreshes twice): milliseconds, once per action;
  the engine's cache is bounded (128 MB).

## D33. CI actions on Node.js 24, as DeckVideoPlayer

- **Why:** GitHub deprecated Node.js 20 for actions and ran the pinned
  ones on Node 24 with a warning (v0.3.0 builds).
- **Pins:** the same commits as DeckVideoPlayer's D87 (still by hash,
  D27), each the latest release on 2026-11-21 with `runs.using: node24`:
  - `actions/checkout` v4 → v7.0.1. v6 keeps the token in a separate file
    that `.git/config` includes; `scripts/build-info.sh` only reads the
    commit.
  - `flatpak-builder`: from `flathub-infra/flatpak-github-actions`
    (archived in April 2025; its issue 17 sends users back upstream and
    keeps only the images) to `flatpak/flatpak-github-actions` v6.8. Same
    inputs, the same `flatpak` calls (so the copied installation of D30
    still applies through `FLATPAK_SYSTEM_DIR`), the same artifact name
    (`DeckReader-x86_64.flatpak`). The container image stays
    `ghcr.io/flathub-infra/flatpak-github-actions:freedesktop-26.08`.
  - `actions/download-artifact` v4 → v8.0.2: v5 only moved downloads by
    artifact id (the release job downloads all of them into `artifacts/`
    and `find`s the bundle); v8 fails on a digest mismatch instead of
    warning.
  - `softprops/action-gh-release` v2 → v3.0.4: only the runtime changed.
- **Checks:** the `tests` and `flatpak` jobs run on the push to `main`;
  the `release` job (download-artifact, action-gh-release) runs only on
  the next tag, so that release's run is its check.

## D34. Mark as read / unread (v0.3.1), as DeckVideoPlayer

- **Asked by the owner:** mark any book or article as unread (progress
  back to 0) or as read (with the check mark of a finished book).
- **As DeckVideoPlayer's *Mark as watched* / *Mark as unwatched* (its
  D42):** two entries in the library menu on the selected book or
  article, also on the start screen's recent books. A partly read book
  offers both; a finished one only *unread*; one never opened (or back at
  its first page) only *read*. Pages count as well as the percentage, so
  a book a few pages into its 2,000 (still 0%) offers *unread* too.
- **What each one leaves** (`StateStore::mark_read`): both clear the
  position (as DeckVideoPlayer's watched does). *Read* sets `finished`
  and 100%; *unread* clears both. Bookmarks stay, and so does `last_read`,
  so a book keeps its place among the recent books (DeckVideoPlayer keeps
  an unwatched file in *Recently played*); marking a book never opened as
  read doesn't add it there.
- **A book marked as read opens at its last screen** (`ReaderScreen::open`:
  finished with no position), as a book read to the end does, so it stays
  finished; X still reads it from the beginning. `finished` stays
  derived from the position, saved on every page turn (D6): reading it
  again clears the check mark, as before.
- **Articles:** the same two entries, next to *Mark as read in wallabag*.
  They only change the Deck; the wallabag one archives the article and
  removes it (D31). No new button: the menu, as in DeckVideoPlayer.


## D35. Footnotes and links (proposed for v0.3.3, not built yet)

- **Why first among the reading-comfort items:** "links and footnotes
  can't be followed" is the one limitation left from 0.1.0 that ordinary
  books hit. Four of the nine sample books have notes, 1,253 note links
  in all, every one pointing to an endnotes file. Only one marks them up
  (`epub:type="noteref"`); the others are plain numbers that link to the
  note.
- **Which links:** every link on the screen that points inside the book.
  crengine merges an EPUB's files into one document and rewrites
  `file.xhtml#id` to `#id`, so the target is found with `getNodeById`.
  A link is a **note** when it is marked (`epub:type="noteref"`,
  `role="doc-noteref"`), when its target is marked (`epub:type` footnote,
  endnote, rearnote or note; `role="doc-footnote"`, `doc-endnote`;
  `<aside>`), or, the common case, when its text is a short marker: up to
  four characters of digits, letters, roman numerals, `*`, `†`, `‡` or
  `§`, with or without brackets or `<sup>`. Any other link is a **jump**.
  Web links (`http:`, `mailto:`) are never opened: there is no browser in
  Game Mode, and the network is used only on request (D31). They show
  their address instead.
- **Controller: the right stick**, which is unused in the reader (and in
  DeckVideoPlayer), so no button changes meaning (D8):
  - Pushing it highlights the first link on the screen, with a teal pill
    behind it. Right or down moves to the next link, left or up to the
    previous one, in reading order (left page, then right).
  - **A opens the highlighted link.** With nothing highlighted, A shows
    the overlay as before.
  - **B removes the highlight**: B dismisses what is on screen first, as
    DeckVideoPlayer's D63 asks. Turning the page also removes it.
  - **The overlay hint bar** adds the right stick glyph with "Notes" only
    when the screen has links.
  - **Keyboard:** `n` for the next link and Shift+`n` for the previous
    one; Enter opens the highlighted link.
  - **Options set aside:**
    - R3 (pressing the stick in) is a single action, so it can't pick one
      note among several.
    - A "Notes" item in the reading menu takes three presses, for
      something done many times a chapter.
    - The D-pad, A, B, X, Y and the bumpers already have reader meanings.
- **Touch:** a tap on a link opens it. The tap area is at least 48 units
  square, because superscript markers are tiny. Taps elsewhere keep the
  page-turn and overlay zones.
- **Opening a note shows it in a panel** at the bottom of the screen,
  without moving the page:
  - It uses the interface look (dark panel, Inter) and takes at most 40%
    of the height. D-pad up and down scroll a long note.
  - The right stick moves to the next or previous link and the panel
    follows, so the notes on a screen can be read in a row.
  - A ("Go to note") jumps to the note in the book. B closes the panel.
  - **Where the text comes from:** the target element. When the target is
    an empty or inline anchor, it is the block that holds it, plus the
    following blocks up to the next note's target or a heading. The
    back-link and the leading number are dropped.
  - The text is plain, in paragraphs, up to 2,000 characters. "Go to
    note" shows the rest.
- **Jumps and "Go to note"** move the reader to the target and push the
  place they left onto a back stack, kept as XPointers (never pages; up
  to 20 places):
  - While the stack isn't empty, a pill shows "B Back to page N" at the
    bottom of the screen. B goes back there before it does its usual
    return to the library. Page turns keep the stack, so a referenced
    section can be read before going back.
  - The stack isn't saved: the saved position is where the reader is
    (D6).
- **Safety (D5, D22):** links are book input.
  - Only `#id` targets are followed, by our own code. crengine's
    `goLink` is never called: for a link without `#` it opens another
    file from the archive or the disk and replaces the document.
  - Each screen's link list, the note text (characters and blocks) and
    the back stack all have limits.
  - Hostile fixtures cover: links to missing ids, links to themselves,
    loops, huge notes and `../` file links.
- **Not now:**
  - In-page footnotes, printed at the bottom of the page through
    crengine's `-cr-hint: footnote-inpage`, as KOReader does. They only
    work for books that mark their notes, which is one of the nine. They
    also change the pagination and move text away from where it was
    written. This could be a setting later.
  - Notes in the book's own style (italics, pictures), by rendering the
    fragment through crengine. To do only if plain text proves too poor.
- **Tests:**
  - Unit tests for the marker heuristic, the note text and the back
    stack, on generated EPUBs: notes marked up, plain numbers to an
    endnotes file, and notes in the same file.
  - A `rstick:` command for headless scripts.
  - Screenshots at both sizes, then the Deck.
- **When built, update:** D8, REQUIREMENTS §5.1 (a right stick row; A in
  the reader), the README controls and the hint bars.

## D36. Math in articles: TeX to MathML (v0.3.2)

- **Asked by the owner:** articles with formulas. Sites that typeset math
  in the browser (MathJax, KaTeX) send raw TeX, and wallabag saves the
  page without running scripts, so the article showed text like
  `\(E \in \mathbb{R}^{M \times d}\)`.
- **Drawing it: crengine-ng's MathML**, which is built in
  (`MATHML_SUPPORT`, KOReader's code):
  - Tried first on the owner's article with its TeX converted to MathML
    by hand: inline and display formulas, integrals, subscripts and ℝ
    came out right, in the theme's colours and at the reading size.
  - It needs a font with an OpenType math table: **STIX Two Math** 2.13
    (OFL, 0.8 MB), registered with the bundled fonts (D9). crengine picks
    it for `<math>` from its own list whenever the text font has no math
    table, so it isn't one of the reading fonts.
  - It also helps EPUB 3 books that already contain MathML.
- **Converting it, when the article's EPUB is made** (in the articles
  thread, during *Update articles*; nothing at reading time, D1):
  - **Temml** 0.14 (MIT): KaTeX's TeX parser with MathML output, run in
    **QuickJS-ng** 0.18 (MIT): a small JavaScript engine, statically
    linked.
  - Temml's options:
    - `trust: false`: no `\href`, `\url` or `\includegraphics`.
    - `maxExpand`: stops macro loops.
    - `maxSize`.
    - `throwOnError`: a formula that fails stays as its TeX text.
    - `xml`: namespaced MathML that is valid XHTML.
  - **The engine's limits:**
    - No I/O at all (no quickjs-libc: plain ECMAScript).
    - A memory limit and a stack limit.
    - A time limit per formula and per article, through the interrupt
      handler, which also stops at *Stop updating articles*.
  - **Set aside:**
    - Our own converter: LaTeX's long tail, and the requirements (§3)
      prefer mature libraries.
    - The C converters (itex2MML, blahtex): old, and they cover less.
    - MathJax or KaTeX themselves: bigger, and MathML is a by-product of
      KaTeX.
    - math-core: it would put a Rust toolchain in the Flatpak build.
    - Pictures of formulas (MicroTeX): a fixed size and fixed colours.
- **Finding the TeX** (`src/texmath.cpp`, pure, unit-tested):
  - **Only in the article's text:** never in tags, attributes or
    comments, nor inside `<code>`, `<pre>`, `<kbd>`, `<samp>`,
    `<script>`, `<style>`, `<textarea>`, `<svg>` or `<math>`. A formula
    lies within one run of text; it doesn't span tags.
  - **The delimiters:**
    - MathJax's defaults: `\(…\)` inline; `\[…\]` and `$$…$$` displayed.
    - The display environments: `\begin{equation}…\end{equation}`,
      `align`, `gather`, `multline` and `eqnarray`, starred or not.
  - **`$…$` inline, only by pandoc's rule:**
    - The opening `$` is followed by a non-space. The closing one comes
      after a non-space and is not followed by a digit.
    - The content must also look like math: it holds a backslash, `^`,
      `_`, `{` or `=`, or it has no word of two or more letters between
      spaces.
    - So "$5 and $10" and "between $5 and 10$" stay text, and `\$` is a
      dollar sign.
  - **Limits:** entities are decoded first (`&lt;` is `<`). An inline
    formula has up to 2,000 characters and a displayed one up to 10,000,
    with at most 500 formulas per article.
- **Line breaks:** crengine draws each `<mrow>` unbroken, but breaks
  one wider than the page anywhere inside it. Temml puts a displayed
  formula in a single `<mrow>`, which broke "N(λ)" in two at 4K.
  - Inline formulas use Temml's `wrap: "tex"`: they break after
    relations and operators, as in TeX.
  - Displayed ones are split at their top level
    (`mathml_break_points`). A list ("S = …,\qquad M = …") breaks only
    after each comma followed by a space, so no "M =" is left at the end
    of a line. Any other formula breaks after each relation.
- **Two engine fixes the math showed** (D26's plate, and colours):
  - MathML's radical sign is a small SVG drawn as a CSS background
    image. D26's plate turned it into a solid block on dark themes.
    Background images now get no plate: they are decoration, not line art
    (`crengine-ng-image-plate.patch`).
  - Fraction bars and the radical's bar are borders without a colour of
    their own, and the radical's stroke takes the text colour too.
    crengine took both from the style, which holds the text colour of the
    layout. Y changes the theme without a new layout, so they kept the
    old theme's colour: light grey on Day. Now they take the text colour
    when drawn, as text does (`crengine-ng-current-colour.patch`, the
    seventh). Table borders in books had the same fault.
  - `scripts/dev-build.sh` now starts a module's checkout again from its
    commit when its patches change, as flatpak-builder does: these two
    patches touch neighbouring lines, so they can't be undone one by one.
- **Articles already on the Deck** were made without math:
  - An article format number goes into state.json (`wallabag_format`,
    2 from this version).
  - The first update after it gets the text of the unread articles again
    and makes again only those with TeX, pictures included. The others
    keep their files, so their pictures aren't fetched twice (a slow site
    took a minute). Then it records 2; if that update fails or is
    stopped, the next one does it.
  - Reading positions stay valid: paragraphs keep their places.
- **Not now:**
  - MathML already in the page: KaTeX rendered on the server sends its
    `<math>` plus a copy in HTML (`katex-html`).
  - MathJax 2's `<script type="math/tex">`.
  - Both wait for a sample of what wallabag keeps of them (it removes
    scripts).

## D37. Wikipedia reading lists (v0.4.0)

- **What:** the articles saved in the Wikipedia apps (the bookmark, which
  fills the "Saved" list, and any other reading list) appear in the
  library as a **Wikipedia** source after Articles. They are made into
  EPUBs on the Deck with their pictures and read offline like the
  wallabag articles (D31). Saving happens on the phone; the Deck only
  downloads. It needs no wallabag account: either source works alone.
  The apps keep the lists in the Wikipedia account when signed in with
  "Sync reading lists" on. Considered: sharing Wikipedia pages to
  wallabag, which works with no new code (the fallback); but the reading
  lists are where the Wikipedia apps save, and Wikipedia's own HTML keeps
  the formulas as MathML and the references as notes.
- **API**, read in the ReadingLists extension's source (master,
  2026-11-23):
  - One set of lists per account, for every language:
    `GET https://meta.wikimedia.org/w/rest.php/readinglists/v0/lists`
    (id, name, default, description, created, updated; 10 a page, then
    `next`) and `GET …/lists/{id}/entries` (id, listId, project such as
    `https://es.wikipedia.org`, title, created, updated; up to 100 a
    page, then `next`; asked newest first).
  - Reading needs a signed-in user with the `viewmyprivateinfo` right;
    GETs need no CSRF token. An account that never synced answers
    `readinglists-db-error-not-set-up`, shown as "No reading lists in
    this Wikipedia account yet: turn on Sync reading lists in the
    Wikipedia app."
  - Risk: the Action API module calls itself internal and experimental;
    the REST routes moved out of RESTBase in 2025 (T336693). The
    official apps depend on them, so a change would be announced, but
    the API is less settled than wallabag's. The client stays small and
    its errors say what failed.
- **Signing in: a personal OAuth 2 token** (an "owner-only consumer"),
  registered once by the user on meta.wikimedia.org with only the "Access
  private information" grant (`privateinfo`, which carries
  `viewmyprivateinfo`): read-only. It needs no review, never expires and
  works only for its owner. It is sent (`Authorization: Bearer`) only to
  meta.wikimedia.org's reading-list routes; articles and pictures are
  fetched without it (the fake server's tests check that no page request
  carries it). Considered: a bot password (Special:BotPasswords), which
  needs a login, cookies and tokens, and puts a password in the file.
  Tried with the owner's account (2026-11-23): 16 lists, 245 entries.
- **Settings:** `[wikipedia]` in `config.ini`: `token`, `max_articles`
  (the newest saved of each list, default 50), `images` (yes), `lists`
  (names to sync; "Saved" or "default" is the app's own list; empty: all,
  the owner's choice). Nothing appears until the token is set; it never appears in
  the log. The template and docs/CONFIGURATION.md have the steps to make
  the token.
- **The article:** `GET https://{project}/w/rest.php/v1/page/{title}/html`
  (Parsoid's HTML), without the token; a redirect is followed to its
  target. Only `https://xx.wikipedia.org` projects: the list comes from
  the server but is input, and an entry naming another host is skipped
  and logged. At most 8 MB of HTML.
  - What it holds (samples, 2026-11-23: Euler's identity, Speed of
    light, and Buenos Aires in Spanish, 0.13–1.4 MB): sections, figures
    with `srcset`, formulas as native MathML (Wikipedia's default since
    2026; no images), references as `<sup class="mw-ref">` links to
    `<ol class="mw-references">` (227 and 404 on the long pages), and
    much for the editor only: `data-mw` attributes (up to 215 KB, with
    `<` and `>` inside quotes), template `<style>`s, navigation boxes.
  - Cleaning (`wiki_clean_page`, a tag walker that respects quotes):
    the three pages keep a quarter of their size (128 → 30, 1033 → 219,
    1422 → 413 KB) in at most 6 ms, every formula and every note.
  - Kept: text, headings, infoboxes and tables (with borders: lines
    without a colour follow the theme since D36), figures and captions,
    formulas (drawn with STIX, D36), notes and references.
  - Removed: `data-*`, `style`, `about`, `typeof` and the other
    editor's attributes; `<style>`, `<link>`, `<meta>`, scripts, audio
    and video; navigation boxes, sidebars, maintenance banners, hatnotes,
    coordinates, authority control, hidden, "noprint" and "nomobile"
    elements (what Wikipedia itself hides on phones), edit links,
    reference back-links, and the "External links" section (by its
    heading, in the main languages).
  - Links to other articles and to the web become plain text: there are
    hundreds a page and nothing to follow offline. Reference links
    become `#cite_note-N` inside the book, marked `role="doc-noteref"`,
    and the references `role="doc-endnote"`: D35 recognises both, so
    footnotes will show them in its panel with no new download, and the
    right stick will visit only notes. ARIA roles rather than
    `epub:type`, which crengine may style or hide on its own.
  - The first paragraph goes before the infobox, as on Wikipedia's
    phone pages: the book starts with text, not with a screen of table.
  - Pictures: D31's code and limits (60 an article, size and time
    limits), from Wikimedia's servers, the densest picture of `srcset`
    (2×: 500 px for a 250 px figure). Icons with both sides under 48 px
    (flags in tables) are left out: Buenos Aires has 297 `<img>`. The
    cover is the article's first picture.
  - The book: the page's title; a line "en.wikipedia.org · saved 27 Jul
    2026"; the article; then "From Wikipedia, the free encyclopedia (CC
    BY-SA 4.0), the version of …" and the address (the licence asks for
    attribution). Author in the library: Wikipedia. Language from the
    page (hyphenation). The h2 and h3 headings are the book's contents
    (chapters for the D-pad and the progress line): 51 on "ARM
    architecture family". `ArticleBook` gained the contents, extra CSS
    and the identifier's scheme (`urn:wikipedia:`).
- **A symbol font for every book:** STIX Two Math is now the last
  fallback font, after Literata and Inter: "m⋅s⁻¹" on Speed of light
  showed "m?s" (Literata has no U+22C5). It covers math operators and
  arrows in any book's text.
- **Storage:** `<data>/deckreader/wikipedia/<id>.epub`; the id is a hash
  of the wiki and the title (53 bits, so the name stays an article
  number), so an article in several lists is one file, dated when it was
  first saved, and keeps its name when it moves between lists.
- **A folder for each list** (asked by the owner after test1; first, all
  lists went into one folder): the sync writes `lists.json` in the
  Wikipedia folder (`articlesync.h`: each list's id, name and articles,
  newest first) right after listing, so the folders fill as articles
  arrive. The library shows the source's folder through it
  (`list_articles`): with several lists, a folder each ("Saved" for the
  default list, then the user's names; "12 articles"; dated by its
  newest article, so "Sorted by newest" puts the list saved to last
  first; lists with nothing on the Deck left out); in a list, its
  articles; with one list, its articles straight away. The folders are
  virtual: an article in two lists is one file, with one reading
  position, shown in both. Real folders would have meant two copies and
  two positions. The format is the source's, not Wikipedia's: feeds
  (v0.6) can use it for a folder per feed.
- **Sync** as in D31: the newest `max_articles` entries of each list
  read (per list since the folders: a total of 50 would have left the
  owner's 2018 topic lists empty, behind 2026's "Saved"); new ones
  downloaded; ones no longer in them removed (not the one being read).
  With the owner's 16 lists and 2 each: 30 articles, all downloaded. An article is not downloaded again when Wikipedia changes
  it: the Deck keeps the version it got (a format number, like D36's,
  can remake them all).
- **Mark as read** (the owner's choice): the token is read-only, so the
  Deck never changes the lists. "Mark as read" (D34) puts the check mark;
  the article leaves the Deck when it is removed from its list on the
  phone.
- **Library:** the Wikipedia source after Articles, with its own icon
  (the articles' page with a "W" cut out, in the interface font, not
  Wikipedia's logo), its lists newest first ("16 lists"); its row says "Reading lists · 5
  articles · updated today 11:31" or how its update is going; recent
  books from it are tagged "Wikipedia". "Update articles" in the menu
  and Y on either row update every source that is set up, at the same
  time, one thread each (they use different servers); with both set up,
  their messages start with "wallabag:" or "Wikipedia:".
- **HTTP:** Wikimedia's User-Agent policy asks clients to name
  themselves and how to reach their author, and blocks generic ones.
  From this version every request sends `DeckReader/<version>
  (https://github.com/nicocarbone/DeckReader)`: the project's public
  page, nothing about the user. One request at a time per source.
- **Code:** what wallabag and Wikipedia share moved out of `wallabag.cpp`
  into `articlesync.*`: the entries and the plan, the pictures, saving
  the book, and the thread (`SyncWorker` now runs any job). `Articles`
  serves either source. The feeds of v0.6 will be a third.
- **Tests:** the lists and entries answers, paging, errors, the project
  check, the ids, the choice of articles, the cleaning on a page shaped
  like Parsoid's (and, with `DECKREADER_TEST_WIKI_PAGES`, on real saved
  pages outside the repository), the contents in the EPUB; the whole sync
  against `tests/fake-wikipedia.py` (paging, a list chosen by name, a
  page gone, a redirect, a host that isn't Wikipedia, pictures off, a
  refused token, an account without lists, stopping).
- **Tried with the owner's account** (2026-11-23, headless, no wallabag
  set up): the 5 newest of 245 entries, 247–1047 KB of HTML each, 46
  pictures, in about 18 s; screenshots at 1280×800 and 4K.
- **Confirmed on the owner's Steam Deck** (2026-11-23, test bundles
  v0.4.0-test1 to test3); released in v0.4.0.
- **Not now:**
  - The display title: Parsoid's HTML has the page's name ("IPhone 3G"),
    not how Wikipedia shows it ("iPhone 3G"). One Action API request per
    wiki (`prop=info&inprop=displaytitle`) would give them all.
  - An article saved through a redirect to a section ("300-page iPhone
    bill" → "IJustine") opens at the top of the target page.

## D38. Third audit (ISSUES.md, Gemini, v0.3.1–v0.4.0) reviewed

Every item was checked against the code, the build and the libraries
before acting; 1177 unit checks after (1127 before). The three saved
Wikipedia pages still clean to the same sizes, every formula and note
kept, each starting with its text.

**Fixed**

- **V4-SEC-1**: API calls (wallabag, the reading lists) and Wikipedia
  pages give up below 1 KB/s for 30 s, as pictures do (15 s): a stalled
  connection no longer holds an update for one or two minutes. 30, not
  15: Parsoid can take seconds to render a long page it hasn't cached.
- **V4-SEC-2** (part): THIRD_PARTY_NOTICES.md says libcurl serves the
  Wikipedia articles too; the source archive's index names libsystemd,
  libcurl and OpenSSL among the runtime's libraries.
- **V4-SEC-3**: a failed `JS_NewStringLen` (out of memory) ends that
  engine instead of passing an exception value to `JS_Call`. A formula
  stopped by its 2 s limit also ends the engine (Temml was stopped at
  any point; nothing of that run is used again). The next formula gets
  a newly loaded Temml (`add_math`'s `ready()`), so one hostile formula
  doesn't leave the rest of the article as TeX; loading counts against
  the article's 20 s.
- **V4-SEC-4**: `crengine-ng-current-colour.patch` replaces the
  radical's stroke only in crengine's own form (six hex digits, then the
  quote). A book's own `-cr-plain` SVG address could only break its own
  picture, but nothing in it is changed now.
- **V4-SEC-5** (part): NAT64 addresses (`64:ff9b::/96`) are judged by
  the IPv4 address in them, and local-use NAT64 (`64:ff9b:1::/48`) is
  refused; Wikipedia pages follow redirects only to https
  (`HttpRequest::https_only`); a language code can't end with a hyphen.
- **V4-BUG-1**: the reading lists' pages are followed until `next` is
  empty, not until a page brings nothing usable (a page of deleted
  entries would have ended a list early and removed its later
  articles). The same `next` twice, 200 pages of one list or 20,000
  entries end the reading; then nothing is removed in that update.
- **V4-BUG-2**: the infobox is found whatever its attributes' order;
  only a paragraph beside it (at its depth, in the lead, before the first
  `h2`) moves before it, never one from a caption, a table or a quote.
- **V4-BUG-3**: an update that rebuilds articles in an older format
  records the new one only if every local article was made again
  (`SyncResult::redo`: one failed, or the math engine didn't load); the
  next update tries again. A Wikipedia page that is gone (404) and was
  never on the Deck doesn't count, or every update would download
  everything again.
- **V4-BUG-4**: an "External links" heading drops its section only when
  it is that section's first heading (otherwise only the heading goes),
  and the contents lose whatever the dropped part held.
- **V4-BUG-5**: inside a dropped element, end tags are matched by name
  (a stack, as for kept elements), so a stray one can't end it early;
  each attribute is written once; `</scripts>` doesn't end a script,
  here or in the TeX finder. Parsoid's HTML is well formed, so these
  guard against pages that aren't.
- **V4-BUG-6**: in the contents, an `h3` before any `h2` is an entry of
  its own; only an `h2` holds `h3`s.
- **V4-BUG-7**: with both sources set up, every message says whose it is.
  A short "N new articles" from one source no longer replaces the other's
  error on screen: an error stays its full 6 s (`App::toast` keeps the
  longer one while it shows).
- **V4-BUG-8**: when the cover asked for doesn't come or is an SVG, the
  first picture that can be one is the cover (Wikipedia: often a map
  first, then photos).
- **V4-BUG-9**: `srcset` with widths (`400w, 800w`) gives the widest;
  in-page links other than notes become text (their targets' ids are
  removed).
- **V4-UI-1** (part): when only one list has articles on the Deck, the
  shelf shows them straight away.
- **V4-UI-2**: several `lists =` lines add up, as `folder =` does.
- **V4-OPT-2**: a picture transfer that stalls after connecting marks
  the transfer, not the host (`HttpResponse::stalled`); a host is given
  up for the article after two stalls, or at once when it can't be
  reached (name, connection, TLS). Wikipedia's pictures all come from
  one host.
- **V4-TEST-1**: `wallabag_epub` shuts the engine down; the 13 "font not
  loaded" warnings are gone.
- **V4-TEST-2**: tests for the above, and the page a book marked as read
  opens at, with one and two pages a screen (`engine::last_screen_page`,
  now a function of its own).

**Declined**

- **V4-SEC-2** (QuickJS-ng's licence stripped): flatpak-builder installs
  every module's licence itself; the build has
  `/app/share/licenses/io.github.nicocarbone.DeckReader/quickjs/LICENSE`.
- **V4-SEC-3** (`JS_RunGC` between articles): QuickJS-ng frees by
  reference counting and collects cycles on its own, within the 64 MB
  limit; a stopped engine is replaced anyway.
- **V4-SEC-5** (TEST-NET ranges): documentation addresses aren't routed;
  there is nothing to reach there.
- **V4-BUG-10** (`SDL_RegisterEvents` failure): SDL3 returns 0 on
  failure (SDL2 returned -1); the check is right.
- **V4-UI-1** (author and last-read sorts on the lists): folders sort by
  name in those modes, as subfolders do in book folders.
- **V4-UI-2** (list names with commas): not seen; one per line would be
  the way if one turns up.
- **V4-OPT-1** (`lists.json` read on each listing): a few KB parsed in
  microseconds, next to the folder listing's `stat` of every file.
- **V4-OPT-3** (repeated scans): milliseconds against seconds of network
  per article (cleaning a 1.4 MB page takes 7 ms).
- **V4-OPT-4** (books marked as read never opened are the first to go
  past 5,000 books): losing a check mark at that size is minor; not
  worth a second timestamp.
