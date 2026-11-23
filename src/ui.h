// Interface toolkit: DeckVideoPlayer's look (dvp/ui.lua) on a Canvas.
//
// Layout uses a virtual canvas 800 units tall (the Deck's native height,
// divided by ui_scale) and as wide as the screen's aspect ratio requires, so
// one layout serves 1280x800 and 3840x2160 alike (DeckVideoPlayer D11).
#pragma once

#include "canvas.h"
#include "text.h"

#include <string>
#include <vector>

namespace dr {

struct UiTheme {
    uint32_t bg = 0x0E1217;
    uint32_t glow = 0x18222E;
    uint32_t panel = 0x181E26;
    uint32_t panel2 = 0x232B36;
    uint32_t bar = 0x1A2029;
    uint32_t accent = 0x10A38F;
    uint32_t text = 0xFFFFFF;
    uint32_t subtext = 0x9AA7B5;
    uint32_t dim = 0x5C6773;
    uint32_t good = 0x5BD18B;
    uint32_t warn = 0xF2B84B;
    uint32_t bad = 0xFF6464;
};

enum class Glyph { A, B, X, Y, View, Menu, L1, R1, L2, R2, DPad, Stick };

struct Hint {
    Glyph glyph;
    std::string label;
};

enum Align { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

class Ui {
public:
    static constexpr float BASE_HEIGHT = 800.0f;
    static constexpr float HINT_BAR_HEIGHT = 72.0f;

    Ui(Canvas& canvas, TextRenderer& text, double ui_scale, const UiTheme& theme);

    float W() const { return w_; }  // canvas width in units
    float H() const { return h_; }  // canvas height in units (800 / ui_scale)
    float scale() const { return s_; } // pixels per unit
    Color col(uint32_t rgb, float a = 1.0f) const { return Color::hex(rgb, a); }

    // Text whose capital letters are vertically centred on y. Returns the
    // width drawn. max_w > 0 ellipsizes.
    float text(float x, float y, const std::string& s, float size, Color c, bool bold = false, int align = ALIGN_LEFT,
               float max_w = 0);
    float text_w(const std::string& s, float size, bool bold = false);
    std::vector<std::string> wrap(const std::string& s, float size, bool bold, float max_w, size_t max_lines);

    void rect(float x, float y, float w, float h, Color c, float radius = 0);
    void stroke(float x, float y, float w, float h, Color c, float radius, float thickness);
    void circle(float cx, float cy, float r, Color c);
    void line(float x0, float y0, float x1, float y1, float thickness, Color c);
    void poly(std::initializer_list<float> pts, Color c);
    void image(const Image& img, float x, float y, float w, float h, float opacity = 1.0f);
    void clip(float x, float y, float w, float h);
    void unclip();

    // A controller button as drawn in the hint bar; returns its width.
    float button(Glyph g, float x, float cy, float size = 26);
    // The footer of every full screen: hints right-aligned on a darker bar.
    void hint_bar(const std::vector<Hint>& hints);
    // Same hints without the bar, right-aligned ending at x_right (panels).
    void hints_inline(const std::vector<Hint>& hints, float x_right, float cy);

    void icon_folder(float x, float cy, float size, Color c);
    void icon_book(float x, float cy, float size, Color c);
    void icon_card(float x, float cy, float size, Color c);
    void icon_usb(float x, float cy, float size, Color c);
    void icon_articles(float x, float cy, float size, Color c); // a news page
    void icon_wikipedia(float x, float cy, float size, Color c); // a "W" on a page
    void icon_check(float cx, float cy, float r);
    void icon_bookmark(float x, float y, float w, float h, Color c);
    void icon_battery(float x, float cy, float h, int percent, bool charging, Color c);
    void progress(float x, float y, float w, float h, float frac, Color fg, Color bg);

    const UiTheme& theme;

private:
    Canvas& c_;
    TextRenderer& t_;
    float s_, w_, h_;
};

} // namespace dr
