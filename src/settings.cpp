#include "settings.h"

#include "util.h"

#include <algorithm>

namespace dr {

const ThemeDef THEMES[4] = {
    {"oled-black", "OLED Black", 0x000000, 0xC8C8C8, true},
    {"oled-dark", "OLED Dark", 0x0E0E10, 0xD4D4D4, true},
    {"sepia", "Sepia", 0xE8D6AE, 0x3A2816, false},
    {"day", "Day", 0xFAFAFA, 0x1A1A1A, false},
};

const FontDef FONTS[4] = {
    {"literata", "Literata", "Literata"},
    {"inter", "Inter", "Inter"},
    {"atkinson", "Atkinson Hyperlegible", "Atkinson Hyperlegible"},
    {"book", "", "Book's fonts"},
};

void ReaderSettings::clamp() {
    theme = std::clamp(theme, 0, THEME_COUNT - 1);
    if (font_index() < 0)
        font = FONTS[0].id;
    font_size = std::clamp(font_size, 14, 36);
    if (line_spacing != 0)
        line_spacing = std::clamp(line_spacing, 11, 18);
    para_spacing = std::clamp(para_spacing, 0, 3);
    side_margin = std::clamp(side_margin, 0, 120);
    vert_margin = std::clamp(vert_margin, 0, 96);
    align = std::clamp(align, 0, 2);
    pages = pages == 1 ? 1 : 2;
    text_aa = std::clamp(text_aa, 0, TEXT_AA_COUNT - 1);
}

int ReaderSettings::font_index() const {
    for (int i = 0; i < FONT_COUNT; i++)
        if (font == FONTS[i].id)
            return i;
    return -1;
}

bool ReaderSettings::layout_differs(const ReaderSettings& o) const {
    return font != o.font || font_size != o.font_size || line_spacing != o.line_spacing ||
           para_spacing != o.para_spacing || side_margin != o.side_margin || vert_margin != o.vert_margin ||
           align != o.align || pages != o.pages || hyphenation != o.hyphenation || text_aa != o.text_aa ||
           book_styles != o.book_styles;
}

std::string line_spacing_label(int v) {
    if (v == 0)
        return "Book";
    return strf("%d.%d\xC3\x97", v / 10, v % 10); // "1.4×"
}

std::string para_spacing_label(int v) {
    static const char* names[] = {"Book", "None", "Half line", "One line"};
    return names[std::clamp(v, 0, 3)];
}

std::string align_label(int v) {
    static const char* names[] = {"Book", "Justified", "Left"};
    return names[std::clamp(v, 0, 2)];
}

std::string text_aa_label(int v) {
    static const char* names[] = {"Grayscale", "RGB subpixels", "BGR subpixels", "Vertical RGB", "Vertical BGR"};
    return names[std::clamp(v, 0, TEXT_AA_COUNT - 1)];
}

} // namespace dr
