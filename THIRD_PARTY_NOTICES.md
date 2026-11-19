# Third-party notices

DeckReader's own code is licensed under the GNU General Public License v2.0
or later (`LICENSE`). The Flatpak bundle also contains the following
components; flatpak-builder installs each module's licence files in
`/app/share/licenses/`. The complete corresponding source of every release
is published next to it, as `DeckReader-<version>-sources.tar.gz`: each
module at its pinned commit or checksum-verified download, the patches, and
DeckReader's own source (`scripts/make-source-archive.py`, D27).

| Component | Version | Licence | Use |
|---|---|---|---|
| [crengine-ng](https://gitlab.com/coolreader-ng/crengine-ng) | 0.9.15 + 1 commit, with five patches (`patches/`) | GPL-2.0-or-later | Book engine (statically linked) |
| crengine-ng's bundled code: md4c, nanosvg, qimagescale | (in crengine-ng) | MIT; zlib; Qt's LGPL-3.0 / GPL-2.0-or-later option plus BSD-2-Clause (see crengine-ng's `thirdparty/`) | Markdown, SVG images, image scaling |
| crengine-ng's hyphenation patterns | (in crengine-ng) | various TeX pattern licences (see `crengine/data/hyph/README.md`) | Hyphenation |
| [libunibreak](https://github.com/adah1972/libunibreak) | 8.0.1 | zlib | Line breaking |
| [utf8proc](https://github.com/JuliaStrings/utf8proc) | 2.12.0 | MIT + Unicode data licence | Unicode processing |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 (`third_party/`) | MIT | State files |
| [Inter](https://rsms.me/inter/) | 4.1 | SIL OFL 1.1 | Interface and reading font |
| [Literata](https://github.com/googlefonts/literata) | 3.103 | SIL OFL 1.1 | Reading font |
| [Atkinson Hyperlegible](https://www.brailleinstitute.org/freefont/) | Google Fonts, commit 95f4904 | SIL OFL 1.1 | Reading font |

From the Freedesktop runtime (not bundled): SDL3 (zlib), FreeType
(FTL/GPLv2, used under GPLv2), HarfBuzz (MIT), FriBiDi (LGPL-2.1+), libpng,
libjpeg-turbo, libwebp, zstd, zlib, fontconfig, and libsystemd's sd-bus
(LGPL-2.1-or-later), used by the keep-awake helper.

Steam, Steam Deck and SteamOS are trademarks of Valve Corporation. This
project is not affiliated with or endorsed by Valve. The Steam artwork in
`packaging/steam/` is original (app name and generic shapes).

The README screenshots in `docs/screenshots/` show pages and covers of
public-domain books in the editions of Standard Ebooks
(standardebooks.org), which dedicates its work to the public domain (CC0
1.0). The books are not part of the repository or the app.
