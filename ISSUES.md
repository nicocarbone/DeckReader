# DeckReader v0.1.0 Audit — Issues & Optimizations

> **Reviewed 2026-11-17.** Outcome of every item (fixed, or declined with
> the evidence) is recorded in [docs/DECISIONS.md](docs/DECISIONS.md) D22.

Complete audit of `DeckReader` (codebase, `docs/`, `REQUIREMENTS.md`, `crengine-ng` integration, packaging/scripts, and headless/unit test runs against `.dev/sample_ebooks` and `.dev/evil-books`).

Each item cites exact files, line numbers, and symbols verified in the current tree, along with a concrete fix so it can be acted on directly.

---

## 1. Security & Archive Hardening

### SEC-1. `epubguard` inspects ZIP Central Directory sizes, while `crengine-ng` decodes using Local File Header sizes (and ignores `m_unpacksize` during `Read`)
- **Severity:** High
- **Where:** `src/book.cpp:95-111` (`engine::check_book_file`), `src/epubguard.cpp:27-48`, `.dev/src/crengine-ng/crengine/src/lvstream/lvziparc.cpp:189-373`, `.dev/src/crengine-ng/crengine/src/lvstream/lvzipdecodestream.cpp:294-392`, `190-219`
- **Details:**
  1. `engine::check_book_file` opens the archive with `LVOpenArchieve` and checks `item->GetSize()` / `item->GetPackSize()`, which come from the ZIP Central Directory (`LVZipArc::ReadContents()`).
  2. When `crengine-ng` later opens an entry (`LVZipDecodeStream::Create`), it reads `packSize` and `unpSize` from the **Local File Header** (`ZipLocalFileHdr`), only falling back to the Central Directory values when both local header fields are `0`. A crafted EPUB can advertise `100` bytes in the Central Directory (passing `check_archive`) and `0x7FFFFFFF` (or a ZIP64 size) in the Local File Header.
  3. Worse, `LVZipDecodeStream::read()` (`lvzipdecodestream.cpp:190-219`) decrements `m_outbytesleft` without enforcing it: if a deflate stream produces far more uncompressed bytes than `unpSize`, `inflate()` continues until `Z_STREAM_END`.
- **Fix:**
  - Either patch `crengine-ng` (`patches/`) to enforce Central Directory / Local Header consistency and abort `LVZipDecodeStream::decodeNext()` if inflated output exceeds `m_unpacksize` (or a hard cap), **and/or** inspect local headers + bound-check streams before/during loading.

### SEC-2. Nested archive bypass in `crengine-ng` (`repeat_recursively = true`)
- **Severity:** High
- **Where:** `src/book.cpp:95-111`, `.dev/src/crengine-ng/crengine/src/lvdocview.cpp:4719-4800`
- **Details:**
  - `check_book_file` checks the outer ZIP container, not whether it is a valid EPUB (`META-INF/container.xml`). If an attacker supplies a ZIP without valid EPUB metadata containing a nested `.epub` / `.fb3` / `.docx` / `.odt`, `LVDocView::loadDocumentInt` falls back to generic archive scanning (`lvdocview.cpp:4719`) and sets `repeat_recursively = true` (`lvdocview.cpp:4796`), opening the inner archive **without** running `check_book_file` on its entries.
- **Fix:**
  - Verify inside `check_book_file` that the archive is an EPUB (e.g., contains `META-INF/container.xml`) or refuse nested archive extensions (`.epub`, `.zip`, `.fb3`, `.docx`, `.odt`) inside the archive.

### SEC-3. Decompression bomb gap below `ratio_min_size` and huge cover decode OOM
- **Severity:** Medium
- **Where:** `src/epubguard.h:20-28`, `src/epubguard.cpp:27-48`, `src/book.cpp:127-147`
- **Details:**
  1. `ArchiveLimits` sets `max_entry = 256 MB`, `max_total = 1024 MB`, `max_entries = 20000`, `max_ratio = 200`, `ratio_min_size = 8 MB`. Because the compression-ratio check is skipped below `8 MB`, an attacker can pack ~128 entries of `7.9 MB` (or 1,000 entries of `1 MB`) compressed 1000:1 (~1 MB `.epub` on disk, ~1 GB uncompressed), bypassing the ratio check completely and exhausting RAM.
  2. In `src/epubguard.cpp:43`, `total += e.size` should guard against overflow (`if (e.size > limits.max_total - total)`) in case `check_archive` is called with custom `ArchiveLimits`.
  3. In `engine::read_metadata` (`src/book.cpp:127-137`), `LVCreateStreamImageSource` + `buf.Draw(img, ...)` decodes the cover image at full uncompressed dimensions before scaling down to `480 px` high. A crafted EPUB with a valid small PNG/JPEG encoding e.g. `12000×12000` pixels allocates ~576 MB during library background scanning (`LibraryScreen::idle_work()`).
