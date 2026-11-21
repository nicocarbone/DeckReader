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
| `keep_awake` | `yes` | Keeps the screen from dimming and the Deck from sleeping while a book is open (D28). In the library, and with `no`, SteamOS's own timers apply. About → *Keep awake* shows whether the system accepted. |
| `keep_awake_minutes` | `10` | After this many minutes without a button press or tap while reading (1–240), the screen is allowed to dim and sleep again, so a Deck left on the table doesn't stay on. The next press keeps it awake again. |

## [interface]

| Key | Default | Meaning |
|---|---|---|
| `ui_scale` | `1.0` | Size of menus and overlays (0.5–3); not the book's text. |
| `recently_read` | `5` | Recently read books listed on the start screen, below the folders (0–20; `0` hides the list). Menu → *Remove from recently read* hides one; reading it again brings it back. |
| `clock` | `24h` | `24h` or `12h` in the status overlay. |
| `accent_color` | `#10A38F` | Colour of selections and highlights (teal; DeckVideoPlayer's blue is `#1A9FFF`). |

## [wallabag]

Web articles saved with [wallabag](https://wallabag.org) (the hosted
wallabag.it or your own server), read like books (D31). Nothing changes
until all five of `server`, `client_id`, `client_secret`, `username` and
`password` are set; then the start screen shows **Articles**. Y on it, or
Menu → *Update articles*, downloads your unread articles (Menu → *Stop
updating articles* while it runs); Menu → *Mark as read in wallabag* (on
an article, or while reading one) archives it. The network is used only
then.

| Key | Default | Meaning |
|---|---|---|
| `server` | — | The address you open wallabag at, with `https://` (wallabag.it: `https://app.wallabag.it`). If the server sits under a path, include it (`https://example.com/wallabag`). `http://` only for a server on your own network: on the internet the password would travel unencrypted (the log warns). |
| `client_id`, `client_secret` | — | An API client: in wallabag's web page, *API clients management* (`/developer` on the server) → *Create a new client*, any name. |
| `username`, `password` | — | Your wallabag login. Kept in this file as written; put the password in `"quotes"` if it starts or ends with a space. |
| `max_articles` | `50` | How many unread articles to keep on the Deck, newest first (1–500). Older unread ones are left in wallabag. |
| `images` | `yes` | Download the articles' pictures from the sites that host them, as your phone's browser does. Only from the internet: pictures that point at the Deck itself or your home network are left out, except your wallabag server's own. `no`: text only, and DeckReader contacts only your wallabag server. |

Articles that are no longer unread in wallabag (read or deleted on the
phone) are removed from the Deck at the next update, with their reading
position. An article that couldn't be downloaded is tried again next time.

Example for wallabag.it:

```ini
[wallabag]
server = https://app.wallabag.it
client_id = 12345_abcdefghijklmnopqrstuvwxyz0123456789
client_secret = abcdefghijklmnopqrstuvwxyz0123456789abcd
username = yourname
password = your password
```

## Files

| Path | Content |
|---|---|
| `~/.config/deckreader/config.ini` | This file |
| `~/.var/app/io.github.nicocarbone.DeckReader/data/deckreader/state.json` | Reading settings, positions, bookmarks, articles waiting to be marked as read (outside the Flatpak: `~/.local/share/deckreader/`) |
| `~/.var/app/io.github.nicocarbone.DeckReader/data/deckreader/articles/` | The downloaded wallabag articles (`<id>.epub`) |
| `~/.var/app/io.github.nicocarbone.DeckReader/cache/deckreader/` | Covers, titles and laid-out books; safe to delete |

Environment overrides (development and tests): `DECKREADER_CONFIG_DIR`,
`DECKREADER_DATA_DIR`, `DECKREADER_CACHE_DIR`, `DECKREADER_APPDIR`,
`DECKREADER_CRENGINE_DATA`.
