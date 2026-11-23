# DeckReader v0.3.1 / v0.3.2 / v0.4.0 Audit — Mark Read, MathML & Wikipedia Reading Lists

Audit of the `v0.3.1` (mark as read/unread, D34), `v0.3.2` (TeX → MathML via Temml + QuickJS-ng + STIX Two Math, D36), and `v0.4.0` working-tree changes (Wikipedia reading lists + shared `articlesync` refactor, D37): `src/texmath.*`, `src/wikipedia.*`, `src/articlesync.*`, `src/articles.*`, `src/wallabag.*`, `src/epubwrite.*`, `src/http.*`, `src/app.*`, `src/book.cpp`, `src/reader_screen.cpp`, `src/library*`, `src/state.*`, `src/config.*`, `src/about.cpp`, `src/ui.*`, `patches/crengine-ng-current-colour.patch`, `patches/crengine-ng-image-plate.patch`, `packaging/flatpak/io.github.nicocarbone.DeckReader.yml`, `scripts/`, `THIRD_PARTY_NOTICES.md`, `docs/CONFIGURATION.md`, `tests/fake-wikipedia.py`, and `tests/unit_tests.cpp`.

Baseline: `tests/run.sh` passes (`1127 checks, 0 failed`, `10` Python shortcut tests), with 13 spurious `warn: font not loaded` warnings during `tex_to_mathml_temml` (see `V4-TEST-1`).

---

## 1. Security, Hardening & Packaging

### V4-SEC-1. Missing `low_speed_s` stall timeout on API and Wikipedia page requests
- **Severity:** Medium
- **Where:** `src/wikipedia.cpp:792-799, 883-890`, `src/wallabag.cpp:311-323`, `src/http.cpp:178-182`
- **Details:**
  - `HttpRequest::low_speed_s` defaults to `0`, and is currently set (`15` s) only on picture downloads (`src/articlesync.cpp:113`).
  - Wallabag API calls (`timeout_s = 60` / `120`), Wikipedia reading-list API calls (`timeout_s = 60`), and Wikipedia Parsoid page downloads (`timeout_s = 120`) have no low-speed stall timeout. If a connection stalls mid-transfer (common on handheld Wi-Fi roaming or half-open proxy/CDN sockets), `http.request()` blocks the sync worker for a full 60–120 seconds with 0 bytes/s before failing.
- **Fix:**
  - Set a sensible default `low_speed_s` (e.g., `20` or `30` seconds) in `HttpRequest`, or set it explicitly on API and page requests as well.

### V4-SEC-2. QuickJS-ng MIT license file is stripped from the Flatpak bundle
- **Severity:** Medium
- **Where:** `packaging/flatpak/io.github.nicocarbone.DeckReader.yml:44, 173-186`, `THIRD_PARTY_NOTICES.md:5-6, 30`, `scripts/make-source-archive.py:149-151`
- **Details:**
  - QuickJS-ng's `CMakeLists.txt` installs `LICENSE` to `${CMAKE_INSTALL_DOCDIR}` (`/app/share/doc/qjs/LICENSE`), which the manifest's global `cleanup: - /share/doc` (line 44) immediately removes.
  - Unlike `fonts`, `stix-math`, and `temml` (which install their license texts under `${FLATPAK_DEST}/share/licenses/`), the `quickjs` module has no `post-install` installing `LICENSE` into `/app/share/licenses/`. The MIT license requires retaining its copyright and permission notice in binary distributions, and `THIRD_PARTY_NOTICES.md:5-6` documents that each bundled module's license is installed under `/app/share/licenses/`.
  - Minor doc drift: `THIRD_PARTY_NOTICES.md:30` still mentions libcurl/OpenSSL only for "the wallabag articles" (omitting Wikipedia), and `scripts/make-source-archive.py:149-151` omits `libsystemd`, `libcurl`, and `OpenSSL` from the Freedesktop runtime summary in `SOURCES.md`.
- **Fix:**
  - Add `post-install: [install -Dm644 ../LICENSE ${FLATPAK_DEST}/share/licenses/quickjs/LICENSE]` to the `quickjs` module (and verify `libunibreak`/`utf8proc` license installation under `/app/share/licenses/`).
  - Update the two notice lines in `THIRD_PARTY_NOTICES.md` and `scripts/make-source-archive.py`.

### V4-SEC-3. Unchecked `JS_NewStringLen` exception and post-interrupt QuickJS runtime reuse in `TexToMathML::convert()`
- **Severity:** Low
- **Where:** `src/texmath.cpp:470-483`
- **Details:**
  - Line 470 calls `JS_NewStringLen(im.ctx, tex.data(), tex.size())` without checking `JS_IsException(args[0])`. If QuickJS hits `MEMORY_LIMIT` (64 MB) while allocating the input string, `JS_EXCEPTION` is passed directly into `JS_Call()` (line 472), violating QuickJS's API contract.
  - When a hostile formula hits `FORMULA_MS` (2000 ms), `Impl::interrupt()` throws `"interrupted"` from an arbitrary bytecode point inside Temml. Lines 479–480 only set `im.broken = true` on `cancel` or `"out of memory"`, leaving the runtime alive after an arbitrary mid-execution interrupt. Also, `JS_RunGC(im.rt)` is never invoked when reusing the runtime across many math-heavy articles in one sync.
- **Fix:**
  - Check `if (JS_IsException(args[0]))` before `JS_Call()`, clear the exception, and mark `im.broken = true`.
  - Treat `"interrupted"` timeouts as `im.broken = true` so the next article gets a clean runtime (reloading Temml takes ~20–30 ms), and optionally call `JS_RunGC(im.rt)` between articles.

### V4-SEC-4. `crengine-ng-current-colour.patch` assumes 6-hex-digit `stroke='#RRGGBB'`
- **Severity:** Low
- **Where:** `patches/crengine-ng-current-colour.patch:78-87` (`lvrend.cpp:8929-8937`)
- **Details:**
  - When `filepath.startsWith("data:image/svg;-cr-plain,")` and `pos = filepath.pos("stroke='#")` succeeds with `pos + 15 <= filepath.length()`, the patch unconditionally replaces 6 characters at `pos + 9`.
  - crengine's own `MathMLHelper` always emits 6 hex digits (`stroke='#%06x'`), so MathML radical signs are safe, but an untrusted EPUB with CSS `url("data:image/svg;-cr-plain,<svg>...stroke='#fff'...")` (3-digit hex) would have its closing `'` and following SVG syntax overwritten.
- **Fix:**
  - Verify `pos + 15 < filepath.length() && filepath[pos + 15] == '\''` and that all 6 characters at `pos + 9 .. pos + 14` are hex digits before replacing.