- **Fix:**
  - Lower `ratio_min_size` (e.g. `1 MB` or `512 KB`) and enforce a **total archive compression ratio** (`total / total_packed <= max_ratio` when `total >= 4 MB`) as well as tighter `max_total` / `max_entry` limits suitable for a handheld EPUB reader (e.g. 256–512 MB total, 64–128 MB per entry).
  - In `engine::read_metadata` (and ideally image decoding), check `img->GetWidth() * img->GetHeight()` against a sensible pixel cap (e.g. `64M` pixels) before drawing/decoding.

### SEC-4. EPUB `@font-face` `file://` / `res://` URLs allow local filesystem font loading
- **Severity:** Medium
- **Where:** `.dev/src/crengine-ng/crengine/src/lvtinydom/ldomdocument.cpp:1928-1932`, `.dev/src/crengine-ng/crengine/src/epubfmt.cpp:841-844`, `.dev/src/crengine-ng/crengine/src/lvfont/lvfreetypefontman.cpp:1299-1325`
- **Details:**
  - When `book_fonts` or embedded fonts are processed, an EPUB CSS rule `@font-face { src: url("file:///...") }` passes `file://` or `res://` URLs to `LVFreeTypeFontManager::RegisterExternalFont()`, which calls `FT_New_Face(_library, fname.c_str(), ...)` on arbitrary local paths. While the Flatpak sandbox limits readable paths (`~/Books`, Documents, Downloads, removable media, `~/.config/deckreader`), outside Flatpak (or within those mounts) untrusted EPUBs should never load external `file://` / `res://` resources.
- **Fix:**
  - Patch `crengine-ng` (or block in `ldomdocument.cpp` / `epubfmt.cpp`) so `RegisterExternalFont` / external `file://` and `res://` URLs from document content are disabled.

### SEC-5. Out-of-bounds stack read/write in `crengine-ng` ZIP64 extra-field parser
- **Severity:** Medium
- **Where:** `.dev/src/crengine-ng/crengine/src/lvstream/lvziparc.cpp:307-316`, `lvzipdecodestream.cpp:349-358`, `ziphdr.h:155-180`
- **Details:**
  - Both `LVZipArc::ReadContents` and `LVZipDecodeStream::Create` iterate over `extra[ZIPHDR_MAX_XT]` checking only `offs + 4 < extraSizeToRead`, then cast `(Zip64ExtInfo*)&extra[offs]`, call `ext->byteOrderConv()`, and call `getField64(pos)` which only checks `pos + 7 < ext->Size` (not `offs + 4 + pos + 7 < extraSizeToRead`). A malformed ZIP64 extra field near the end of `extra` can read uninitialized stack bytes or mutate bytes past `extraSizeToRead`.
- **Fix:**
  - Patch `crengine-ng` to validate `offs + 4 <= extraSizeToRead` and `offs + 4 + ext->Size <= extraSizeToRead` (clamped to `sizeof(Zip64ExtInfo::data)`).

---

## 2. Bugs — Engine, Core Loop, State & Input

