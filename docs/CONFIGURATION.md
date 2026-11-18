# Configuration

DeckReader has two kinds of settings (D11):

- **Reading settings** (theme, font, size, spacing, margins, alignment, two
  pages, hyphenation, book styles, text rendering) are changed in the
  reader's menu (☰) and remembered automatically.
- **`~/.config/deckreader/config.ini`** holds what you set once by editing a
  file. It is created on the first start from a commented template.

Rules (as in DeckVideoPlayer): `key = value` lines under `[section]`
headers; lines starting with `#` or `;` are comments; keys ignore case and
treat `-` and `_` alike; values may be quoted. Unknown keys and bad values
are reported in the log and ignored.

## [library]

| Key | Default | Meaning |
|---|---|---|
| `folder` | `~/Books` | A folder with books. Repeat the line for several folders. Subfolders appear as folders. |
| `removable_media` | `yes` | Show microSD cards and USB drives (`/run/media`) on the start screen. |

The Flatpak can read `~/Books`, `~/Documents`, `~/Downloads`, `/run/media`,
`/media` and `/mnt` (read-only). For any other folder, allow it once:

```sh
flatpak override --user --filesystem=/path/to/books:ro io.github.nicocarbone.DeckReader
```

## [reader]

| Key | Default | Meaning |
|---|---|---|
| `tap_zones` | `yes` | Tapping the left or right 30% of the touchscreen turns pages; the middle shows the status overlay. With `no`, every tap shows the overlay. |

## [interface]

| Key | Default | Meaning |
|---|---|---|
| `ui_scale` | `1.0` | Size of menus and overlays (0.5–3); not the book's text. |
| `clock` | `24h` | `24h` or `12h` in the status overlay. |
| `accent_color` | `#10A38F` | Colour of selections and highlights (teal; DeckVideoPlayer's blue is `#1A9FFF`). |

## Files

| Path | Content |
|---|---|
| `~/.config/deckreader/config.ini` | This file |
| `~/.var/app/io.github.nicocarbone.DeckReader/data/deckreader/state.json` | Reading settings, positions, bookmarks (outside the Flatpak: `~/.local/share/deckreader/`) |
| `~/.var/app/io.github.nicocarbone.DeckReader/cache/deckreader/` | Covers, titles and laid-out books; safe to delete |

Environment overrides (development and tests): `DECKREADER_CONFIG_DIR`,
`DECKREADER_DATA_DIR`, `DECKREADER_CACHE_DIR`, `DECKREADER_APPDIR`,
`DECKREADER_CRENGINE_DATA`.
