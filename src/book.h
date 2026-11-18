// The book engine: a thin wrapper around crengine-ng's LVDocView (D2).
//
// crengine parses the EPUB, applies its CSS, lays the text out into pages
// and draws a page into a 32-bit buffer. Nothing here knows about SDL; the
// reader screen uploads the buffers as textures.
#pragma once

#include "canvas.h"
#include "settings.h"

#include <memory>
#include <string>
#include <vector>

class LVDocView;

namespace dr {

struct TocEntry {
    std::string title;
    int level = 0; // 0 = top level
    int page = 0;
};

// Chapter lookups on a TOC whose entries may be in any page order (D22).
// Index of the entry with the largest page <= page (-1 if none).
int toc_chapter_at(const std::vector<TocEntry>& toc, int page);
// Smallest entry page > last_visible, or -1.
int toc_next_page(const std::vector<TocEntry>& toc, int last_visible);
// Largest entry page < page, or -1.
int toc_prev_page(const std::vector<TocEntry>& toc, int page);

struct BookInfo {
    std::string title, author, language;
};

namespace engine {
// One-time setup: crengine log level, our bundled fonts, hyphenation
// patterns, the layout cache folder. Returns false if no font loads.
bool init(const std::string& fonts_dir, const std::string& crengine_data_dir, const std::string& cache_dir);
void shutdown();
// Families crengine registered (for checks and the About screen).
std::vector<std::string> font_faces();
// Checks the ZIP directory of an EPUB (epubguard.h). "" when it is safe.
std::string check_book_file(const std::string& path);
// Title/author/language from the book's metadata, and its cover scaled to
// cover_h pixels high (cover may stay empty). Parses only the OPF, so it is
// fast enough to run for every book in a folder.
bool read_metadata(const std::string& path, BookInfo& info, Image* cover, int cover_h);
} // namespace engine

class Book {
public:
    Book();
    ~Book();
    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;

    // Opens (and checks) the file. On failure *error says why, for the user.
    bool open(const std::string& path, std::string* error);
    void close();
    bool is_open() const { return open_; }
    const std::string& path() const { return path_; }
    const BookInfo& info() const { return info_; }

    // Applies reading settings and the viewport. `unit` is pixels per
    // interface unit (window height / 800), so font sizes and margins scale
    // with the screen. Re-lays out the book only when something changed; the
    // reading position is kept. Returns true if pages changed.
    bool configure(const ReaderSettings& s, int width_px, int height_px, float unit);
    // Instant theme switch: no new layout (requirements 4.3).
    void set_colors(uint32_t bg, uint32_t text);

    int page_count() const;
    int current_page() const;
    int pages_per_screen() const; // 1, or 2 when two pages fit side by side
    void go_to_page(int page);
    // Draws the screen that starts at `page` into px (w*h XRGB pixels).
    bool render(int page, uint32_t* px, int w, int h);

    // The reading position as a crengine XPointer string: the start of the
    // screen the reader navigated to. Re-layouts (font, size, margins...)
    // restore this same anchor rather than re-reading the start of whatever
    // page they land on, so repeated changes never drift (D14).
    std::string position();
    bool go_to_position(const std::string& xpointer);
    int page_of_position(const std::string& xpointer);
    // 0..10000 for the current screen.
    int percent();

    const std::vector<TocEntry>& toc() const { return toc_; }
    // Index of the TOC entry that contains `page` (-1 before the first).
    int chapter_at(int page) const;
    std::string chapter_title_at(int page) const;
    // First page of the next / previous chapter from `page`.
    int next_chapter_page(int page) const;
    int prev_chapter_page(int page) const;

private:
    void rebuild_toc();
    std::string make_css(const ReaderSettings& s) const;

    std::unique_ptr<LVDocView> view_;
    bool open_ = false;
    std::string path_;
    BookInfo info_;
    std::vector<TocEntry> toc_;
    // What the current layout was made for.
    bool configured_ = false;
    std::string anchor_; // see position()
    ReaderSettings applied_;
    int width_ = 0, height_ = 0;
    float unit_ = 0;
};

} // namespace dr
