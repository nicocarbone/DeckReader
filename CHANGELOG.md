# Changelog

User-visible changes in each release. The section for a tag becomes the
text of its GitHub release (CI extracts it, see `docs/DEVELOPMENT.md` §6),
so write it for people installing the app, and add one for every release.
Design details behind each change are in [docs/DECISIONS.md](docs/DECISIONS.md).

## 0.2.0 (2026-11-18)

The screen no longer dims while you read, and a complete About screen is
now also in the reading menu.

**Install or update:** in Desktop Mode, download `DeckReader-v0.2.0.tar.gz`
below, extract it and run `./scripts/install-steamos.sh` in Konsole.
Updating keeps your settings, reading positions and bookmarks. Full steps
are in the
[README](https://github.com/nicocarbone/DeckReader#install-on-steam-deck-and-steamos).

### Reading

- **The screen stays on while you read.** SteamOS no longer dims the
  screen or puts the Deck to sleep in the middle of a page. After 10
  minutes without a button press or tap, SteamOS's own dim and sleep
  timers apply again, so a Deck left on the table still turns off; in the
  library they always apply. To change this, add `keep_awake = no` or
  `keep_awake_minutes = 30` (1 to 240) under `[reader]` in
  `~/.config/deckreader/config.ini`.

### Also

- A complete **About** screen, now also in the reading menu: version and
  build, source code, licence, how to support the project, credits, and
  the details a bug report needs (versions, device, screen, whether
  SteamOS accepted the keep-awake request, folders and files).
- Menus have section headers, and long values such as folder paths use
  the free room in the row before they are cut.

## 0.1.0 (2026-11-18)

The first version of DeckReader, an EPUB reader for the Steam Deck and
SteamOS Game Mode that is used entirely with the controller. It is beta
software: so far it has only been tried on a Steam Deck OLED.

**Install:** in Desktop Mode, download `DeckReader-v0.1.0.tar.gz` below,
extract it and run `./scripts/install-steamos.sh` in Konsole. Then copy
your `.epub` files into `~/Books` (or onto the microSD card) and start
DeckReader from the Steam library in Game Mode. Full steps are in the
[README](https://github.com/nicocarbone/DeckReader#install-on-steam-deck-and-steamos).

### Library

- Your book folders (`~/Books` by default), the microSD card and USB
  drives, with covers, titles, authors and reading progress.
- Your recently read books on the start screen, below the folders, so A
  resumes the last one. `recently_read` in `config.ini` sets how many.
- Sort by title, author or last read (Y); book info (View).

### Reading

- EPUB 2 and EPUB 3 books without DRM, set like a printed book:
  hyphenation in the book's language, justified text, illustrations.
- Two pages side by side on the Deck's wide screen, or one page.
- Four themes, switched instantly with Y: OLED Black, OLED Dark, Sepia and
  Day. Black line art stays visible on the dark themes.
- Literata, Inter and Atkinson Hyperlegible, or the book's own fonts. Text
  size, line and paragraph spacing, margins, alignment, hyphenation and the
  book's own styles are in the reading settings (Menu).

### Getting around

- Turn pages with the D-pad, L1/R1, or a tap or swipe on the touchscreen;
  skip chapters with D-pad up/down or L2/R2.
- Hold the left stick to scrub through the whole book, with the page and
  chapter shown as you go.
- Contents (View) and bookmarks (X, and in the reading settings).
- The status overlay (A) shows the book, chapter, pages, percentage, pages
  left in the chapter, battery and clock. An optional progress line under
  the text shows the pages left in the chapter and in the book.

### Also

- Your place is saved on every page turn, safe against sleep and a dead
  battery.
- The same buttons and look as Deck Video Player. The installer adds
  DeckReader to Steam with its library artwork.
- No network access and no account. Nothing runs while you read.

### Known limitations

- EPUB only, and only without DRM.
- Links and footnotes can't be followed yet.
- Chinese, Japanese, Korean, Arabic and Hebrew text needs fonts that are
  not bundled yet.
- Only tried on a Steam Deck OLED. Reports from the Steam Deck LCD and other
  devices are welcome in the
  [issues](https://github.com/nicocarbone/DeckReader/issues).