### BUG-1. Vertical subpixel rendering (`Vertical RGB`, `Vertical BGR`) is broken with Freedesktop SDK's FreeType (Harmony LCD)
- **Where:** `src/book.cpp:261-262`, `src/settings.cpp:68-71`, `docs/DECISIONS.md` (D21), `.dev/src/crengine-ng/crengine/src/lvfont/lvfreetypefontman.cpp:331-351`, `/usr/include/freetype2/freetype/config/ftoption.h:126`
- **Details:**
  - `org.freedesktop.Sdk//26.08` builds FreeType **without** `FT_CONFIG_OPTION_SUBPIXEL_RENDERING` (it uses Harmony LCD rendering).
  - In `crengine-ng`'s `#else` (Harmony) branch (`lvfreetypefontman.cpp:334-350`), `font_aa_lcd_v_rgb` and `font_aa_lcd_v_bgr` set **horizontal** subpixel offsets (`{ -21, 0 }, { 0, 0 }, { 21, 0 }` and `{ 21, 0 }, { 0, 0 }, { -21, 0 }`) instead of vertical offsets (`{ 0, -21 }, { 0, 0 }, { 0, 21 }` / `{ 0, 21 }, { 0, 0 }, { 0, -21 }`), while `lvfreetypeface.cpp:200-204` renders with `FT_RENDER_MODE_LCD_V` (a 3× vertical bitmap). Thus, the vertical modes exposed in `Text rendering` to test on the Steam Deck's rotated panel (D21) do not actually filter vertically.
- **Fix:**
  - Patch `crengine-ng`'s `lvfreetypefontman.cpp` so `font_aa_lcd_v_rgb` and `font_aa_lcd_v_bgr` set vertical `FT_Vector` geometry (`{0, -21}, {0, 0}, {0, 21}` and `{0, 21}, {0, 0}, {0, -21}`), and send upstream.

### BUG-2. Infinite 100% CPU busy-loop when `StateStore::save()` fails
- **Where:** `src/app.cpp:272, 452`, `src/state.cpp:139-158, 183-197`
- **Details:**
  - If `StateStore::save()` fails (e.g., disk full, read-only filesystem, permission error), `dirty_` remains `true`.
  - On every iteration of `App::loop()`, `App::next_timeout()` checks `state.save_due_in(now_)`, which returns `0`. `SDL_WaitEventTimeout(&e, 0)` returns immediately, `state.save_if_due(now_)` retries `write_file_atomic()`, fails again, logs an error, and repeats forever at 100% CPU (violating D1 and draining battery).
  - Also, `StateStore::save()` does not update `last_save_ms_` / `saved_once_` (only `save_if_due()` does), so direct `save()` calls don't reset the debounce window.
- **Fix:**
  - Update `last_save_ms_ = now_ms` and `saved_once_ = true` even when `save()` fails (or apply exponential backoff on failure) so `save_due_in()` never spins at `0 ms`. Also update `last_save_ms_` / `saved_once_` inside `save()` (passing `mono_ms()` or taking `now_ms`).

### BUG-3. `SDL_Gamepad` handle leaked on every `SDL_EVENT_GAMEPAD_ADDED`
- **Where:** `src/app.cpp:207-210`
- **Details:**
  - `SDL_OpenGamepad(e.gdevice.which)` returns an owned `SDL_Gamepad*`, which is never stored or closed via `SDL_CloseGamepad` on `SDL_EVENT_GAMEPAD_REMOVED`. Repeatedly connecting/disconnecting Bluetooth controllers leaks gamepad handles and file descriptors.
- **Fix:**
  - Track opened `SDL_Gamepad*` handles by `SDL_JoystickID` in `App` (or `Input`) and close the matching handle on `SDL_EVENT_GAMEPAD_REMOVED` and in `~App()`.

### BUG-4. Multiple-gamepad state collision in `Input`
- **Where:** `src/input.cpp:129-172`
- **Details:**
  - `Input::held_` keys buttons by `e.gbutton.button` without `e.gdevice.which`, and `SDL_EVENT_GAMEPAD_REMOVED` unconditionally clears `held_`, `lx_`, `ly_`, `lt_`, `rt_`, `stick_dir_`. If an external controller is connected alongside the Steam Deck's built-in controller (common when docked), disconnecting one resets held inputs from the other, and axis jitter on the idle controller overwrites `lx_` / `ly_` of the active one.
- **Fix:**
  - Key held entries/axes by `(joystick_id, button/axis)` or track per-gamepad state and only reset state for the removed `e.gdevice.which`.

