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
- DeckVideoPlayer's dvp-keep-awake could take D28's changes (UnInhibit on
  stop, no wake-ups while holding).

## v0.3: web articles from wallabag, reading comfort

Web articles saved on the phone with [wallabag](https://wallabag.org)
(self-hosted or wallabag.it), read on the Deck like books.

- Opt-in: no UI until `config.ini` has the server address, an API client
  (ID and secret, created in wallabag under Developer) and the account's
  username and password.
- "Update articles" in the library menu downloads the unread entries as
  EPUBs into an Articles folder in the app's data folder (book folders
  stay read-only, D4), listed in the library like a book folder; entries
  no longer unread in wallabag are removed. The network is used only
  then, never in the background.
- API: OAuth2 token (`/oauth/v2/token`, password grant), the list of
  unread entries, and each entry's EPUB export. Confirm the export call
  against a real server first; if it isn't usable, build the EPUB from
  the entry's HTML.
- Possibly: mark an article read (archived) in wallabag when finished.
- HTTP through libcurl, which is in the Freedesktop runtime.
- Needs network permission in the Flatpak: a new decision (D4 says none),
  and the README's "no network access at all" claim changes.

Reading comfort:

- Footnotes in a popup (links followed with the controller).
- Fallback fonts for CJK, Arabic and Hebrew text (Noto subsets; D9).
- Optional dyslexia-oriented font (OpenDyslexic).
- "Open with" from Desktop Mode: quit after closing the book
  (DeckVideoPlayer D21).
- Book info: series, description; delete or hide a book.
- More formats crengine already reads: FB2, MOBI (no DRM), TXT.

## v0.4: OPDS catalogues (requirements 6.2)

- OPDS feeds from Calibre-web, Kavita, Komga: browse, search, download
  into the library folder with progress; read offline.
- Credentials in `config.ini` first; then a controller on-screen keyboard
  built so DeckVideoPlayer can reuse it (its roadmap needs a first text
  field too).
- Uses the network permission added for wallabag (v0.3).

## Later / ideas

- Highlights and notes; export.
- Dictionary lookup (offline StarDict files).
- Reading statistics (time per book, pages per session).
- The app's own Flatpak repository on GitHub Pages, like DeckVideoPlayer's
  D81.
