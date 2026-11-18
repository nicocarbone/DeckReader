#include "ui.h"

#include <algorithm>
#include <cmath>

namespace dr {

Ui::Ui(Canvas& canvas, TextRenderer& text, double ui_scale, const UiTheme& th)
    : theme(th), c_(canvas), t_(text) {
    float scale = static_cast<float>(std::clamp(ui_scale, 0.5, 3.0));
    s_ = canvas.height() / BASE_HEIGHT * scale;
    h_ = canvas.height() / s_;
    w_ = canvas.width() / s_;
}

float Ui::text(float x, float y, const std::string& s, float size, Color c, bool bold, int align, float max_w) {
    if (s.empty())
        return 0;
    float px = size * s_;
    std::string str = max_w > 0 ? t_.ellipsize(s, px, bold, max_w * s_) : s;
    float w = t_.measure(str, px, bold);
    float xp = x * s_;
    if (align == ALIGN_CENTER)
        xp -= w / 2;
    else if (align == ALIGN_RIGHT)
        xp -= w;
    float baseline = y * s_ + t_.cap_height(px, bold) / 2;
    t_.draw(c_, xp, baseline, str, px, bold, c);
    return w / s_;
}

float Ui::text_w(const std::string& s, float size, bool bold) {
    return t_.measure(s, size * s_, bold) / s_;
}

std::vector<std::string> Ui::wrap(const std::string& s, float size, bool bold, float max_w, size_t max_lines) {
    return t_.wrap(s, size * s_, bold, max_w * s_, max_lines);
}

void Ui::rect(float x, float y, float w, float h, Color c, float radius) {
    c_.fill_rrect(x * s_, y * s_, w * s_, h * s_, radius * s_, c);
}

void Ui::stroke(float x, float y, float w, float h, Color c, float radius, float thickness) {
    c_.stroke_rrect(x * s_, y * s_, w * s_, h * s_, radius * s_, std::max(1.0f, thickness * s_), c);
}

void Ui::circle(float cx, float cy, float r, Color c) {
    c_.fill_circle(cx * s_, cy * s_, r * s_, c);
}

void Ui::line(float x0, float y0, float x1, float y1, float thickness, Color c) {
    c_.stroke_line(x0 * s_, y0 * s_, x1 * s_, y1 * s_, std::max(1.0f, thickness * s_), c);
}

void Ui::poly(std::initializer_list<float> pts, Color c) {
    std::vector<float> v(pts);
    for (float& f : v)
        f *= s_;
    c_.fill_polygon(v.data(), static_cast<int>(v.size() / 2), c);
}

void Ui::image(const Image& img, float x, float y, float w, float h, float opacity) {
    c_.draw_image(img, x * s_, y * s_, w * s_, h * s_, opacity);
}

void Ui::clip(float x, float y, float w, float h) {
    c_.set_clip(static_cast<int>(std::floor(x * s_)), static_cast<int>(std::floor(y * s_)),
                static_cast<int>(std::ceil((x + w) * s_)), static_cast<int>(std::ceil((y + h) * s_)));
}

void Ui::unclip() {
    c_.reset_clip();
}

float Ui::button(Glyph g, float x, float cy, float size) {
    Color white = col(0xFFFFFF);
    Color ink = col(theme.bg);
    float r = size / 2;
    auto letter = [&](const char* l) {
        circle(x + r, cy, r, white);
        text(x + r, cy, l, size * 0.58f, ink, true, ALIGN_CENTER);
        return size;
    };
    switch (g) {
    case Glyph::A: return letter("A");
    case Glyph::B: return letter("B");
    case Glyph::X: return letter("X");
    case Glyph::Y: return letter("Y");
    case Glyph::View: {
        circle(x + r, cy, r, white);
        float q = size * 0.3f, t = std::max(size * 0.07f, 1.5f);
        stroke(x + r - q * 0.85f, cy - q * 0.85f, q * 1.1f, q * 1.1f, ink, q * 0.25f, t);
        rect(x + r - q * 0.3f, cy - q * 0.3f, q * 1.1f, q * 1.1f, white, q * 0.25f);
        stroke(x + r - q * 0.3f, cy - q * 0.3f, q * 1.1f, q * 1.1f, ink, q * 0.25f, t);
        return size;
    }
    case Glyph::Menu: {
        circle(x + r, cy, r, white);
        float lw = size * 0.44f, t = std::max(size * 0.08f, 1.5f);
        for (int i = -1; i <= 1; i++)
            rect(x + r - lw / 2, cy + i * size * 0.17f - t / 2, lw, t, ink, t / 2);
        return size;
    }
    case Glyph::L1:
    case Glyph::R1:
    case Glyph::L2:
    case Glyph::R2: {
        static const char* names[] = {"L1", "R1", "L2", "R2"};
        const char* n = names[static_cast<int>(g) - static_cast<int>(Glyph::L1)];
        float w = size * 1.45f;
        rect(x, cy - r, w, size, white, size * 0.3f);
        text(x + w / 2, cy, n, size * 0.52f, ink, true, ALIGN_CENTER);
        return w;
    }
    case Glyph::DPad: {
        circle(x + r, cy, r, white);
        float a = size * 0.36f, t = size * 0.2f;
        rect(x + r - a, cy - t / 2, 2 * a, t, ink, t * 0.2f);
        rect(x + r - t / 2, cy - a, t, 2 * a, ink, t * 0.2f);
        return size;
    }
    case Glyph::Stick: {
        circle(x + r, cy, r, white);
        circle(x + r, cy, r * 0.62f, ink);
        circle(x + r, cy, r * 0.42f, white);
        return size;
    }
    }
    return size;
}

void Ui::hints_inline(const std::vector<Hint>& hints, float x_right, float cy) {
    const float glyph = 26, gap = 10, between = 30, size = 19;
    float x = x_right;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        float lw = text_w(it->label, size);
        float gw = (it->glyph >= Glyph::L1 && it->glyph <= Glyph::R2) ? glyph * 1.45f : glyph;
        x -= lw;
        text(x, cy, it->label, size, col(theme.text));
        x -= gap + gw;
        button(it->glyph, x, cy, glyph);
        x -= between;
    }
}