### BUG-5. `Book::chapter_at` and `Book::next_chapter_page` mishandle out-of-order TOCs and right-hand spread chapters
- **Where:** `src/book.cpp:395-425`, `src/reader_screen.cpp:377, 627`
- **Details:**
  1. `Book::chapter_at(int page)` (`src/book.cpp:395-404`) has `else break;`, assuming `toc_` is monotonically non-decreasing in `page`. Real EPUBs can have non-linear TOC entries (endnotes, nested/reordered links, front/back matter); a single out-of-order entry aborts the scan and breaks chapter lookup for the rest of the book.
  2. `Book::next_chapter_page(int page)` (`src/book.cpp:411-417`) returns the *first* `e.page > last_visible` instead of the *minimum* `e.page > last_visible` (unlike `prev_chapter_page` which uses `std::max`).
  3. In 2-page mode, if Chapter 1 is on the left page (`cur = 4`) and Chapter 2 starts on the right page (`5`) of the `(4, 5)` spread:
     - `next_chapter_page(4)` requires `e.page > 5` (skips Chapter 2, jumps to Chapter 3).
     - `prev_chapter_page(4)` requires `e.page < 4` (skips Chapter 2, jumps before Chapter 1).
     - Meanwhile `draw_hud` (`chapter_title_at(4)`) displays Chapter 1, while `open_toc()` (`chapter_at(4 + 2 - 1)`) highlights Chapter 2.
- **Fix:**
  - Remove `else break;` in `chapter_at` (pick the entry with the largest `e.page <= page`, or last matching entry) and take the minimum `e.page > last_visible` in `next_chapter_page`. Decide consistently whether a chapter starting on `cur + 1` is considered already entered or reachable via `prev_chapter_page` / `next_chapter_page`, and align `draw_hud` with `open_toc()`.

### BUG-6. `Book::open` fails on paths containing `@/` and `App::run` doesn't canonicalize CLI `open_path`
- **Where:** `src/book.cpp:172`, `src/app.cpp:500-501`, `src/reader_screen.cpp:30-31`
- **Details:**
  1. `LVDocView::LoadDocument(path.c_str())` calls `LVSplitArcName` (`lvdocview.cpp:4102`), which treats `@/` as an archive-member separator. If a book sits in a directory ending with `@` (e.g. `.../user@/book.epub`), `LoadDocument` fails.
  2. Passing a relative path on the CLI (`deckreader book.epub`) stores the relative string as the key in `state.json` instead of its canonical `realpath`, creating duplicate `BookState` entries with separate positions/bookmarks compared to opening the same book from the Library.
- **Fix:**
  - Canonicalize `opt.open_path` (e.g., `realpath`) in `App::run` or `ReaderScreen::open`, and verify `is_file` before attempting to open.

### BUG-7. `Book::configure` applies `"Literata"` preferred-font bias even when `"Book's fonts"` is selected
- **Where:** `src/book.cpp:235-245`
- **Details:**
  - When `"Book's fonts"` (`book_fonts == true`) is chosen, `face` defaults to `"Literata"`, and `fontMan->SetAsPreferredFontWithBias(lString8("Literata"), 25600, true)` still gives `"Literata"` a strong `25600` bias over all other fonts instead of clearing the preferred bias (`0`).
- **Fix:**
  - Pass bias `0` (`SetAsPreferredFontWithBias(lString8(), 0, true)`) when `book_fonts` is `true` so embedded/fallback font matching isn't skewed toward Literata.

### BUG-8. `ReaderScreen::toggle_bookmark` only removes one bookmark on a 2-page spread and records left-page chapter
- **Where:** `src/reader_screen.cpp:224-249`
- **Details:**
  - If two bookmarks exist on the same 2-page spread (e.g. created in 1-page mode or at a larger font size, one on page `cur` and one on `cur + 1`), pressing `X` removes only the first one and shows `"Bookmark removed"` while the bookmark ribbon stays visible on screen.
  - Also, `b.chapter = book_.chapter_title_at(cur);` always uses the left page even if the bookmark/anchor is on the right page.
- **Fix:**
  - Remove all bookmarks on the visible spread (`[cur, cur + pps)`) or show an accurate count/state, and compute `b.chapter` from `book_.page_of_position(b.xpointer)` (when valid).

