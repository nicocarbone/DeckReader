# Changelog

User-visible changes in each release. The section for a tag becomes the
text of its GitHub release (CI extracts it, see `docs/DEVELOPMENT.md` §6),
so write it for people installing the app, and add one for every release.
Design details behind each change are in [docs/DECISIONS.md](docs/DECISIONS.md).

## 0.4.0 (2026-11-23)

The articles you save in the Wikipedia app, on the Deck: a folder for each
reading list, read offline like books. No wallabag account needed.

**Install or update:** in Desktop Mode, download `DeckReader-v0.4.0.tar.gz`
below, extract it and run `./scripts/install-steamos.sh` in Konsole.
Updating keeps your settings, reading positions and bookmarks. Full steps
are in the
[README](https://github.com/nicocarbone/DeckReader#install-on-steam-deck-and-steamos).

### Wikipedia

- **Articles saved in the Wikipedia app**, on a new **Wikipedia** shelf in
  the library, read offline like books: save them with the bookmark in
  the Wikipedia app on your phone (any reading list), then choose Update
  articles on the Deck (or press Y on Wikipedia). Each list is a folder
  (Saved, and yours). Pictures, formulas and references come along, the
  article's sections are its chapters, and the first paragraph comes
  before the info box. No wallabag account needed.
- To set it up, make a personal Wikipedia token that can read your lists
  and change nothing, and add it to `config.ini` in a `[wikipedia]`
  section: the steps are in
  [CONFIGURATION.md](https://github.com/nicocarbone/DeckReader/blob/main/docs/CONFIGURATION.md#wikipedia).
- With wallabag set up too, Update articles updates both.

### Also

- Math symbols missing from the book's font (such as the dot in
  "m⋅s⁻¹") now show, in books and articles alike.
- Updating articles gives up on a connection that stalls after 30
  seconds instead of waiting up to two minutes, and one slow picture no
  longer leaves out the rest of an article's pictures.
- If the update that adds math to older articles misses one, the next
  update tries it again.

## 0.3.2 (2026-11-23)

Math in web articles: formulas show as math instead of as their code.

**Install or update:** in Desktop Mode, download `DeckReader-v0.3.2.tar.gz`
below, extract it and run `./scripts/install-steamos.sh` in Konsole.
Updating keeps your settings, reading positions and bookmarks. Full steps
are in the
[README](https://github.com/nicocarbone/DeckReader#install-on-steam-deck-and-steamos).

### Web articles

- **Math in articles.** Formulas that sites write in TeX for MathJax or
  KaTeX (between `\(` and `\)`, `\[` and `\]`, or `$$`) now show as math,
  with fractions, integrals, sub- and superscripts and symbols, in your
  theme's colours and at your text size, instead of as their code. Long
  formulas break between their parts. Prices such as "$5 and $10" stay
  as they are.
- The first update after installing looks at your unread articles again
  and downloads once more the ones with math, so they show it too. The
  others are left as they are.

### Also

- A math font (STIX Two Math) is now included. EPUB books whose formulas
  are in MathML, such as some textbooks, use it too.
- After changing the theme with Y, table borders and other lines without
  a colour of their own take the new theme's colour right away.

## 0.3.1 (2026-11-21)

Any book or article can now be marked as read or as unread.

**Install or update:** in Desktop Mode, download `DeckReader-v0.3.1.tar.gz`
below, extract it and run `./scripts/install-steamos.sh` in Konsole.
Updating keeps your settings, reading positions and bookmarks. Full steps
are in the
[README](https://github.com/nicocarbone/DeckReader#install-on-steam-deck-and-steamos).

### Library

- **Mark as read** and **Mark as unread**, in the library menu (Menu) on
  any book or article. A book marked as read gets the check mark of a
  finished book and opens at its last page; marked as unread, it starts
  again from the beginning, at 0%. A partly read book offers both. Its
  bookmarks are kept, and so is its place among your recently read books.
  For an article this only changes the Deck: *Mark as read in wallabag*
  still archives it in wallabag and removes it.

## 0.3.0 (2026-11-21)

Web articles: the articles you save on your phone with wallabag now
download to the Deck, pictures included, and read like books, offline.

**Install or update:** in Desktop Mode, download `DeckReader-v0.3.0.tar.gz`
below, extract it and run `./scripts/install-steamos.sh` in Konsole.
Updating keeps your settings, reading positions and bookmarks. Full steps
are in the
[README](https://github.com/nicocarbone/DeckReader#install-on-steam-deck-and-steamos).

### Web articles

- **Read your wallabag articles.** Articles you save with
  [wallabag](https://wallabag.org) (the hosted wallabag.it or your own
  server), from your phone's share menu or a browser, appear on the start
  screen under **Articles**, with their pictures, and read like books.
- Press Y on Articles (or Menu → *Update articles*) to download the
  unread ones (Menu → *Stop updating articles* while it runs). The 50
  newest are kept, and the ones you read elsewhere are removed.
- Each article shows its site, authors, date and reading time. Its
  pictures are downloaded from the sites that host them, never from the
  Deck itself or your home network (`images = no` for text only).
- **Mark as read in wallabag**, in the menu while reading or on an article
  in the library, removes it from the Deck and archives it in wallabag,
  right away or with the next update if there is no connection.
- To set it up, fill in the new `[wallabag]` section of
  `~/.config/deckreader/config.ini` (see
  [CONFIGURATION.md](https://github.com/nicocarbone/DeckReader/blob/main/docs/CONFIGURATION.md#wallabag)).
  Without it, nothing changes: no Articles shelf, no network use.
- DeckReader now has network access in its Flatpak, used only for
  wallabag (and the articles' pictures) and only when you update or mark
  an article as read.

### Also

- Folders can be sorted by newest (Y), the order the Articles shelf
  opens in.

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
