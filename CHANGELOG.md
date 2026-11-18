# Changelog

User-facing changes in each release.

## 0.1.0 (unreleased)

First version.

- **Library:** your book folders (`~/Books` by default), the microSD card and
  USB drives, recent books and a "Continue reading" shortcut; covers, titles
  and authors from the books; sort by title, author or last read; book info.
- **Reading:** real book typography (hyphenation in the book's language,
  justification), two pages side by side on the Deck's wide screen.
- **Themes:** OLED Black, OLED Dark, Sepia (warm paper) and Day, switched instantly with Y.
- **Text:** Literata, Inter, Atkinson Hyperlegible or the book's own fonts;
  size, line and paragraph spacing, margins, alignment.
- **Navigation:** pages with the D-pad, L1/R1 or touch; chapters with
  L2/R2 or D-pad up/down; hold the left stick to scrub through the book;
  Contents (View) and bookmarks (X).
- **Status overlay (A):** chapter, pages, progress, pages left in the
  chapter, battery and clock.
- Your place is saved on every page turn, safely against sleep and power
  loss.
- Same controls and look as Deck Video Player, in teal; installer adds it to
  Steam with artwork.
- Text rendering: grayscale (default), RGB/BGR subpixels for external
  monitors, and vertical RGB/BGR to try on the Deck's screen.

Fixed after the first test on a Steam Deck OLED:

- Changing the font, size or spacing no longer moves your place in the book
  (several changes in a row used to drift back a few pages).
- The font menu now changes the font in books whose styles name a font of
  their own.
- "Updating pages…" shows while a new layout is prepared.
- Sepia is warmer; the accent colour is teal, so DeckReader and Deck Video
  Player are easy to tell apart.

From the first code audit:

- Safer handling of malformed or hostile EPUB files.
- Swipe left or right on the touchscreen to turn pages; dragging no longer
  counts as a tap.
- Pressing X on a page with two bookmarks removes both; the bookmark icon
  has its notch back.
- Chapter skipping works in books whose contents list is out of order.
- Messages no longer cover the status overlay's title; a second controller
  no longer interferes with the first.
- Folders sort by the books' titles as soon as they are read.