### BUG-9. `StateStore::load()` can clobber `state.json` on non-`ENOENT` errors, and `from_json()` skips clamping bookmark fields
- **Where:** `src/state.cpp:99-137`
- **Details:**
  1. `StateStore::load()` treats any `read_file` failure as `"first run"` and returns `true`. If `read_file` failed due to a transient error (`EACCES`, `EIO`, `EMFILE`), subsequent `save()` calls overwrite the existing `state.json`.
  2. `StateStore::from_json()` clamps `b.percent` to `[0, 10000]`, but does not clamp `bm.percent` (`[0, 10000]`) or `b.page` / `b.pages` (`>= 0`).
- **Fix:**
  - Distinguish `ENOENT` from other errors in `read_file` / `StateStore::load()`, and clamp `bm.percent`, `b.page`, and `b.pages` in `from_json()`.

### BUG-10. `parse_config` accepts invalid `accent_color` strings like `"0x10A3"`, `"+10A38"`, `"-00001"`
- **Where:** `src/config.cpp:72-80`
- **Details:**
  - `std::strtoul(v.c_str(), &end, 16)` skips leading whitespace, accepts `+`/`-`, and accepts `0x` prefixes. Because the check is only `v.size() == 6 && end && *end == '\0'`, malformed values like `#0x10A3` or `"+10A38"` or `"-00001"` pass validation without warning.
- **Fix:**
  - Verify all 6 characters of `v` satisfy `std::isxdigit(static_cast<unsigned char>(ch))`.

### BUG-11. `ms_to_next_minute()` can return a negative value during a leap second
- **Where:** `src/system.cpp:51-58`, `src/reader_screen.cpp:443, 516`
- **Details:**
  - When `tm.tm_sec == 60` (leap second), `(60 - tm.tm_sec) * 1000 - tv.tv_usec / 1000 + 50` can be negative (down to `-949`). `ReaderScreen` casts it to `uint64_t` and adds it to `now`, wrapping `clock_due_` far into the future.
- **Fix:**
  - Clamp `ms_to_next_minute()` return value with `std::max(50, ...)` (or `(60 - std::min(tm.tm_sec, 59)) * 1000 ...`).

### BUG-12. `removable_mounts` can show an empty `/run/media/<user>` `tmpfs` mount as `"USB drive"`
- **Where:** `src/fsutil.cpp:147-205`
- **Details:**
  1. `removable_mounts` only skips a mount if another mount exists strictly below it (`parent_of_other`). If `/run/media/deck` is a `tmpfs` (or `overlay`) mount with no SD card or USB drive inserted underneath it, `parent_of_other` is `false` and it appears in the library as `"USB drive"`.
  2. `unescape_mountinfo` checks `std::isdigit(s[i + 1])` without verifying `s[i + 1..i + 3]` are octal `'0'..'7'`.
- **Fix:**
  - Require `starts_with(m.source, "/dev/")` (or skip virtual sources like `tmpfs`, `devtmpfs`, `autofs`) in `removable_mounts`, and check `'0'..'7'` for all three digits in `unescape_mountinfo`.

### BUG-13. `make_dirs` returns `true` when target exists as a regular file
- **Where:** `src/fsutil.cpp:89-98`
- **Details:**
  - `::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST` succeeds even if `path` is a regular file rather than a directory.
- **Fix:**
  - Return `::mkdir(...) == 0 || (errno == EEXIST && is_dir(path))`.

### BUG-14. Touch/mouse drag triggers `tap()`, and `Book::configure` / `make_css` lack defensive bounds checks
- **Where:** `src/app.cpp:217-222`, `src/book.cpp:214-216, 262`
- **Details:**
  1. `SDL_EVENT_MOUSE_BUTTON_UP` calls `tap()` unconditionally without checking pointer movement distance between button-down and button-up, so dragging/swiping on a touchscreen or trackpad still triggers a page turn or row open on release.
  2. `Book::make_css` indexes `para[s.para_spacing]` and `Book::configure` indexes `aa[s.text_aa]` without clamping locally (safe only while callers remember to call `s.clamp()`).

---

## 3. Bugs — UI, Typography & Rendering

