# Decisions

Why DeckReader is built the way it is. Each entry: the decision, the
reason, and what was considered instead. Add an entry when you make a new
choice; refer to it as Dn in code comments. DeckVideoPlayer's decisions are
cited as "DeckVideoPlayer Dn".

## D1. Event-driven loop; a frame is two textured quads

The main loop sleeps in `SDL_WaitEventTimeout` with a timeout only when a
timer is really pending (a held button's repeat, a toast, the HUD clock,
a debounced save, a re-layout, the left-stick scrub). Nothing renders while
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
able to read SSH keys or other apps' data. No network permission in v0.1
(nothing uses it). Other folders: `flatpak override` (documented in
`config.ini`).

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
unambiguous.

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
