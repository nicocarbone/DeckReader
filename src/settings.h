// Reading settings changed from the reader's menu (theme, font, sizes...).
// They apply to every book and are saved in the state file.
#pragma once

#include <cstdint>
#include <string>

namespace dr {

struct ThemeDef {
    const char* id;
    const char* name;
    uint32_t bg;   // 0xRRGGBB
    uint32_t text; // 0xRRGGBB
    bool dark;
};
// Requirements 4.3, in cycling order (Y in the reader).
extern const ThemeDef THEMES[4];
constexpr int THEME_COUNT = 4;

struct FontDef {
    const char* id;   // stored in the state file
    const char* face; // crengine face (family) name; "" = the book's own fonts
    const char* name; // shown in the menu
};
extern const FontDef FONTS[4];
constexpr int FONT_COUNT = 4;
constexpr int TEXT_AA_COUNT = 5;
// Height of the progress line's band below the text, in canvas units.
constexpr int PROGRESS_LINE_UNITS = 30;

struct ReaderSettings {
    int theme = 0;               // index into THEMES
    std::string font = "literata";
    int font_size = 24;          // 14..36, in units of the 800-tall canvas (D12)
    int line_spacing = 14;       // tenths of an em, 11..18; 0 = the book's
    int para_spacing = 0;        // 0 = book, 1 = none, 2 = half line, 3 = one line
    int side_margin = 40;        // units, 0..120
    int vert_margin = 24;        // units, 0..96
    int align = 0;               // 0 = book, 1 = justified, 2 = left
    int pages = 2;               // 1, or 2 = two pages side by side when wide enough
    bool hyphenation = true;
    int text_aa = 0;             // 0 = grayscale, 1/2 = RGB/BGR subpixels, 3/4 = vertical RGB/BGR
    bool book_styles = true;     // the book's own CSS
    bool progress_line = false;  // percent and pages left at the bottom (D24)

    // Brings every field back into its valid range.
    void clamp();
    int font_index() const;
    // Whether a change from `other` needs a new layout (not just new colors).
    bool layout_differs(const ReaderSettings& other) const;
};

// Labels for the menu.
std::string line_spacing_label(int v);
std::string para_spacing_label(int v);
std::string align_label(int v);
std::string text_aa_label(int v);

} // namespace dr
