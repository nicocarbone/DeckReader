# DeckReader — Software Requirements Specification

> Revision 2 (2026-11-17): reviewed against DeckVideoPlayer's actual code and
> decisions. Changes from revision 1: the technology stack is fixed (§2.1),
> the licence wording matches DeckVideoPlayer (§1, §3.2), the controller map
> follows DeckVideoPlayer's D10 (§5.1), and the sandbox, suspend, font and
> performance requirements are made precise (§4.4, §5.2, §6, §7). Reasons
> for each choice are recorded in [docs/DECISIONS.md](docs/DECISIONS.md).

## 1. Project Overview

**DeckReader** is a lightweight, offline-first e-book reader engineered specifically for **SteamOS Game Mode (Gamescope)** on the Valve Steam Deck (OLED primary target, LCD secondary target). It is a companion application to [**DeckVideoPlayer**](https://github.com/nicocarbone/DeckVideoPlayer), sharing its design philosophy, controller-first ergonomics, OLED visual polish, and SteamOS integration patterns.

### Core Principles

- **Ecosystem Continuity (`DeckVideoPlayer`):** Maintain architectural, visual, UX, and packaging consistency with `DeckVideoPlayer` so both applications feel like a unified native suite on the Steam Deck.
- **Simple \& Maintainable:** Keep the codebase modular, clean, and easy to audit. Avoid unnecessary abstraction layers or over-engineered state management.
- **Lightweight \& Fast:** Fast cold startup (first library frame \< 500 ms), low memory footprint (\< 100 MB of private memory while reading, measured on a Steam Deck), and zero background daemons.
- **Reuse Open Source \& Existing Project Patterns:** Leverage mature open-source libraries for EPUB parsing, text shaping/rasterization, and rendering, and reuse proven system/UI patterns from `DeckVideoPlayer` rather than reinventing them.
- **License:** DeckReader's own code is **GPL-2.0-or-later**, the same licence as `DeckVideoPlayer`'s code, so code can move freely between both projects. Third-party dependencies must be GPL-compatible; the distributed bundle carries the strictest licence among its parts (as DeckVideoPlayer's does, its D36). AGPL components are not used (§3.2).
- **Safe \& Resilient:** Defend against malformed EPUB archives and persist reading state atomically so SteamOS sleep or sudden battery depletion never corrupts progress.

---

## 2. Ecosystem Continuity (`DeckVideoPlayer` Reference)

When designing and implementing `DeckReader`, use **`https://github.com/nicocarbone/DeckVideoPlayer`** as the primary architectural and UX reference:

- **Codebase \& Stack Consistency:** Align build tooling (Flatpak on the Freedesktop runtime, per-user install), project directory layout, documentation layout (AGENTS.md, docs/ARCHITECTURE, DECISIONS, ROADMAP, DEVELOPMENT, CONFIGURATION), headless gamescope testing, and configuration/data storage paths with `DeckVideoPlayer`. The programming language differs by necessity (§2.1).
- **UI \& OSD Visual Continuity:** Match `DeckVideoPlayer`'s clean, high-contrast, OLED-friendly overlay aesthetic (Inter font hierarchy, the same colour tokens except the accent — DeckReader's is teal `#10A38F`, so the two apps are easy to tell apart — translucent dark panels, rounded selection pills, button-glyph hint bars, right-side drawer menus, toasts) on the same virtual 800-unit-tall canvas, so layouts scale from 1280×800 to 4K the same way.
- **Controller Muscle Memory:** Preserve `DeckVideoPlayer`'s button conventions for shared actions (opening/closing menus, toggling the HUD/OSD, navigating file lists, confirming, backing out, chapter skipping) so users never have to relearn controls between media apps (§5.1).
- **SteamOS Deployment Parity:** Replicate `DeckVideoPlayer`'s user-space installation script, update workflow, and automated Steam Game Mode non-Steam shortcut registration (including Steam grid/hero/logo/icon artwork in the same visual family).

### 2.1 Technology Stack

`DeckVideoPlayer` is mpv with a Lua interface drawn on mpv's OSD; mpv cannot lay out books, so DeckReader uses:

| Concern | Choice | Licence |
|---|---|---|
| Language | C++17 (the book engine's own language: no binding layer) | — |
| Book engine (EPUB 2/3, CSS, layout, hyphenation, images) | crengine-ng | GPL-2.0-or-later |
| Window, GPU presentation, gamepad, touch | SDL3 (Freedesktop runtime) | zlib |
| Interface text | FreeType + HarfBuzz (also used by crengine-ng) | FTL/GPLv2, MIT |
| State files | JSON (nlohmann/json), atomic writes | MIT |
| Packaging | Flatpak, `org.freedesktop.Platform` 26.08 | — |

Shared with `DeckVideoPlayer` directly: the SteamOS installer and Steam shortcut tool, the artwork style, the colour tokens and layout metrics, the button map, the headless test approach, and the documentation structure.

---

## 3. Disallowed Technologies \& Anti-Patterns (with Technical Reasons)

Specific libraries are flexible as long as they remain **GPL-compatible** and align with `DeckVideoPlayer`'s architecture, **provided the implementation avoids the following**:

1. **No Web Engines (Electron, Chromium/CEF, Tauri with WebKitGTK):**
    - *Technical reason:* Browser runtimes carry heavy idle RAM/CPU overhead (hundreds of megabytes and background helper processes) that degrade battery life. WebKitGTK and Chromium also suffer from DMA-BUF/Wayland rendering artifacts, broken hardware compositing, and input-focus traps inside SteamOS's **Gamescope** microcompositor.
2. **No AGPL or GPL-incompatible Dependencies:**
    - *Technical reason:* DeckReader's own code must stay GPL-2.0-or-later so it can be shared with `DeckVideoPlayer` in both directions. GPL-3.0-or-later components are acceptable in the bundle (DeckVideoPlayer's bundle is GPL-3.0-or-later because of FFmpeg), but AGPL components (e.g. MuPDF, KOReader's code) are excluded to keep the licence situation simple and the code reusable.
3. **No Continuous / Uncapped Frame Rendering Loops:**
    - *Technical reason:* Drawing static text continuously at 60–90 Hz prevents the Steam Deck's AMD APU from entering low-power idle states, wasting battery on unchanged frames. The main loop must be event-driven (blocking/sleeping until input, transition animation, or I/O occurs).
4. **No Custom From-Scratch HTML/CSS Layout Engines or ZIP Parsers:**
    - *Technical reason:* EPUB 2/3 layout rules, CSS cascading, embedded font handling, and ZIP container parsing contain decades of edge cases. Writing them from scratch bloats the codebase and introduces parser vulnerabilities.
5. **No External Browser OAuth2 Redirect Flows (e.g., Google Books / Google Drive Web Logins):**
    - *Technical reason:* Gamescope restricts applications to a single active window context. Spawning a web browser for OAuth2 redirects breaks controller navigation, fails localhost loopback callbacks, or leaves the user stranded outside the app in Game Mode. (Google Books also lacks a public API to download full commercial EPUB files due to Adobe DRM.)
6. **No System-Wide Package Dependencies (`/usr` modifications):**
    - *Technical reason:* SteamOS uses an immutable root filesystem (`steamos-readonly`) that wipes system-level packages during OS updates. All binaries, bundled libraries, and assets must reside in user space (a per-user Flatpak installation).
7. **No Continuous Smooth Vertical Scrolling for Reading:**
    - *Technical reason:* Continuous scrolling forces constant GPU redraws and triggers purple/gray pixel-response smearing ("black smear") on OLED panels when dark backgrounds transition at low brightness.

---

## 4. Display, Color \& Typography Requirements

### 4.1 Resolution \& Scaling

- Native support for **1280×800** (Steam Deck handheld display) with clean DPI-aware scaling for docked external monitors (**1920×1080**, **2560×1440**, **3840×2160**): the interface uses `DeckVideoPlayer`'s 800-unit virtual canvas, and book pages are laid out at the real pixel resolution with font sizes, margins and CSS absolute units scaled by the same factor.
- Respond cleanly to dynamic window resize / display change events when docking or undocking. Every re-layout (screen size or typography change) keeps the reading position exactly, however many changes are made in a row.
- On external screens the user sets Steam's *Game Resolution* to *Native* for full-resolution pages (as with `DeckVideoPlayer`).

### 4.2 Anti-Aliasing \& Subpixel Handling

- **Default (Steam Deck OLED \& LCD):** **Grayscale anti-aliasing** with light auto-hinting.
    - *Technical rationale:* Both Steam Deck panels are portrait panels rotated 90°, so their subpixels run vertically; standard horizontal RGB subpixel rendering causes color fringing on both internal screens.
- **Optional Setting:** Subpixel RGB or BGR rendering for users reading on an external desktop monitor, and vertical RGB/BGR subpixel rendering to try on the Deck's rotated panels (on the internal screens horizontal subpixel rendering only adds faint colour fringes).

### 4.3 Color Themes

Instant theme switching without reparsing or re-laying out the book:

1. **OLED Pure Black:** `#000000` background, `#C8C8C8` soft-white text (maximizes OLED power savings while avoiding harsh glare in dark rooms).
2. **OLED Smear-Safe Dark:** `#0E0E10` dark charcoal background, `#D4D4D4` text (prevents OLED pixel wake-up smearing during page turns).
3. **Sepia (Warm Paper):** `#E8D6AE` background, `#3A2816` ink (contrast 9.8:1; strengthened from `#F4ECD8`, which looked too close to white on the Deck OLED).
4. **Daylight White:** `#FAFAFA` background, `#1A1A1A` ink.

### 4.4 Typography Controls

- Bundle 3 high-legibility open-source fonts (SIL OFL): **Literata** (serif, designed for e-reading), **Inter** (sans-serif, also the interface font, as in `DeckVideoPlayer`) and **Atkinson Hyperlegible** (high legibility). A "Book's fonts" choice uses the book's embedded fonts. A dyslexia-oriented font (OpenDyslexic) may be added later.
- The chosen font applies even when a book's CSS names fonts it doesn't embed (e.g. `font-family: "Garamond"` with no generic family).
- Adjustable font size (14 – 36 in 1-step increments), in units of the 800-unit canvas (pixels on the Steam Deck; scaled on larger screens).
- Adjustable line spacing (1.1× – 1.8× line height, or the book's own), paragraph spacing (book / none / half line / one line), and horizontal/vertical page margins.
- Text alignment: the book's own, Justified, or Left-aligned. Hyphenation (language-aware, on by default) can be switched off.
- Two pages side by side when the screen is wide enough for the chosen text size (one page otherwise, or always one page by choice).

---

## 5. Controller UX \& SteamOS Game Mode Integration

### 5.1 Gamepad-First Navigation

Every screen (Library, Reader, Table of Contents, Settings) must be 100% operable using only standard gamepad inputs, **inheriting `DeckVideoPlayer`'s control conventions first** (its D10):

| Button | Library | Reader | Drawer menus |
|---|---|---|---|
| A | open / read | show or hide the HUD (DVP: A shows the playback overlay) | select |
| B | back / quit at the start screen | close the HUD, else back to the library | close |
| X | read from the start (DVP: play from start) | add or remove a bookmark | secondary action (e.g. delete bookmark) |
| Y | sort | cycle the color theme | — |
| D-pad ↑↓ | move | previous / next chapter | move |
| D-pad ←→ | page through the list | previous / next page | change value |
| L1 / R1 | page through the list | previous / next page | page through the list |
| L2 / R2 | first / last | previous / next chapter (DVP: chapters) | first / last |
| View (⧉) | book info (DVP: video info) | Table of Contents | — |
| Menu (☰) | menu (refresh, about, quit) | reading settings (and about) | — |
| Left stick | as the D-pad | **hold left/right to scrub** with a live page/chapter preview; up/down as the D-pad | as the D-pad |

The Steam and "…" buttons stay with Steam. Keyboard keys match `DeckVideoPlayer`'s (arrows, Enter, Backspace, x, y, m, i, PgUp/PgDn, Home/End, Ctrl+Q).

- **Touchscreen / Trackpad (Secondary):** In the reader, tap the left/right screen zones or swipe horizontally to turn pages, and tap the center to toggle the HUD; in lists and menus, tap a row to open it. Drags are never taken as taps. (This deliberately differs from `DeckVideoPlayer`, which ignores taps, because a reader's tap zones are unambiguous.)

### 5.2 SteamOS Lifecycle \& HUD

- **Suspend/Resume Safety:** The reading position is saved on every page turn (the first change after a pause immediately, then at most every 2 s), when the window loses focus (Steam overlay), when the book is closed and on exit (SIGTERM from Steam included). No sleep signal is needed: whatever was on screen when SteamOS slept is already on disk.
- **Screen stays awake while reading:** SteamOS dims the screen and sleeps on Steam's idle timer, which a reader would otherwise hit on every slow page. While a book is open the app asks Steam not to (a screensaver inhibition, as `DeckVideoPlayer` does during playback); after a configurable time without input (10 minutes by default) and in the library, Steam's own timers apply again, so a Deck left on the table still dims and sleeps.
- **About screen:** version and build, source code, licence, how to support the project, the details a bug report needs (versions, device, screen, keep-awake status, folders) and credits, as in `DeckVideoPlayer`; from the library menu and the reading menu.
- **Cursor Management:** Hide the mouse cursor automatically in Game Mode unless mouse/trackpad pointer movement is detected (consistent with `DeckVideoPlayer`).
- **Optional progress line:** a reading setting adds a discreet line under the text with the percentage read, the pages left in the chapter and the pages left in the book, in its own band (text never runs under it), at no cost to page turns.
- **Minimalist HUD (hidden by default while reading):** Styled consistently with `DeckVideoPlayer`'s OSD, displaying Book Title, Chapter Title, Current/Total Pages, Percentage Read, pages left in the chapter, and **System Battery % + Local Time** (essential in fullscreen Gamescope where the system top bar is hidden).

---

## 6. Library, Local Files \& Offline Network Sync

### 6.1 Local File Navigation

- Configurable root book folders in `~/.config/deckreader/config.ini`, defaulting to `~/Books`, plus removable microSD storage and USB drives under `/run/media/` (detected from `/proc/self/mountinfo` and named "SD Card" / "USB drive", as in `DeckVideoPlayer`).
- The Flatpak sandbox can read `~/Books`, Documents, Downloads and removable media (read-only, not the whole home folder, as in `DeckVideoPlayer`'s D39). Other folders need `flatpak override --user --filesystem=PATH:ro io.github.nicocarbone.DeckReader`, documented in `config.ini`.
- Support hierarchical folder browsing (consistent with `DeckVideoPlayer`'s file browser) and list the recently read books on the start screen below the sources, under a section header, as `DeckVideoPlayer` lists recently played videos: each with its progress and where it is ("Local", "SD Card", "USB"); how many is set in `config.ini` (`recently_read`, 0 hides them); a book can be removed from the list; the cursor starts on the book read last.
- Extract and cache EPUB titles, authors and cover thumbnails for fast library browsing (filled lazily, never blocking input).
- Primary format: `.epub` (EPUB 2 and EPUB 3). **DRM-protected books are not supported.**

### 6.2 Network Catalog \& Offline Downloads (Optional Phase 2)

- Support **OPDS (Open Publication Distribution System)** feeds—the open standard supported by self-hosted servers (Calibre-web, Kavita, Komga)—using simple HTTP/HTTPS + Basic/Token authentication. Credentials are first entered in `config.ini` (as `DeckVideoPlayer` does for its OpenSubtitles key): the Steam on-screen keyboard cannot be reliably opened by a non-Steam application. A controller-driven on-screen keyboard may follow, built so `DeckVideoPlayer` can reuse it (its roadmap needs a first text field too).
- **Offline Download Manager:** Download remote `.epub` files directly into the local library folder with progress feedback so books remain 100% readable offline.

---

## 7. Power, Stability \& Security Requirements

### 7.1 Power \& Performance

- **Event-Driven Loop:** Block on OS/windowing events (`SDL_WaitEventTimeout` with a timeout only when a timer is actually pending) so CPU and GPU usage drop to \~0% while viewing a static page.
- **No Background Filesystem Polling:** Scan local folders only at startup, when a folder is opened, or when the user triggers a manual refresh.
- **Adjacent Page Caching:** Keep the previous and next screens pre-rendered as GPU textures so page turns complete in \< 16 ms without CPU spikes.
- **Book opening:** Show feedback immediately; laid-out books are cached on disk, so reopening is fast. (A first open of a large book needs a full layout to know its page count.)

### 7.2 Security \& Packaging

- **Archive Hardening:** Validate all paths inside `.epub` (ZIP) containers against path traversal (`../` Zip Slip) and enforce sensible upper bounds on uncompressed entry sizes, totals, entry counts and compression ratios to prevent decompression bombs.
- **Atomic State Persistence:** Save reading position, bookmarks, and settings atomically (temporary file, `fsync`, rename, `fsync` of the folder), as JSON like `DeckVideoPlayer`, so sudden power loss never corrupts user data. An unreadable state file is set aside, never silently overwritten.
- **SteamOS Distribution Parity:** Package and install using the same user-space directory layout, install/uninstall scripts, and Steam Game Mode shortcut registration approach established in `DeckVideoPlayer`.
