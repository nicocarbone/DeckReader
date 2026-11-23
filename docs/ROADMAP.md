# Roadmap

What comes next, in order. Same guiding rules as DeckVideoPlayer: simple
first, opt-in features show no UI until configured, nothing runs while
idle, network work only on request.

## v0.1.0: local reader (released 2026-11-18, tried on a Steam Deck OLED)

Library (folders, microSD/USB, recent books, covers, sorting, book info),
reader (two pages, hyphenation, four themes, three fonts, sizes and
spacing, contents, bookmarks, left-stick scrub, HUD with battery and
clock), saving on every page turn, Flatpak, SteamOS installer, Steam
artwork. See CHANGELOG.md.

## v0.2.0: keep awake, About (released 2026-11-18)

- The screen stays awake while reading, with a limit without input (D28);
  confirmed on the owner's Steam Deck.
- A complete About screen, also in the reading menu (D29).

## v0.1.x: from the first Deck test

- Run the on-device checklist (DEVELOPMENT.md §7); fix what it finds.
- Pick the Deck's text rendering from the vertical subpixel modes (D21).
- Faster re-layouts on large books (crengine lays out the whole book on
  every typography change; measure on the Deck first).
- Prune cached titles/covers of books that were deleted (audit OPT-3).
- Send the ZIP-hardening and external-font patches upstream (D22).
- Measure memory on the Deck (Mesa) against the 100 MB target (D16) and
  battery drain while reading.
- Send the soft-hyphen fix upstream to crengine-ng (D3), and the
  variation-selector one (D26).
- Report the `LVStreamFragment` fix upstream (reads past the end of a
  stored ZIP entry; patched in D32).
- DeckVideoPlayer's dvp-keep-awake could take D28's changes (UnInhibit on
  stop, no wake-ups while holding).

## v0.3.0: web articles from wallabag (released 2026-11-21)

Web articles saved on the phone with [wallabag](https://wallabag.org)
(self-hosted or wallabag.it), read on the Deck like books (D31).

- The `[wallabag]` settings, the Articles source, "Update articles"
  (also Y on Articles), "Mark as read in wallabag" (queued when offline),
  the article EPUBs with their pictures, sorting by newest, the network
  permission; tested against a fake server and against wallabag.it, and
  confirmed on the owner's Steam Deck (test bundle v0.3.0-test2).
- Second audit reviewed and fixed (D32).
- Parallel picture downloads only if updates turn out slow on the Deck
  (D32).

## v0.3.1: mark as read or unread (released 2026-11-21)

- Mark any book or article as read (✓) or unread (back to 0%) from the
  library menu, as DeckVideoPlayer's watched/unwatched (D34); confirmed
  on the owner's Steam Deck (test bundle v0.3.1-test1).

## v0.3.2: math in articles (released 2026-11-23)

- TeX formulas in web articles (MathJax, KaTeX) become MathML when the
  article is downloaded, drawn by crengine with a bundled math font
  (D36): Temml in QuickJS, STIX Two Math. Articles already on the Deck
  with math are made again at the first update. Confirmed on the owner's
  Steam Deck (test bundle v0.3.2-test1).
- Later, with a sample of what wallabag keeps: MathML already in a page
  (KaTeX rendered on the server) and MathJax 2's `<script type="math/tex">`.

## v0.4.0: Wikipedia reading lists (released 2026-11-23)

The articles saved in the Wikipedia apps (the "Saved" list and any other
reading list), made into EPUBs on the Deck and read offline like the
wallabag articles (D37). Tried with the owner's account and confirmed on
the owner's Steam Deck (test bundles v0.4.0-test1 to test3); third audit
reviewed (D38).

- A Wikipedia source after Articles, a folder for each reading list;
  "Update articles" (and Y on either row) updates both.
- The article's HTML from Wikipedia itself: formulas as MathML (drawn
  since v0.3.2), references kept as notes, navigation boxes and editor
  data removed, links to other articles as plain text.
- The sync thread, the pictures and the EPUB shared with wallabag (and
  with the feeds of v0.6).
- Later: the display title ("iPhone", not "IPhone"), one Action API
  request per wiki; a redirect's section (an article saved through a
  redirect opens at the top of its target).

## Reading comfort (after v0.4.0)

- Footnotes in a popup (links followed with the controller): designed
  in D35, right after Wikipedia; its references are marked as notes from
  the start (D37).
- Fallback fonts for CJK, Arabic and Hebrew text (Noto subsets; D9).
- Optional dyslexia-oriented font (OpenDyslexic).
- "Open with" from Desktop Mode: quit after closing the book
  (DeckVideoPlayer D21).
- Book info: series, description; delete or hide a book.
- More formats crengine already reads: FB2, MOBI (no DRM), TXT.

## v0.5: your Calibre library (OPDS catalogues)

Books from Calibre's content server, browsed on the Deck and downloaded to
read offline: the book is chosen on the Deck. The same works with
Calibre-web, Kavita and Komga, which speak OPDS too (requirements 6.2).
Calibre (or the server) must be running and reachable: at home, or
through a VPN.

- Settings in `config.ini`: the server's address and an optional login
  (Calibre uses Basic or Digest; libcurl does both).
- A Calibre shelf in the library: browse by newest, author, series (in
  their order) and tag, with covers. No typing needed; search comes with
  the on-screen keyboard (v0.7).
- A on a book downloads it, with progress, into the app's own data folder
  (the book folders stay read-only, D4), like the articles; it then reads
  like any book. Downloads can be deleted from the menu.
- EPUB only: other formats are converted in Calibre first.
- Reuses the articles thread and the HTTP code (D31). New: an Atom/XML
  parser (no DTDs, no external entities), shared with the feeds of v0.6,
  and a screen to browse a catalogue.