void Ui::hint_bar(const std::vector<Hint>& hints) {
    float y = h_ - HINT_BAR_HEIGHT;
    rect(0, y, w_, HINT_BAR_HEIGHT, col(theme.bar));
    hints_inline(hints, w_ - 40, y + HINT_BAR_HEIGHT / 2);
}

void Ui::icon_folder(float x, float cy, float s, Color c) {
    float h = s * 0.74f, y = cy - h / 2 + s * 0.06f;
    rect(x, y - s * 0.1f, s * 0.42f, s * 0.22f, c, s * 0.06f);
    rect(x, y, s, h, c, s * 0.1f);
}

void Ui::icon_book(float x, float cy, float s, Color c) {
    float w = s * 0.72f, h = s, y = cy - h / 2;
    rect(x + (s - w) / 2, y, w, h, c, s * 0.08f);
    rect(x + (s - w) / 2 + w * 0.16f, y, w * 0.06f, h, col(theme.bg, 0.55f));
    rect(x + (s - w) / 2 + w * 0.34f, y + h * 0.22f, w * 0.48f, h * 0.07f, col(theme.bg, 0.55f), h * 0.03f);
    rect(x + (s - w) / 2 + w * 0.34f, y + h * 0.36f, w * 0.36f, h * 0.07f, col(theme.bg, 0.55f), h * 0.03f);
}

void Ui::icon_card(float x, float cy, float s, Color c) {
    float w = s * 0.74f, h = s, y = cy - h / 2, x0 = x + (s - w) / 2, cut = w * 0.32f;
    poly({x0, y + h, x0, y + cut, x0 + cut, y, x0 + w, y, x0 + w, y + h}, c);
    for (int i = 0; i < 4; i++)
        rect(x0 + w * (0.34f + i * 0.15f), y + h * 0.08f, w * 0.08f, h * 0.2f, col(theme.bg, 0.6f));
}