### V4-SEC-5. Defense-in-depth gaps in `public_only` IPv6 check, redirect protocol downgrade, and `wiki_host()`
- **Severity:** Low
- **Where:** `src/http.cpp:46-57, 175`, `src/wikipedia.cpp:106-123, 888`
- **Details:**
  - `public_ipv6()` (`src/http.cpp:46-57`) blocks `::ffff:0:0/96`, loopback, ULA, and link-local, but does not filter the NAT64 well-known prefix `64:ff9b::/96` (RFC 6052) or local-use NAT64 `64:ff9b:1::/48` (RFC 8215), nor IPv4 TEST-NET ranges (`192.0.2.0/24`, `198.51.100.0/24`, `203.0.113.0/24`). On a NAT64 IPv6-only network, `64:ff9b::127.0.0.1` or `64:ff9b::192.168.1.1` translates to loopback/LAN IPv4 targets.
  - `CURLOPT_REDIR_PROTOCOLS_STR` (`src/http.cpp:175`) allows `https://` → `http://` downgrade on redirects, including for Wikipedia page fetches (`follow_redirects = true`).
  - `wiki_host()` (`src/wikipedia.cpp:106-123`) rejects a leading hyphen (`lang.front() == '-'`) but allows a trailing hyphen (e.g., `en-.wikipedia.org`).
- **Fix:**
  - Check `64:ff9b::/96` via `public_ipv4(a + 12)` and reject `64:ff9b:1::/48` in `public_ipv6()`; restrict Wikipedia page redirects to `https` when `req.pages_server.empty()`; reject `lang.back() == '-'` in `wiki_host()`.

---

## 2. Bugs (Correctness & Data Integrity)

### V4-BUG-1. `run_wiki_sync()` pagination drops articles on deleted-only pages, lacks an entries-page cap/cycle guard, and deletes local articles on `MAX_ENTRIES` truncation
- **Severity:** High
- **Where:** `src/wikipedia.cpp:67-68, 826-855, 867-870`
- **Details:**
  - **Premature break on tombstone pages:** `parse_wiki_entries()` (lines 67–68) skips items with `"deleted": true` without adding them to `got`. In `run_wiki_sync()` line 844:
    `if (next.empty() || got.empty() || entries.size() >= MAX_ENTRIES) break;`
    If a page of entries returned by Wikimedia's ReadingLists API contains only deleted entries (tombstones) and still returns a non-empty `"next"` cursor, `got.empty()` is `true` — pagination stops early, silently skipping all remaining pages in that list, and `r.listed = true` causes `plan_sync()` to **delete those valid local articles**.
  - **Missing entries-page cap / cursor-cycle guard:** Unlike the outer `/lists` loop (`MAX_LIST_PAGES = 100`), the inner `/entries` loop (`for (;;)`, line 831) has neither a per-list page limit nor a `seen_next` cursor set. A buggy or cyclic `"next"` token returning 1 entry per page triggers 20,000 sequential HTTP requests.
  - **Deletion on `MAX_ENTRIES` truncation:** If a user's lists contain more than `MAX_ENTRIES` (20,000) entries, lines 844 and 847 stop reading remaining lists, yet line 867 still sets `r.listed = true` and computes `r.remove`, causing **already-downloaded local articles from unread/truncated lists to be deleted**.
- **Fix:**
  - Track `std::set<std::string> seen_next` and a per-list page limit (e.g. `MAX_ENTRY_PAGES = 200`), and stop only when `next.empty()` or `!seen_next.insert(next).second` (not on filtered `got.empty()`).
  - If `MAX_ENTRIES` or the page cap truncates listing, do not mark the list complete for deletion (either return an error or omit `r.remove` / leave `r.listed = false`).

### V4-BUG-2. `lead_first()` misses infoboxes with attributes before `class`, and can rip `<p>` out of `<figcaption>`, `<table>`, or `<blockquote>`
- **Severity:** High
- **Where:** `src/wikipedia.cpp:453-503`
- **Details:**
  - **Missed infoboxes:** Line 456 searches strictly for `"<table class=\""`. Because `wiki_clean_page()` (line 695) preserves `class`, `lang`, and `dir` in Parsoid's source attribute order, any infobox emitting `<table lang="..." class="infobox">` or `<table dir="ltr" class="infobox">` is skipped.
  - **Ripping nested `<p>` out of containers:** Line 499 calls `text_paragraph(box_end, sec_end, &end)`, which finds the first `<p>...</p>` with non-empty text anywhere before `sec_end` **without tracking element nesting**. If a `<figure><figcaption><p>Caption</p></figcaption></figure>`, a second `<table>` (e.g., a sidebar, series nav, or maintenance table), or a `<blockquote>` appears between the infobox and the lead paragraph, `lead_first()` extracts that nested `<p>...</p>` out of its container and pastes it before `<table class="infobox">` — turning a caption or table cell into the opening paragraph and leaving behind an empty/broken caption or cell.
  - **Missing `</section>`:** If the cleaned HTML has no `</section>`, `sec_end` falls back to `h.size()`, allowing `lead_first()` to pull a `<p>` from after an `<h2>` section heading to the top of the article.
- **Fix:**
  - Match `<table` with `class="...infobox..."` regardless of attribute order.
  - Stop scanning at `<h2`/`<h3` as well as `</section>`, and only pick a `<p>` at depth 0 inside the lead `<section>` (ignoring `<p>` nested inside `<table>`, `<figure>`, `<figcaption>`, `<blockquote>`, `<div>`, `<ul>`, `<ol>`).

### V4-BUG-3. `Articles::on_wake()` advances `wallabag_format` / `wikipedia_format` even when rebuilds fail or Temml fails to load
- **Severity:** High
- **Where:** `src/articles.cpp:207-214`, `src/wallabag.cpp:243-247`
- **Details:**
  - `Articles::on_wake()` bumps `wallabag_format` / `wikipedia_format` whenever `r.update && r.listed && r.error.empty() && !r.cancelled`.
  - When upgrading from v0.3.0 to v0.3.2 (`req.rebuild == true`), if even one article fails with a transient HTTP 500 (`r.failed > 0`, while `r.error` stays `""`), OR if `temml.min.js` fails to load (`m.unavailable = true` in `wallabag.cpp:246`, leaving articles without MathML), `wallabag_format` is still incremented to `ARTICLE_FORMAT` (2). Future updates run with `req.rebuild == false`, so those articles are **never rebuilt with math**.
- **Fix:**
  - Only advance `wallabag_format` / `wikipedia_format` when `r.failed == 0` (and for wallabag, add a `bool math_unavailable` flag on `SyncResult` so a failed Temml load does not mark the format upgrade done).