- One-way: reading progress isn't sent back to Calibre (that is the
  wireless connection, further down).
- Tests: a fake OPDS server shaped like Calibre's; then the owner's
  Calibre.

## v0.5.x: Google Drive folders

A Google Drive folder shared with "anyone with the link" (one's own books,
or a folder shared with permission) as a shelf: its books and subfolders
listed, A downloads one, with v0.5's catalogue screen and downloads.

- The folder's link in `config.ini`; listing and downloads through
  Google's Drive API (documented), not the folder's web page.
- The API needs a key. To decide: one from the project's own Google Cloud
  project, limited to the Drive API (users only paste a link; the key is
  public in the code), or each user's own (setup like the Wikipedia
  token, no shared quota).
- Public and link-shared folders only: private ones would need a Google
  sign-in (much more work).
- Quirks to handle: files Drive marks as downloaded too often
  ("download quota exceeded"), and older share links that need their
  resource key.
- Until then: rclone, set up once in Desktop Mode, can sync a Drive
  folder to `~/Books`.

## v0.6: newsletters and blogs by feed

Posts from RSS and Atom feeds (Substack and Ghost newsletters, blogs,
news sites) made into EPUBs on the Deck and read offline like the
wallabag articles, with no account anywhere. Most of it is v0.3's work:
the EPUB builder, the picture downloads, the articles thread, and
v0.5's XML parser.

- Feed addresses in `config.ini` (`[feeds]`, one `feed =` line each);
  a Feeds source in the library, one folder per feed, newest first.
- "Update feeds" (also Y on Feeds), in the articles thread (D31): only
  on request, no timers (D1).
- The full text from the feed (`content:encoded`, Atom `content`), with
  its pictures. Posts with only a summary in the feed keep the summary
  and the link (fetching the whole page needs a readability extractor:
  later, if such feeds are common).
- The newest posts of each feed (a `max_posts` setting). "Mark as read"
  removes a post from the Deck; posts read or removed aren't downloaded
  again (their ids in state.json).
- Feeds are untrusted input: size limits, XML read without DTDs or
  external entities, the HTTP limits of D31. Conditional requests
  (ETag, Last-Modified), so an update with nothing new costs almost
  nothing.
- Paid Substack posts: the public feed (`/feed`) has only their preview.
  Substack is said to give paid subscribers a private feed address,
  which would be one more `feed =` line (to check with the owner's
  account). No sign-in to Substack's unofficial API (undocumented, can
  change without notice, against its terms).
- Tests: a fake feed server like `tests/fake-wallabag.py`; RSS 2.0 and
  Atom samples, including broken and hostile ones.
- Later: OPML import (the list of feeds from another reader).

## v0.7: the on-screen keyboard

The Steam keyboard can't be opened reliably from a non-Steam app, so a
controller keyboard of its own, built so DeckVideoPlayer can reuse it
(its roadmap needs a first text field too).

- Search in the Calibre and OPDS catalogues (v0.5): title, author.
- Then search in whole wikis (v0.8).
- Later, settings typed in the app instead of `config.ini` (server
  addresses, logins).

## v0.8: whole wikis offline (Kiwix's ZIM files)

Offline copies of whole wikis (Wikipedia in any language, Wiktionary,
Wikivoyage, topic selections), the ZIM files Kiwix reads, browsed in the
reader like books: paged, in the reading themes, with the controller. In
DeckReader rather than an app of its own: the book engine, the reading
interface, the library with the microSD card and the packaging are
already here, and Kiwix's own apps need a web engine, which is out
(requirements 3.1).

- Before it: following links with the controller and going back (D35,
  footnotes), and the on-screen keyboard (v0.7) for search.
- `.zim` files in the library next to the EPUBs (split `.zimaa` ones
  too); opening one shows its main page.
- libzim (openZIM, GPL) in the Flatpak, at first without Xapian: search
  by title, suggestions as one types; full-text search later.
- Each page cleaned for the engine as Wikipedia's are (D37), its
  pictures and styles read from the ZIM; no JavaScript.
- Links followed with the controller and by touch. With dozens of links
  on a screen, a pointer on the right trackpad may beat stepping through
  them: to try. B goes back.
- A wiki menu: main page, search, random article, history; bookmarks
  and reading positions per article (XPointers, as for books).
- ZIM files downloaded to the microSD card from Kiwix's catalogue, an
  OPDS feed (v0.5). Sizes: the whole English Wikipedia with pictures is
  about 100 GB; without pictures, other languages and topic selections
  far less.
- First, a proof of concept: libzim built in the SDK and a few pages of
  a small Wikipedia ZIM through the engine, to see how Kiwix's HTML
  renders.

## Maybe: Calibre's wireless device connection (let's see)

Not scheduled: to decide once the Calibre shelf (v0.5) has been used for a
while. The Deck would appear in Calibre as a device, like a Kobo plugged
in: the book is chosen on the PC.

- "Send to device" in Calibre on the PC: Calibre converts the book to
  EPUB if needed, with its cover and metadata, and it lands on the Deck.
- The Deck's books listed in Calibre, removable from there; covers and
  titles fixed in Calibre updated on the Deck.
- Read status sent back: books finished on the Deck marked as read in
  Calibre.
- Costs: Calibre's device protocol is undocumented (to learn from
  Calibre's own driver); a "Connect to Calibre" screen that stays
  connected while one works on the PC; same home network only, with the
  Calibre program open.

## Later / ideas

- Highlights and notes; export.
- Dictionary lookup (offline StarDict files).
- Reading statistics (time per book, pages per session).
- The app's own Flatpak repository on GitHub Pages, like DeckVideoPlayer's
  D81.