### UI-1. `Ui::icon_bookmark` renders a convex pentagon/shield instead of a notched bookmark ribbon
- **Where:** `src/ui.cpp:189-191`, `src/canvas.cpp:144-177`
- **Details:**
  - Verified in headless screenshots: `Ui::icon_bookmark` passes a concave 5-vertex polygon (`{x, y, x+w, y, x+w, y+h, x+w/2, y+h-w*0.45f, x, y+h}`) to `Canvas::fill_polygon()`. `fill_polygon` uses half-plane intersection (`inside` checks all edge normals), which only works for **convex** polygons, clipping away the two bottom tails into a downward-pointing pentagon.
- **Fix:**
  - Split `icon_bookmark` into two convex polygons (e.g. a top rectangle plus two triangles for the left and right tails, matching how `icon_battery` constructs its lightning bolt).

### UI-2. `ReaderScreen::draw_hud` overlaps progress text onto button hints when space is narrow (`max_w == 0`)
- **Where:** `src/reader_screen.cpp:659`, `src/ui.cpp:16-29`, `src/text.cpp:134-154`
- **Details:**
  - `ReaderScreen::draw_hud` passes `std::max(0.0f, W - 40 - hints_w - tx - 16)` as `max_w` to `Ui::text()`.
  - `Ui::text()` treats `max_w > 0` as "ellipsize to `max_w`" and `max_w <= 0` as **"no limit"** (`max_w > 0 ? t_.ellipsize(...) : s`). When `ui_scale` is high (e.g., `2.0` on `1280×800`, where `W = 640`) or the window is narrow, `W - 40 - hints_w - tx - 16 <= 0`, clamping to `0.0f` and drawing the full `extra` string (`"32% · 5 pages left in chapter"`) directly on top of the right-aligned hints.
  - Additionally, in `TextRenderer::ellipsize()`, if `max_w` is positive but smaller than `"…"`, it still returns `"…"`.
- **Fix:**
  - Skip drawing `extra` in `draw_hud` if available width is below a minimum threshold (e.g. `< 24`), and make `TextRenderer::ellipsize` return `""` when even `"…"` exceeds `max_w`.

### UI-3. Toast overlaps HUD top bar and overlaps menus at high `ui_scale`; HUD stays drawn behind drawer menus
- **Where:** `src/app.cpp:352-361, 394-403`, `src/reader_screen.cpp:485-490, 576-592`
- **Details:**
  - Verified in headless screenshots (`1280×800` and `3840×2160`):
    1. When `hud_` is open and the user triggers a toast (e.g., `X` Bookmark added, `Y` Theme) or opens `Contents`/`Settings` while a toast is active, `draw_toast()` at `y = 28, h = 52` draws directly over the HUD top bar's book title (`y = 0..86`).
    2. When `ui_scale` is large (e.g. `2.0` at `1280×800` -> `ui.W() = 640`), `menus_.back()->width` (`500..640`) exceeds `ui.W() * 0.6`, so `free_w = std::max(ui.W() * 0.4f, ui.W() - menus_.back()->width)` extends behind the menu drawer.
    3. Opening `Contents`, `Bookmarks`, or `Settings` while `hud_` is open leaves `hud_` visible underneath the 97%-opaque drawer (and leaves `X Bookmark` / `Y Theme` cut in half along the left edge of the drawer).
- **Fix:**
  - Clamp drawer width to e.g. `std::min(m->width, ui.W() * 0.85f)` (or compute `free_w` from actual drawn `x0`), move toast `y` below the HUD top bar when `hud_` is visible (or hide `hud_` when opening a drawer menu).

### UI-4. `LibraryScreen::idle_work` updates titles/authors without re-sorting; `IconUsb` draws SD card icon; `list_root` skips `utf8_sanitize`
- **Where:** `src/library_screen.cpp:230-256, 271-275`, `src/library.cpp:44`
- **Details:**
  1. Entering a folder initially sorts by filename (`display_title` without metadata). As `idle_work()` lazily populates `MetaCache`, row titles/authors change on screen without re-sorting `loc.entries` (so `"Borges - Ficciones.epub"` becomes `"Ficciones"` while staying sorted under `B`).
  2. `LibraryScreen::draw_row` calls `ui.icon_card(...)` for both `IconCard` and `IconUsb`.
  3. `list_root()` (`src/library.cpp:44`) sets `e.name = path_basename(p);` without `utf8_sanitize(...)` (unlike `list_folder` and `list_recent`).