### V4-BUG-4. `wiki_clean_page()`: `<h3>` with `id="External_links"` can delete an enclosing `<h2>` section and leave dangling TOC entries
- **Severity:** Medium
- **Where:** `src/wikipedia.cpp:550-560, 645-654`
- **Details:**
  - Line 645 matches `(t.name == "h2" || t.name == "h3") && id && external_links_heading(*id)` and unwinds `stack` to the nearest `<section>` via `drop_from(k - 1, ...)`.
  - If an `<h3>` with `id="External_links"` (or localized equivalent) appears inside an `<h2>`'s `<section>` without its own nested `<section>`, `drop_from()` truncates `out` back to before the parent `<h2>`'s `<section>`, silently **deleting the parent `<h2>` and all its content**, while leaving the parent `<h2>` (and any earlier `<h3>`s in that section) **orphaned in `page.toc`** pointing to deleted `#id` anchors.
- **Fix:**
  - Record `size_t toc_size` on `Open`, roll back `page.toc.resize(stack[k].toc_size)` inside `drop_from()`, and only unwind to `<section>` if the heading is `<h2>` or if no earlier heading was emitted inside that `<section>`.

### V4-BUG-5. `wiki_clean_page()` `dropping` counter desynchronizes on stray end tags, duplicate attributes can emit invalid XML, and `find_end_tag()` accepts prefix matches
- **Severity:** Medium
- **Where:** `src/wikipedia.cpp:420-425, 584-587, 632-636, 686-706`, `src/texmath.cpp:232`
- **Details:**
  - **Stray close tags inside dropped elements:** While `dropping > 0`, line 585 decrements `dropping--` on any non-void closing tag without matching element names. Hand-edited Wikipedia templates inside navboxes/hatnotes often contain a stray `</span>` or `</p>`, which drops `dropping` to `0` prematurely, leaks the rest of the navbox into the article, and causes the navbox's real `</div>` to close an outer `<section>`.
  - **Duplicate attributes:** Lines 688–706 emit kept attributes without deduplicating attribute names. If an input tag carries duplicate attributes (e.g., `class="a" class="b"`), the emitted XHTML violates XML 1.0's unique-attribute rule.
  - **Weak raw-text end-tag match:** `find_end_tag()` (`src/wikipedia.cpp:420-425`) and `find_tex()` (`src/texmath.cpp:232`) match `</script` / `</style` without checking that the following character is `>` or whitespace (so `</scripts>` terminates `<script>`).
- **Fix:**
  - Replace the integer `dropping` counter with a small `std::vector<std::string> drop_stack` that matches closing tag names the same way `stack` does.
  - Deduplicate emitted attribute names per start tag.
  - Require `>` or whitespace immediately after `name` in `find_end_tag()` and `texmath.cpp:232`.

### V4-BUG-6. `make_article_epub()` nests subsequent `<h3>` TOC items inside an initial `<h3>` when no `<h2>` precedes it
- **Severity:** Medium
- **Where:** `src/epubwrite.cpp:698-716`
- **Details:**
  - When the first entry in `book.toc` has `level == 3` (`!in_h2`), the `else` branch runs (`nav += li; in_h2 = true;`), treating that `<h3>` as an open `<h2>`. Any subsequent `<h3>` is then wrapped in `<ol>...</ol>` *inside* the first `<h3>` instead of appearing as a sibling.
- **Fix:**
  - Only set `in_h2 = true` when `s.level <= 2`; when `s.level == 3 && !in_h2`, emit `li + "</li>"` at top level.