void Ui::icon_usb(float x, float cy, float s, Color c) {
    // A USB stick: body, connector, two contacts.
    float w = s * 0.5f, x0 = x + (s - w) / 2;
    float top = cy - s / 2, plug = s * 0.3f;
    rect(x0 + w * 0.15f, top, w * 0.7f, plug, c, s * 0.04f);
    rect(x0 + w * 0.3f, top + plug * 0.3f, w * 0.12f, plug * 0.3f, col(theme.bg, 0.7f));
    rect(x0 + w * 0.58f, top + plug * 0.3f, w * 0.12f, plug * 0.3f, col(theme.bg, 0.7f));
    rect(x0, top + plug, w, s - plug, c, s * 0.1f);
}

void Ui::icon_recent(float x, float cy, float s, Color c) {
    float r = s / 2, cx = x + r;
    circle(cx, cy, r, c);
    circle(cx, cy, r * 0.8f, col(theme.bg));
    line(cx, cy, cx, cy - r * 0.5f, s * 0.1f, c);
    line(cx, cy, cx + r * 0.38f, cy + r * 0.22f, s * 0.1f, c);
}

void Ui::icon_check(float cx, float cy, float r) {
    circle(cx, cy, r, col(theme.good));
    float t = r * 0.26f;
    line(cx - r * 0.45f, cy + r * 0.02f, cx - r * 0.12f, cy + r * 0.36f, t, col(theme.bg));
    line(cx - r * 0.12f, cy + r * 0.36f, cx + r * 0.48f, cy - r * 0.32f, t, col(theme.bg));
}

void Ui::icon_bookmark(float x, float y, float w, float h, Color c) {
    // A ribbon with a notch: fill_polygon only does convex shapes, so a
    // rectangle and the two tails (D22).
    float notch = w * 0.45f, body = h - notch;
    rect(x, y, w, body, c);
    poly({x, y + body - 0.5f, x + w / 2, y + body - 0.5f, x, y + h}, c);
    poly({x + w / 2, y + body - 0.5f, x + w, y + body - 0.5f, x + w, y + h}, c);
}

void Ui::icon_battery(float x, float cy, float h, int percent, bool charging, Color c) {
    float w = h * 1.9f, y = cy - h / 2, t = std::max(h * 0.12f, 1.2f);
    stroke(x, y, w, h, c, h * 0.22f, t);
    rect(x + w + t * 0.4f, cy - h * 0.22f, t * 1.4f, h * 0.44f, c, t * 0.5f);
    float inner = w - 4 * t;
    float frac = std::clamp(percent / 100.0f, 0.0f, 1.0f);
    Color fill = percent <= 15 && !charging ? col(theme.bad) : c;
    if (frac > 0)
        rect(x + 2 * t, y + 2 * t, std::max(inner * frac, t), h - 4 * t, fill, h * 0.08f);
    if (charging) {
        // A lightning bolt from two triangles (fill_polygon wants convex shapes).
        float bx = x + w / 2, by = cy;
        poly({bx + h * 0.10f, by - h * 0.46f, bx - h * 0.24f, by + h * 0.06f, bx + h * 0.06f, by + h * 0.06f},
             col(theme.warn));
        poly({bx - h * 0.10f, by + h * 0.46f, bx + h * 0.24f, by - h * 0.06f, bx - h * 0.06f, by - h * 0.06f},
             col(theme.warn));
    }
}

void Ui::progress(float x, float y, float w, float h, float frac, Color fg, Color bg) {
    rect(x, y, w, h, bg, h / 2);
    frac = std::clamp(frac, 0.0f, 1.0f);
    if (frac > 0)
        rect(x, y, std::max(w * frac, h), h, fg, h / 2);
}

} // namespace dr