- **Fix:**
  - Re-sort `loc.entries` (keeping `selected` pointing to the same `path`) once `idle_work()` finishes metadata in a `Folder`, or incrementally when `sort_ != SortMode::Recent`. Add a dedicated `ui.icon_usb(...)`, and apply `utf8_sanitize` in `list_root`.

### UI-5. `HUD` percentage disagrees with `Library` on the final page, and `Menu::handle` wraps on key-repeat unlike `LibraryScreen`
- **Where:** `src/reader_screen.cpp:65-67, 644`, `src/menu.cpp:39-46`
- **Details:**
  1. On the final screen (`cur + pps >= count`), `save_position()` forces `st.percent = 10000` (100%), while `ReaderScreen::draw_hud` displays `book_.percent() / 100` (which crengine often reports as `99%`).
  2. `Menu::handle` wraps `selected` around top/bottom even when `e.repeat` is true, whereas `LibraryScreen::handle` stops at the ends while holding Up/Down.
- **Fix:**
  - Display `100%` in `draw_hud` when `cur + pps >= n`, and gate wrap-around in `Menu::handle` on `!e.repeat`.

### UI-6. `TextRenderer::glyph` assumes positive `bm.pitch` and `sizes_` cache is unbounded
- **Where:** `src/text.h:61`, `src/text.cpp:40-65, 80-82`
- **Details:**
  - `bm.buffer[y * bm.pitch + x]` assumes `bm.pitch >= 0`. Also, `TextRenderer::sizes_` never evicts old `(size_px, bold)` `FT_Face` + glyph caches if the window is resized repeatedly in desktop mode.

---

## 4. Performance & Memory Optimizations

### OPT-1. Free `ReaderScreen::pixels_` when not actively rendering (saves 33 MB at 4K toward the `< 100 MB` target)
- **Where:** `src/reader_screen.h:76`, `src/reader_screen.cpp:100, 111-118`
- **Details:**
  - `ReaderScreen::pixels_` stays allocated (`1280×800×4 = 4 MB`, `3840×2160×4 = 33 MB`) for the entire time a book is open, even during idle reading after pre-rendering completes.
  - Combined with `App::overlay_` (`33 MB` at 4K), 66 MB of CPU pixel buffers sit idle at 4K.
- **Fix:**
  - Clear/shrink `pixels_` once `has_idle_work()` becomes `false` (or use a local buffer in `texture_for` when not scrubbing), and consider releasing/shrinking `App::overlay_` when reading with no overlay needed (or keeping it only when `overlay` was recently active).

### OPT-2. Fast scanline path in `Canvas::fill_rect` / `Canvas::fill_rrect` (especially at 4K)
- **Where:** `src/canvas.h:46`, `src/canvas.cpp:63-112`
- **Details:**
  - `Canvas::fill_rect` delegates to `fill_rrect(..., 0, c)` -> `fill_shape(...)`, executing a per-pixel lambda with interior bounding-box branch checks (`ix0 < px < ix1`) and `sd_rrect()` `std::sqrt()` along edges. At `3840×2160`, drawing a menu panel (`1728×2160` = 3.7M pixels) or full-screen dim (`8.3M` pixels) runs millions of per-pixel checks.
- **Fix:**
  - Implement `Canvas::fill_rect` directly with integer clipping + solid `std::fill_n` (when opaque) or fast row blending, and split `fill_rrect` into a solid interior rectangle + 4 corner/edge bands so interior pixels never run per-pixel conditionals.

### OPT-3. `MetaCache` whole-cache clears and unbounded disk cache growth
- **Where:** `src/metacache.h:28`, `src/metacache.cpp:89-91, 118-127`
- **Details:**
  - When `covers_.size() >= 48` or `scaled_.size() > 400`, `MetaCache` calls `.clear()`, evicting even the covers currently visible on screen and causing redundant PNG disk reads and area-averaging rescales on the next draw.
  - `metadata.json` and `covers/*.png` never prune entries for books that were deleted or moved.
- **Fix:**
  - Use simple LRU eviction (e.g. a small generation counter or `std::list`) instead of `.clear()`, and prune stale `metadata.json` / orphaned `covers/*.png` entries periodically or on manual refresh.