### V4-BUG-7. Dual-source toast overwrite and missing source prefix when wallabag and Wikipedia both run
- **Severity:** Medium
- **Where:** `src/articles.cpp:223-244`, `src/app.cpp:79-83, 135-147`
- **Details:**
  - `App::update_articles()` starts wallabag and Wikipedia syncs concurrently, but `App` has a single `toast_` slot. If one source fails (showing a 6-second error toast) and the other finishes shortly afterward with `"Wikipedia: No new articles"` (2.2 s), the success toast immediately overwrites the error toast before the user can read it (though the error still remains visible on the source's start-screen row via `Articles::status()`).
  - Also, lines 227–228 and 234 omit the `who` prefix (`"wallabag: "`) when `pending` is non-empty, making it unclear which source produced the message when both are configured.
- **Fix:**
  - Always include `who` in `src/articles.cpp:227-234`, and avoid letting a short non-error toast replace an active error toast (or show a single combined toast when both concurrent updates finish).

### V4-BUG-8. Wikipedia articles get no library cover when their first image is an SVG
- **Severity:** Low
- **Where:** `src/wikipedia.cpp:914-915`, `src/articlesync.cpp:133-134`
- **Details:**
  - `run_wiki_sync()` passes `pics.front()` as `cover_url`. In `add_pictures()` (`src/articlesync.cpp:133`), `if (url == cover_url && media != "image/svg+xml") book.cover = href;`.
  - Many Wikipedia articles begin with an SVG map, diagram, emblem, or icon followed by raster photos. Because `cover_url` is an SVG (or if `pics.front()` fails to download), `book.cover` remains empty and none of the subsequent raster images are promoted to cover.
- **Fix:**
  - Allow Wikipedia (or any article whose `cover_url` was SVG / failed) to fall back to the first successfully downloaded non-SVG image as `book.cover`.

### V4-BUG-9. `densest()` ignores `w` descriptors in `srcset`, and `wiki_clean_page()` leaves dead non-citation `#id` links
- **Severity:** Low
- **Where:** `src/wikipedia.cpp:386-412, 675-676, 696-698`
- **Details:**
  - `densest()` only parses `x`/`X` descriptors (`1.5x`, `2x`); for `w` descriptors (`400w, 800w`), every candidate gets `x = 1` and only the first (smallest) URL is selected.
  - Line 675 preserves internal `#fragment` links (`<a href="#foo">`), while lines 696–698 strip `id` from all elements except `cite_note-*`, `cite_ref-*`, and `h2`/`h3`/`h4`, leaving internal links to tables, figures, or anchors as dead links.
- **Fix:**
  - Support `w` descriptors (comparing widths when `w` is used) in `densest()`.
  - Either unwrap non-citation `#...` links or preserve target `id`s that are referenced.

### V4-BUG-10. `SDL_RegisterEvents(1)` error check tests `0` instead of `(Uint32)-1`
- **Severity:** Low
- **Where:** `src/articles.cpp:47-48`
- **Details:**
  - `SDL_RegisterEvents(1)` returns `(Uint32)-1` (`UINT32_MAX`) on failure, not `0`. `ev ? ev : static_cast<uint32_t>(SDL_EVENT_USER)` would store `UINT32_MAX` if registration ever failed.
- **Fix:**
  - Check `ev != static_cast<Uint32>(-1)`.

---

## 3. UI / UX Issues

### V4-UI-1. Sorting Wikipedia's reading-lists folder by `Author` or `Recent` is a no-op, and empty lists force a single-folder wrapper
- **Severity:** Low
- **Where:** `src/library_screen.cpp:44-49, 478-483`, `src/library.cpp:152-169`
- **Details:**
  - `LibraryScreen` shares `article_sort_` (`Name`, `Author`, `Recent`, `Newest`) across wallabag articles, Wikipedia's top-level reading-lists folder, and individual reading lists. At the reading-lists folder level (`loc.list == 0`), all rows are `LibEntry::Folder` with no author and no `last_read` state, so cycling `Y` to `Author` or `Recent` displays `"Sorted by author"` / `"Sorted by last read"` while keeping the exact alphabetical `Name` order.
  - In `list_articles()` (`src/library.cpp:152-169`), `if (lists.size() == 1)` is checked *before* filtering out lists with 0 downloaded files on disk. If a user has 2 lists in `lists.json` and 1 has 0 downloaded articles (e.g., all its entries failed), Wikipedia still shows an extra 1-folder layer instead of opening straight into the single non-empty list.
- **Fix:**
  - Limit the reading-lists folder level to `Name` / `Newest` (or keep a separate sort mode for folders), and collapse to direct articles when only one list has downloaded files on disk.

### V4-UI-2. `[wikipedia] lists` overwrites earlier `lists =` lines instead of appending
- **Severity:** Low
- **Where:** `src/config.cpp:144-148`, `docs/CONFIGURATION.md:112`
- **Details:**
  - Unlike `[library] folder =` (which appends on repeated lines), `[wikipedia] lists =` calls `cfg.wikipedia.lists.clear()` on every line, so repeating `lists =` silently drops earlier lines, and list names containing a comma cannot be specified.
- **Fix:**
  - Append across repeated `lists =` lines (or support one list per line as well as comma-separated values) and document it.

---

## 4. Optimization Opportunities

### V4-OPT-1. `Articles::lists()` re-reads and parses `lists.json` from disk on every `LibraryScreen::list()` / download progress tick
- **Severity:** Medium
- **Where:** `src/articles.cpp:77-83, 178-181`, `src/library_screen.cpp:44-45`
- **Details:**
  - While viewing Wikipedia during a sync, every downloaded article triggers `on_wake()` → `refresh_library()` → `LibraryScreen::list()` → `src->lists()`, re-reading and parsing `lists.json` from disk on every single article tick and every folder navigation, even though `lists.json` is written only once per sync.
- **Fix:**
  - Cache the parsed `std::vector<ArticleList>` in `Articles` and reload it on startup and when a Wikipedia sync enters `Downloading` or completes.

### V4-OPT-2. `add_pictures()` blacklists `upload.wikimedia.org` for the whole article after a single picture timeout
- **Severity:** Medium
- **Where:** `src/articlesync.cpp:120-121`, `src/http.cpp:238-241`
- **Details:**
  - `CURLE_OPERATION_TIMEDOUT` sets `res.unreachable = true`, and `add_pictures()` inserts `host` into `dead_hosts` on the first `res.unreachable` response.
  - Because every Wikipedia article picture is served from `upload.wikimedia.org`, one slow/stalled large thumbnail or brief Wi-Fi hiccup causes `dead_hosts` to skip **all remaining pictures** in that article.
- **Fix:**
  - Blacklist a host immediately on DNS/connect/TLS failure, but only after 2–3 consecutive timeouts (not on a single transfer timeout).

### V4-OPT-3. Redundant HTML scans and copies during article sync
- **Severity:** Low
- **Where:** `src/wallabag.cpp:411-431`, `src/wikipedia.cpp:914-915`, `src/texmath.cpp:164-166`
- **Details:**
  - `wallabag.cpp:411, 416`: During rebuild, `find_tex(article.content)` runs twice on every article with math (once to test emptiness, again inside `add_math()`).
  - `wallabag.cpp:421, 424-431`: `article_book(article)` copies `article.content` into `book.html`, which `add_pictures()` / `rewrite_html()` immediately replaces.
  - `wikipedia.cpp:914` + `articlesync.cpp:90`: `article_images(html, base)` scans the full cleaned Wikipedia HTML twice per article (once just to pick `pics.front()`, then again inside `add_pictures()`).
  - `texmath.cpp:164-166`: `decode_entities()` runs twice on every inline `$...$` span (`looks_like_math()` and `add()`).
- **Fix:**
  - Pass the already-computed `TexSpan` vector into `add_math()`, move/omit `b.html` when `add_pictures()` will overwrite it, let `add_pictures()` select the first image itself when requested, and decode each inline `$...$` span once.

### V4-OPT-4. `StateStore::save()` evicts never-opened books marked as read first when capping at `MAX_BOOKS`
- **Severity:** Low
- **Where:** `src/state.cpp:183-190, 214-221`
- **Details:**
  - Marking a never-opened book as read (`mark_read(path, true)`, D34) leaves `b.last_read == 0`. If `books_.size()` ever exceeds `MAX_BOOKS` (5000), `StateStore::save()` evicts entries with the smallest `last_read` first, dropping manually marked-as-read books before any previously opened book.

---

## 5. Test Suite Gaps

### V4-TEST-1. `TEST(wallabag_epub)` initializes crengine without calling `engine::shutdown()`, causing `warn: font not loaded` in `TEST(tex_to_mathml_temml)`
- **Severity:** Low
- **Where:** `tests/unit_tests.cpp:1436-1452, 2396`
- **Details:**
  - `TEST(wallabag_epub)` calls `engine::init(fonts, data, temp_dir())` at line 1436 and never calls `engine::shutdown()`. When `TEST(tex_to_mathml_temml)` later calls `engine::init()` at line 2396, `fontMan` is already populated, emitting 13 `warn: font not loaded` warnings on every `tests/run.sh` run and testing against leftover engine state from an earlier test.
- **Fix:**
  - Add `engine::shutdown();` before the closing brace at `tests/unit_tests.cpp:1451`.

### V4-TEST-2. Missing regression tests for new v0.3.1 / v0.3.2 / v0.4.0 edge cases
- **Severity:** Low
- **Where:** `tests/unit_tests.cpp`
- **Details:**
  - Add unit tests covering:
    1. `wiki_clean_page()`: infobox with `lang`/`dir` before `class`, `<figure><figcaption><p>...</p></figcaption></figure>` between infobox and lead `<p>`, stray closing tag inside a dropped navbox, `<h3>` with `id="External_links"` inside an `<h2>` section.
    2. `run_wiki_sync()`: a page of only `"deleted": true` entries with a `"next"` cursor, and a repeated `"next"` cursor.
    3. `make_article_epub()`: TOC beginning with `<h3>` followed by another `<h3>` without an enclosing `<h2>`.
    4. `ReaderScreen` / `StateStore`: opening a book marked as read (`finished == true, xpointer == ""`) in both 1-page and 2-page spread layouts.

---

# DeckReader v0.3.0 Audit — Wallabag Article Sync

> **Reviewed 2026-11-20.** Outcome of every item (fixed, or declined with
> the reason) is recorded in [docs/DECISIONS.md](docs/DECISIONS.md) D32.

Audit of the `v0.3.0` candidate changes (`src/http.*`, `src/epubwrite.*`, `src/wallabag.*`, `src/articles.*`, `src/app.*`, `src/config.*`, `src/library*`, `src/reader_screen.cpp`, `src/state.*`, `src/ui.*`, `src/log.cpp`, `src/about.cpp`, `packaging/`, `scripts/`, `docs/`, and `tests/fake-wallabag.py` + `tests/unit_tests.cpp`).

Baseline: `tests/run.sh` passes (`639 checks, 0 failed`, `10` Python shortcut tests).

---

## 1. Security & Network Hardening

### V3-SEC-1. SSRF / loopback & LAN requests when fetching untrusted article pictures (and following redirects)
- **Severity:** High
- **Where:** `src/wallabag.cpp:214-257` (`add_pictures`), `src/http.cpp:98-102` (`HttpClient::request`)
- **Details:**
  - When `[wallabag] images = yes` (the default), `add_pictures()` fetches every `<img src>` / `data-src` / `srcset` and `preview_picture` URL found in untrusted web article HTML with `follow_redirects = true` (`CURLOPT_MAXREDIRS = 5`).
  - Neither the initial URL nor redirect targets are restricted from hitting loopback (`127.0.0.0/8`, `::1`, `localhost`), link-local / cloud metadata (`169.254.0.0/16`, `fe80::/10`), or LAN/RFC1918 addresses.
  - On a Steam Deck, local services listen on loopback (Steam client local services, Decky Loader, Syncthing, dev servers) and home routers/NAS admin endpoints sit on the LAN. A malicious saved web page containing `<img src="http://127.0.0.1:.../...">` (or redirecting there) causes DeckReader to issue GET requests to local/LAN services (and embed any image/SVG response into the EPUB).
  - Also, if an `<img>` URL contains embedded credentials (`http://user:pass@host/img`), libcurl sends `Authorization: Basic ...`.
- **Fix:**
  - For picture requests (when the picture's host differs from the configured wallabag server host), reject loopback, link-local, and unspecified targets (ideally via `CURLOPT_OPENSOCKETFUNCTION` / resolved `sockaddr` check so DNS rebinding and redirects to `127.0.0.1` / `::1` / `169.254.x.x` are also blocked, while keeping `tests/fake-wallabag.py` working because its pictures share the server's `127.0.0.1` host).
  - Reject or strip `user:pass@` userinfo from picture URLs in `resolve_url()`.

### V3-SEC-2. Unvalidated `access_token` interpolated into `Authorization` HTTP header
- **Severity:** Medium
- **Where:** `src/wallabag.cpp:90-95` (`parse_token`), `src/wallabag.cpp:342`, `src/http.cpp:93-96`
- **Details:**
  - `parse_token()` returns the JSON `"access_token"` string verbatim without validating characters, and `run_sync()` builds `"Authorization: Bearer " + token`. If a compromised server or HTTP MITM returns CR/LF (`\r`, `\n`) or control characters in `access_token`, it is passed into `CURLOPT_HTTPHEADER`.
- **Fix:**
  - Reject tokens in `parse_token()` that are empty or contain any control character (`<= 0x1F` or `0x7F`) or whitespace.

### V3-SEC-3. Tarpit / stalled picture hosts can hang a sync for up to 60 minutes per article
- **Severity:** Medium
- **Where:** `src/http.cpp:104-105`, `src/wallabag.cpp:233`
- **Details:**
  - `HttpClient::request()` sets `CURLOPT_CONNECTTIMEOUT = 20s` and `CURLOPT_TIMEOUT = req.timeout_s`, with **no** `CURLOPT_LOW_SPEED_LIMIT` / `CURLOPT_LOW_SPEED_TIME`.
  - `add_pictures()` uses `timeout_s = 60` per picture (up to `MAX_PICTURES = 60`). A broken, hotlink-blocked, or hostile image host that accepts TCP and stalls (sending 0 bytes or 1 byte every few seconds) holds the sync thread for 60 full seconds per request (and across multiple stalled hosts in a 50-article sync, tens of minutes).
- **Fix:**
  - Set `CURLOPT_LOW_SPEED_LIMIT` (e.g. `1024` bytes/s) and `CURLOPT_LOW_SPEED_TIME` (e.g. `15` seconds), and use tighter timeouts for picture fetches (e.g. `connect_timeout = 10s`, `timeout_s = 25s`).

### V3-SEC-4. Plaintext `http://` wallabag credentials and `config.ini` file permissions
- **Severity:** Low
- **Where:** `src/config.cpp:99-103`, `src/wallabag.cpp:302-323`, `docs/CONFIGURATION.md`
- **Details:**
  - `server = http://...` sends `client_id`, `client_secret`, `username`, `password`, and the bearer token in cleartext without logging a warning (legitimate for `127.0.0.1`/LAN self-hosted setups, risky if a user accidentally writes `http://app.wallabag.it`).
  - While DeckReader creates `config.ini` with `0600` (`write_file_atomic`), editing it in external editors can leave it `0644` (world-readable) with the wallabag password and client secret inside.
- **Fix:**
  - Log a `LOG_W` warning when `[wallabag] server` uses `http://` on a non-loopback host (and warn if `config.ini` has group/other read bits set when `[wallabag]` is configured).

### V3-SEC-5. `crengine-ng`'s `LVStreamFragment::Read` / `Seek` bypasses stored-entry size bounds (directly affects v0.3.0 article images)
- **Severity:** Medium
- **Where:** `.dev/src/crengine-ng/crengine/src/lvstream/lvstreamfragment.h:47-77`, `src/epubwrite.cpp:447`, `patches/crengine-ng-zip-hardening.patch`
- **Details:**
  - As noted in D31 (`docs/DECISIONS.md:622`), `LVStreamFragment::Read()` does not clamp `size` to `m_size - m_pos`, so any reader reading a stored (`method == 0`) ZIP entry until EOF reads past the end of the entry into subsequent ZIP entries / central directory.
  - Additionally, `LVStreamFragment::Seek()` ignores `pos` for `LVSEEK_END` (`pos = m_start + m_size` instead of `pos += m_start + m_size`) and does not anchor `LVSEEK_CUR` to `m_pos + m_start` if the underlying archive stream moved.
  - Because `crengine-ng-zip-hardening.patch` only capped deflated entries (`LVZipDecodeStream::read`) and v0.3.0 stores all raster article images uncompressed (`method == 0`), this leaves a gap in ZIP stream isolation for both article EPUBs and untrusted books.
- **Fix:**
  - Patch `lvstreamfragment.h` in `patches/crengine-ng-zip-hardening.patch` to clamp `Read()` to `m_size - m_pos` and fix `Seek()` for `LVSEEK_CUR` and `LVSEEK_END`, with a unit test reading a stored entry past its end.

---

## 2. Bugs — HTML/EPUB Builder (`src/epubwrite.cpp`) & Picture Fetching (`src/wallabag.cpp`)

### V3-BUG-1. `from_srcset()` breaks modern CDN URLs containing commas (Substack, Cloudinary, Cloudflare, Imgix, Medium)
- **Where:** `src/epubwrite.cpp:225-248` (`from_srcset`)
- **Details:**
  - `from_srcset()` splits on every comma (`split(srcset, ',')`).
  - Most modern image CDNs (Substack `substackcdn.com/image/fetch/w_848,c_limit,f_auto/...`, Cloudflare `/cdn-cgi/image/width=800,format=auto/...`, Cloudinary, Imgix, Ghost, NYTimes) put **commas inside the image URL itself**. `split(srcset, ',')` shatters those URLs at the first comma, causing 404s and silently dropping all images from those sites.
- **Fix:**
  - Implement the HTML5 `srcset` candidate-splitting rule: skip leading whitespace/commas, scan the URL up to whitespace (stopping only on a trailing `,` at the end of a URL without a descriptor), then scan the optional descriptor (`...w` / `...x`) up to the next `,` or end of string. Add unit tests with Substack/Cloudinary/Cloudflare-style comma URLs.

### V3-BUG-2. `picture_url()` checks `src` before lazy-load attributes (`data-src`, `data-original`, `data-lazy-src`)
- **Where:** `src/epubwrite.cpp:250-268` (`picture_url`)
- **Details:**
  - The loop checks `{"src", "data-src", "data-original", "data-lazy-src", "data-url"}` in that order.
  - Lazy-loading pages often put a real `https://.../placeholder.gif` or low-res blurred preview URL in `src` alongside the full-resolution URL in `data-src` / `data-lazy-src` / `data-original`. Because `src` resolves to a non-empty `http(s)` URL, `picture_url()` returns the placeholder and ignores `data-src` (the unit test only exercised `src="data:image/gif;base64,..."`, which `resolve_url` rejects).
- **Fix:**
  - Check `{"data-src", "data-original", "data-lazy-src", "data-url", "src"}` (and consider `data-srcset` / `srcset` before a placeholder `src`, or prefer `data-*` over `src`).

### V3-BUG-3. One malformed image URL (e.g. unencoded space) blacklists the entire host in `dead_hosts`
- **Where:** `src/wallabag.cpp:239-240`, `src/epubwrite.cpp:289-315` (`resolve_url`)
- **Details:**
  - `resolve_url()` does not percent-encode spaces (or other illegal URL chars) in relative/absolute image paths (e.g. `"my photo.jpg"`).
  - When passed to `libcurl`, `curl_easy_perform()` fails with `CURLE_URL_MALFORMAT`, setting `res.error` with `res.too_large == false`.
  - In `add_pictures()` (`src/wallabag.cpp:239-240`), `if (!res.error.empty() && !res.too_large) dead_hosts.insert(host);` immediately **blacklists that host for the rest of the article**, dropping every subsequent valid image on the same site!
  - Same issue if a single slow/huge image hits `CURLE_OPERATION_TIMEDOUT`: all remaining valid images from that host are skipped.
- **Fix:**
  - Only add a host to `dead_hosts` on transport-level host failures (DNS failure, connection refused/reset, TLS failure — expose a `bool host_unreachable` flag on `HttpResponse`), never on `CURLE_URL_MALFORMAT` or per-request read errors.
  - Percent-encode spaces (and reject control characters) in `resolve_url()`.

### V3-BUG-4. `resolve_url()` mishandles query-only bases, query-only refs, and `../` path segments
- **Where:** `src/epubwrite.cpp:289-315`
- **Details:**
  1. If `base` has no `/` after the host before `?` (e.g. `https://example.com?p=123`), `b.find('/', sep + 3)` returns `npos`, so `origin` includes `?p=123`, producing `https://example.com?p=123/img.png`.
  2. If `ref` starts with `?` (`?format=png`), line 313 strips the base filename (`path.rfind('/') + 1`) instead of keeping the base path before `?`.
  3. Relative `../` and `./` segments are not normalized, so `https://example.com/a/b/../img.png` and `https://example.com/a/img.png` fail to deduplicate in `article_images()` and are downloaded twice.
- **Fix:**
  - Find `path_at` with `b.find_first_of("/?", sep + 3)`, handle `r[0] == '?'`, and collapse `.` / `..` path segments.

### V3-BUG-5. `read_tag()`, `each_picture_tag()`, and `decode_entities()` edge cases corrupt image tags and `alt` text
- **Where:** `src/epubwrite.cpp:125-222, 271-285`
- **Details:**
  1. **Unquoted self-closing attribute (`src/epubwrite.cpp:213`):** `<img src=/img/a.png/>` stops only at whitespace or `>`, so `src` is parsed as `"/img/a.png/"` (404).
  2. **HTML comments (`src/epubwrite.cpp:271-285`):** `each_picture_tag()` does not skip `<!-- ... -->`, so commented-out `<img src="...">` tags are downloaded and rewritten inside the comment (and `--` inside `alt` produces invalid XML).
  3. **Double-escaped non-ASCII entities & XML control chars (`src/epubwrite.cpp:150-156, 344`):** `decode_entities()` only decodes numeric/named entities in `1..127` and leaves `&#8212;`, `&mdash;`, `&eacute;`, etc. untouched; `rewrite_images()` then runs `xml_escape()` on `alt`, double-escaping them into `&amp;#8212;` / `&amp;mdash;`. Meanwhile `strtol` ignores trailing garbage (`&#12abc;`), and `code > 0 && code < 128` decodes illegal XML 1.0 control characters (`0x01`–`0x08`, `0x0B`, `0x0C`, `0x0E`–`0x1F`) into attributes.
- **Fix:**
  - In `read_tag()`, stop unquoted values on `/` when followed by `>`.
  - In `each_picture_tag()`, skip `<!-- ... -->` comments.
  - In `decode_entities()`, validate full numeric entity strings with `endptr`, encode valid Unicode code points `>= 128` to UTF-8, and reject illegal XML 1.0 control characters.

### V3-BUG-6. `make_article_epub()` embeds raw wallabag HTML and unstripped XML control characters into XHTML
- **Where:** `src/epubwrite.cpp:108-120, 378-427`
- **Details:**
  - `xml_escape()` does not strip NUL bytes (`\0`, possible via JSON `\u0000`) or illegal XML 1.0 control characters (`< 0x20` other than `\t`, `\n`, `\r`), which can truncate or break XML parsing in `content.opf` and `article.xhtml`.
  - `b.html` is concatenated directly into `<div class="article">...</div>` of an `application/xhtml+xml` document. Untrusted `<style>` tags in wallabag HTML are applied by `crengine-ng` when *"Book's own styles"* is On (e.g. `display: none` or broken layout), and stray `</div></body></html>` or NUL bytes in `b.html` can break out of `<div class="article">` or drop the `<p class="source">` footer.
- **Fix:**
  - Strip NUL and illegal XML 1.0 control characters in `xml_escape()` and from `b.html`, and strip `<script>...</script>` / `<style>...</style>` blocks from `b.html` (or disable embedded styles/strip dangerous tags) before writing `article.xhtml`.

### V3-BUG-7. `preview_picture` (cover) is dropped on articles with `>= 60` images; SVG sniffing false positives
- **Where:** `src/wallabag.cpp:216-224`, `src/epubwrite.cpp:365-373`
- **Details:**
  1. `add_pictures()` appends `preview` at the end of `urls`, then loops `i < MAX_PICTURES` (60). Any article with 60+ images never downloads `preview`, leaving it with no cover thumbnail in the library. (Also, skipped images beyond 60 aren't added to `*left_out`.)
  2. `image_type()` classifies any response starting with `<` that contains `<svg` and not `<html` within the first 2048 bytes as `image/svg+xml`. An HTML error fragment without `<html` (e.g. `<!doctype html><div><svg>...`) or an XML error page mentioning `<svg` matches falsely.
- **Fix:**
  - Download/insert `preview` at index `0` (or reserve a slot for it) so the cover is always fetched first.
  - In `image_type()`, verify `<svg` is followed by whitespace/`>`/`/` and reject `<!doctype html` / `<head` / `<body`.

---

## 3. Bugs — Sync Logic, State & UI (`src/wallabag.cpp`, `src/articles.cpp`, `src/library_screen.cpp`, `src/reader_screen.cpp`, `src/config.cpp`)

### V3-BUG-8. No pagination on `/api/entries.json` — server `perPage` caps cause accidental article deletion
- **Where:** `src/wallabag.cpp:388-405`
- **Details:**
  - `run_sync()` requests `page=1&perPage=<max_articles>` (where `max_articles` can be up to `500`) in a single call and never inspects `pages` / `limit` in the JSON response.
  - If a wallabag server or proxy caps `perPage` below `s.max_articles` (returning `pages > 1`), `unread` contains only page 1; `plan_sync()` then places every valid unread local article from page 2+ into `r.remove`, **deleting them and wiping their reading positions and bookmarks** on the Deck!
  - Large single-page `perPage=500` requests can also hit PHP memory/timeout limits on small self-hosted servers.
- **Fix:**
  - Paginate `GET /api/entries.json` with a reasonable page size (e.g. `perPage = std::min(s.max_articles, 50)`) until `unread.size() >= s.max_articles` or `page >= total_pages`, stopping cleanly if a page returns 0 items.

### V3-BUG-9. `Articles::on_wake()` deletes `r.remove` and bumps `wallabag_updated` even on cancelled or failed-mid-download syncs
- **Where:** `src/articles.cpp:143-160`, `src/wallabag.cpp:401-415`
- **Details:**
  - `if (r.listed)` runs **before** `if (r.cancelled) return;` and without checking `r.error.empty()`.
  - If Wi-Fi drops halfway through downloading new articles (`r.listed == true`, `r.error != ""`), `app_.state.wallabag_updated` is still set to `wall_time()` (making the About/Articles row report it was updated just now even though the update failed). And if a cancel UI is wired up to `Articles::cancel()`, a cancelled sync after listing would still unlink `r.remove` and update `wallabag_updated`.
- **Fix:**
  - Handle `if (r.cancelled) return;` (after applying `r.archived` and recounting) before `if (r.listed)`, and only update `wallabag_updated` when `r.error.empty()` (still removing `r.remove` only when `r.listed && !r.cancelled`).

### V3-BUG-10. `parse_iso8601()` accepts negative hours/minutes/seconds, out-of-range timezone offsets, and rejects lowercase `'z'`
- **Where:** `src/wallabag.cpp:50-88`
- **Details:**
  - `std::sscanf(..., "%4d-%2d-%2dT%2d:%2d:%2d%n", ...)` accepts negative integers for `%2d` (e.g. `"-1"`), and line 54 checks `h > 23 || mi > 59 || sec > 60` without checking `h < 0 || mi < 0 || sec < 0`.
  - Timezone offset hours/minutes on line 74 are not checked against `0..23` / `0..59`.
  - Line 65 checks `rest == "Z"` and rejects valid lowercase `"z"`.
- **Fix:**
  - Check `h < 0 || mi < 0 || sec < 0`, validate timezone offset `oh <= 23 && om <= 59`, accept `"z"`, and verify `timegm(&tm) != static_cast<time_t>(-1)`.

### V3-BUG-11. `parse_config()` accepts `[wallabag] server = https://` (empty host) as configured
- **Where:** `src/config.cpp:99-103`
- **Details:**
  - `parse_config()` only checks `starts_with(to_lower(value), "https://") || starts_with(..., "http://")`, unlike `wallabag_base()` (`src/wallabag.cpp:38-48`) which also checks `!url_host(s).empty()`. Setting `server = https://` passes config parsing without warning, shows the Articles shelf, and only errors when updating.
- **Fix:**
  - Use `wallabag_base(value)` (or check non-empty host) in `parse_config()`.

### V3-BUG-12. `when_text()` formats 24h hours without leading zero (`9:05` vs `09:05`)
- **Where:** `src/articles.cpp:25` vs `src/system.cpp:46`
- **Details:**
  - `when_text()` uses `strf("%d:%02d", a.tm_hour, a.tm_min)` in 24-hour mode (`9:05`), inconsistent with `clock_text()` in `src/system.cpp:46` (`%02d:%02d` -> `09:05`).

### V3-BUG-13. UI polish gaps in `Articles` folder & Library menu while syncing
- **Where:** `src/library_screen.cpp:216-222, 449-450, 459-465`, `src/articles.h:39`
- **Details:**
  1. Line 450 draws the right-aligned sync status (`under`) under the article count with **no `max_w`**, so long hostnames or `"Marking as read in wallabag… 12 of 50"` can overlap `"Articles"` on the left at higher `ui_scale`.
  2. When entering an empty `Articles` folder while an update is already running (`app_.articles->busy()`), line 462–464 still tells the user `"No articles yet. Choose Update articles in the menu..."` instead of showing the live progress.
  3. `Articles::cancel()` exists in `src/articles.h:39`, but there is no way for the user to cancel a stalled/slow sync from the UI (clicking *"Update articles"* while busy just toasts `"Already updating the articles"`). Offering *"Cancel article update"* in the menu while `busy()` is true lets users abort slow updates without quitting the app.

---

## 4. Performance, Cache & Memory Optimizations

### V3-OPT-1. Concurrent picture downloads via `curl_multi` (5–10× faster syncs)
- **Where:** `src/http.h`, `src/http.cpp`, `src/wallabag.cpp:214-257`
- **Details:**
  - As measured in D31 (`docs/DECISIONS.md:564-566`), downloading 9 articles / 97 pictures sequentially on one easy handle took ~45 s (33 s for 20 SVGs on one slow site). With `max_articles = 50`, an initial sync can take several minutes.
  - Using `curl_multi` inside the same single `SyncWorker` thread (e.g. 4–6 concurrent picture transfers per article, preserving connection reuse & HTTP/2 multiplexing and zero extra threads) cuts multi-image article sync times dramatically.

### V3-OPT-2. Peak RAM spike (~120+ MB) when building image-heavy article EPUBs
- **Where:** `src/wallabag.cpp:422-436`, `src/epubwrite.cpp:46-104`
- **Details:**
  - With `MAX_PICTURES_TOTAL = 60 MB`, `book.images` holds up to 60 MB in RAM, and `zip_build()` constructs a second complete ~60 MB `std::string` in memory before `write_file_atomic()` writes it — a **120+ MB temporary spike** on top of the app's ~88 MB footprint (requirements §1 target: `< 100 MB`).
  - Also, `deflate_raw()` (`src/epubwrite.cpp:31`) uses `Z_BEST_COMPRESSION` (level 9) instead of `Z_DEFAULT_COMPRESSION` (level 6), spending extra CPU on large HTML/SVGs for ~1–2% savings; and `zip_build()`, `xml_escape()`, `decode_entities()`, `rewrite_images()` don't `reserve()` output buffers.
- **Fix:**
  - Stream ZIP entries directly to the atomic temp file fd (or release each `ArticleImage::data` as it is appended/written) and consider lowering `MAX_PICTURES_TOTAL` (e.g. 25–32 MB). Use `Z_DEFAULT_COMPRESSION` and `reserve()` string capacities.

### V3-OPT-3. Prune `MetaCache` & cover thumbnails when articles are removed
- **Where:** `src/articles.cpp:113-115, 149-151`, `src/metacache.h`
- **Details:**
  - `Articles::mark_read()` and `Articles::on_wake()` unlink `<id>.epub` and call `app_.state.forget(path)`, but never remove the article from `app_.meta` (`metadata.json` + `$XDG_CACHE_HOME/deckreader/covers/<hash>.png`). Unlike local books, wallabag articles churn continuously, so orphaned cover PNGs and metadata entries accumulate indefinitely.
- **Fix:**
  - Add `MetaCache::forget(const std::string& path)` (erasing from `meta_`, `covers_`, `scaled_`, unlinking `cover_file(path)`, and marking dirty) and call it alongside `state.forget(path)` in `Articles::mark_read()` and `Articles::on_wake()`.

### V3-OPT-4. Avoid redundant library refreshes during download progress and double save/cache write on *"Mark as read"*
- **Where:** `src/articles.cpp:131-134`, `src/app.cpp:116-120`, `src/reader_screen.cpp:453-460`
- **Details:**
  1. On every downloaded article (`p.done > shown_done_`), `Articles::on_wake()` calls `app_.refresh_library()`, which calls `library_->refresh()` even when the user is on the **Root screen** or inside **`~/Books`** — causing up to 50 redundant directory scans, `/proc/self/mountinfo` reads, and `meta_cursor_` resets while browsing non-Article folders. Only refresh the library list on download progress when viewing the `Articles` folder (and on completion when viewing `Articles` or `Root`).
  2. In `ReaderScreen::open_settings()` (`src/reader_screen.cpp:453-460`), *"Mark as read in wallabag"* calls `app_.show_library()` (which runs `Book::close()` -> `view_->updateCache()` writing a fresh crengine layout cache file, calls `state.save()`, and refreshes the library) and immediately calls `app_.articles->mark_read(path)` (which deletes the `.epub`, removes it from `state`, marks `state` dirty again, and refreshes the library a second time). Skipping `updateCache()` when closing an article to mark it read, and forgetting it before saving/refreshing, avoids wasted disk I/O and double work.

---

## 5. Missing Regression Tests

### V3-TEST-1. Suggested unit tests for `tests/unit_tests.cpp`
- `from_srcset` with commas inside CDN URLs (`V3-BUG-1`).
- `picture_url` preferring `data-src` / `data-lazy-src` over an `https://` placeholder `src` (`V3-BUG-2`).
- `read_tag` with unquoted self-closing `<img src=/a.png/>`, `each_picture_tag` skipping `<!-- <img ...> -->`, and `decode_entities` with `&#8212;` / `&mdash;` / invalid control chars (`V3-BUG-5`).
- `resolve_url` with `https://example.com?q=1`, `?w=800`, `../img.png`, and spaces (`V3-BUG-3`, `V3-BUG-4`).
- `parse_iso8601` rejecting negative hour/min/sec (`2026-11-19T-1:00:00Z`) and accepting lowercase `z` (`V3-BUG-10`).
- `parse_token` rejecting tokens with `\r`/`\n` (`V3-SEC-2`), and `parse_config` rejecting `server = https://` (`V3-BUG-11`).
- Multi-page `/api/entries.json` pagination in `tests/fake-wallabag.py` (`V3-BUG-8`).
- `LVStreamFragment::Read` stopping at the end of a stored ZIP entry (`V3-SEC-5`).

---

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
