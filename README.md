# DeckReader

An e-book reader for the **Steam Deck** and **SteamOS game mode**, made to
be used entirely with the controller. Companion to
[Deck Video Player](https://github.com/nicocarbone/DeckVideoPlayer): same
look, same buttons, same installer.

> **Beta, not yet tested on a Steam Deck.** Provided without any warranty
> (see `LICENSE`).

- **EPUB 2 and 3** with real book typography: hyphenation in the book's
  language, justification, two pages side by side on the Deck's wide screen.
- **Four themes** switched instantly: OLED Black, OLED Dark (no smear), Sepia,
  Day.
- **Three fonts** (Literata, Inter, Atkinson Hyperlegible) or the book's own;
  size, line and paragraph spacing, margins, alignment.
- **Contents, bookmarks, and a left-stick scrub** through the whole book.
- **Your place is saved on every page turn**, safely against sleep and
  sudden power loss.
- **Library** from `~/Books`, the microSD card and USB drives, with covers,
  recent books and "Continue reading".
- **Status overlay** with chapter, pages, battery and clock (the Steam bar is
  hidden in game mode).
- Light: one process, no background work, nothing drawn while you read.

## Install (Steam Deck / SteamOS)

1. Switch to **Desktop Mode**, download `DeckReader.flatpak` and the source
   (for the installer script).
2. In Konsole:
   ```sh
   scripts/install-steamos.sh path/to/DeckReader.flatpak
   ```
   It installs the app for your user (nothing touches SteamOS's read-only
   system), creates `~/Books`, and adds **DeckReader** to Steam with its
   artwork.
3. Copy `.epub` files into `~/Books` or onto the microSD card, return to
   Gaming Mode and launch DeckReader from the Non-Steam section.

Uninstall: `scripts/install-steamos.sh --uninstall` (your settings and
reading positions stay).

## Controls

| Button | Library | Reading |
|---|---|---|
| A | open / read | show or hide the status overlay |
| B | back (quit at the start) | hide the overlay, else back to the library |
| X | read from the start | bookmark this page (again to remove) |
| Y | sort | next theme |
| D-pad ←→, L1/R1 | page through the list | previous / next page |
| D-pad ↑↓ | move | previous / next chapter |
| L2/R2 | first / last | previous / next chapter |
| View ⧉ | book info | contents |
| Menu ☰ | refresh, about, quit | reading settings, bookmarks |
| Left stick | move | hold left/right to scrub through the book |
| Touchscreen | tap a row | tap left/right edge: page; middle: overlay |

The same buttons do the same things in Deck Video Player.

## Settings

Reading settings are in the reader's menu (☰). Book folders and a few
interface options are in `~/.config/deckreader/config.ini`, created on the
first start; see [docs/CONFIGURATION.md](docs/CONFIGURATION.md). The
Flatpak can read `~/Books`, Documents, Downloads and removable drives; for
another folder:

```sh
flatpak override --user --filesystem=/path/to/books:ro io.github.nicocarbone.DeckReader
```

DRM-protected books cannot be opened.

## Building

Everything builds inside the Freedesktop SDK (`flatpak install --user
flathub org.freedesktop.Sdk//26.08`):

```sh
scripts/dev-build.sh       # development build in .dev/
scripts/dev-run.sh         # run it in a window
tests/run.sh               # tests
scripts/build-flatpak.sh   # dist/DeckReader.flatpak
```

Details: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). How it works:
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Why:
[docs/DECISIONS.md](docs/DECISIONS.md). What it must do:
[REQUIREMENTS.md](REQUIREMENTS.md).

## License

© 2026 nicocarbone. Licensed under the **GNU General Public License v2.0
or later** (`LICENSE`), like Deck Video Player. The book engine is
[crengine-ng](https://gitlab.com/coolreader-ng/crengine-ng) (GPL-2.0-or-later);
fonts are under the SIL Open Font License. Every component and its licence:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Steam, Steam Deck and SteamOS are trademarks of Valve Corporation. This
project is not affiliated with or endorsed by Valve.