### OPT-4. Synchronous `stat()` storm in `list_root` / `list_recent` and unnecessary `idle_work` wake
- **Where:** `src/library.cpp:35-37, 105-107`, `src/library_screen.cpp:52, 230-256`
- **Details:**
  - `list_root()` calls `is_file()` (`stat()`) on up to 50 recent paths and `list_recent()` on up to 200 paths synchronously on the main thread every time the screen opens/refreshes (slow if paths live on unmounted/sleeping removable media or network mounts).
  - `LibraryScreen::list()` unconditionally sets `meta_pending_ = true`, causing an extra `next_timeout() == 0` loop wakeup and full scan even when all books are already cached.
- **Fix:**
  - Compute `meta_pending_` accurately in `list()` (or clear it if nothing needs fetching), and avoid redundant `stat()` calls on paths under unmounted prefixes.

### OPT-5. Re-layout latency on large books (tracking `ROADMAP.md` v0.1.x)
- **Where:** `src/book.cpp:224-306`, `src/reader_screen.cpp:279-289`
- **Details:**
  - `Book::configure` calls `view_->checkRender()` synchronously on the main thread, taking ~70–280 ms on desktop for 0.2–1.3 MB EPUBs (observed in `tests/run.sh`) and noticeably longer on Steam Deck APU / large books. Also, `Book::configure` calls `make_css(s)` twice (`src/book.cpp:287-288`) and `Book::rebuild_toc()` unconditionally even if only `text_aa` changed? Wait: `text_aa` affects glyph width/kerning slightly so TOC pages can shift, which is why `rebuild_toc()` runs — good, just avoid computing `make_css(s)` twice.

---

## 5. Build, Packaging & Test Hygiene

### PKG-1. Fragile `CRENGINE_INCLUDE_DIR` derivation in `CMakeLists.txt`
- **Where:** `CMakeLists.txt:21-22`
- **Details:**
  - `get_filename_component(CRENGINE_PREFIX "${crengine-ng_DIR}/../../.." ABSOLUTE)` assumes `crengine-ngConfig.cmake` is always exactly 3 directories deep (`lib/cmake/crengine-ng`), which breaks on `lib64/` or multiarch `lib/<triplet>/cmake/` layouts.
- **Fix:**
  - Use `find_path(CRENGINE_INCLUDE_DIR crengine.h PATHS "${CRENGINE_PREFIX}/include/crengine-ng" ...)` or query target/prefix properly.

### PKG-2. Flatpak manifest `sources` copies `.git` into build sandbox
- **Where:** `packaging/flatpak/io.github.nicocarbone.DeckReader.yml:144-151`
- **Details:**
  - `skip:` excludes `.dev`, `.flatpak-builder`, `build-dir`, `dist`, `repo`, but not `.git` (note `scripts/build-info.sh` uses `.git` for `commit=`, which is intentional, though `.git` can grow over time; documenting or keeping shallow is worth noting).

### PKG-3. `pgrep -x steam` in installer/shortcut tool is not user-scoped
- **Where:** `scripts/install-steamos.sh:23`, `scripts/steam_shortcut.py:120`
- **Details:**
  - `pgrep -x steam` matches a `steam` process owned by any user on multi-user systems rather than the current user (`pgrep -u "$USER" -x steam` / `pgrep -u <uid> -x steam`).

### PKG-4. `ResourceWarning` unclosed files in `tests/test_steam_shortcut.py` and missing regression tests
- **Where:** `tests/test_steam_shortcut.py:130, 134`, `tests/unit_tests.cpp`
- **Details:**
  - `tests/test_steam_shortcut.py` lines 130 & 134 use `open(...).read()` without `with`, emitting `ResourceWarning` during `tests/run.sh`.
  - Add unit/integration tests for:
    - ZIP Local File Header vs Central Directory size mismatch & nested `.epub` rejection (`SEC-1`, `SEC-2`).
    - Out-of-order TOC entries in `Book::chapter_at` / `next_chapter_page` (`BUG-5`).
    - `accent_color` hex validation (`BUG-10`).
    - `removable_mounts` filtering out empty `tmpfs` `/run/media/<user>` (`BUG-12`).
    - `StateStore::save_due_in` backoff/non-spinning after a write failure (`BUG-2`).
