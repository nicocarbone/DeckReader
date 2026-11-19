#include "menu.h"

#include "util.h"

#include <algorithm>

namespace dr {

namespace {
constexpr float TOP = 107;      // first row
constexpr float BOTTOM = 72;    // room for the hints
constexpr float ROW_H = 52;
constexpr float PAD = 16;       // pill inset
constexpr float TEXT_SIZE = 20;
} // namespace

int Menu::visible_rows(float canvas_h) const {
    return std::max(1, static_cast<int>((canvas_h - TOP - BOTTOM) / ROW_H));
}

bool Menu::selectable(int i) const {
    return i >= 0 && i < static_cast<int>(items.size()) && !items[static_cast<size_t>(i)].header &&
           !items[static_cast<size_t>(i)].separator;
}

int Menu::step(int from, int dir, bool wrap) const {
    const int n = static_cast<int>(items.size());
    int i = from;
    for (int k = 0; k < n; k++) {
        i += dir;
        if (i < 0 || i >= n) {
            if (!wrap)
                break;
            i = (i + n) % n;
        }
        if (selectable(i))
            return i;
    }
    return from;
}

int Menu::nearest(int i, int dir) const {
    if (selectable(i))
        return i;
    int j = step(i, dir, false);
    return j != i ? j : step(i, -dir, false);
}

void Menu::keep_visible(int rows) {
    if (selected < scroll_)
        scroll_ = selected;
    // Scrolling up onto a section's first row shows its header too.
    if (selected > 0 && selected == scroll_ && items[static_cast<size_t>(selected - 1)].header)
        scroll_ = selected - 1;
    if (selected >= scroll_ + rows)
        scroll_ = selected - rows + 1;
    scroll_ = std::clamp(scroll_, 0, std::max(0, static_cast<int>(items.size()) - rows));
}

void Menu::select_marked() {
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].marked)
            selected = static_cast<int>(i);
}

float Menu::drawn_width(float canvas_w) const {
    return std::min(width, canvas_w * 0.85f);
}

bool Menu::handle(const InputEvent& e) {
    const int n = static_cast<int>(items.size());
    const int page = std::max(1, rows_drawn_ - 1);
    if (n && !selectable(selected))
        selected = nearest(selected, 1);
    switch (e.action) {
    case Action::Back:
        return false;
    // A press wraps around; holding stops at the ends (as in the library).
    case Action::Up:
        if (n)
            selected = step(selected, -1, !e.repeat);
        break;
    case Action::Down:
        if (n)
            selected = step(selected, 1, !e.repeat);
        break;
    case Action::L1:
        if (n)
            selected = nearest(std::max(0, selected - page), -1);
        break;
    case Action::R1:
        if (n)
            selected = nearest(std::min(n - 1, selected + page), 1);
        break;
    case Action::L2:
        if (n)
            selected = nearest(0, 1);
        break;
    case Action::R2:
        if (n)
            selected = nearest(n - 1, -1);
        break;
    case Action::Left:
    case Action::Right:
        if (n && items[static_cast<size_t>(selected)].adjust)
            items[static_cast<size_t>(selected)].adjust(e.action == Action::Left ? -1 : 1);
        break;
    case Action::Confirm:
        if (n) {
            MenuItem& it = items[static_cast<size_t>(selected)];
            if (it.activate)
                it.activate();
            else if (it.adjust)
                it.adjust(1);
        }
        break;
    case Action::X:
        if (n && items[static_cast<size_t>(selected)].secondary)
            items[static_cast<size_t>(selected)].secondary();
        break;
    default:
        break;
    }
    selected = std::clamp(selected, 0, std::max(0, static_cast<int>(items.size()) - 1));
    return !closed;
}

void Menu::draw(Ui& ui) {
    const UiTheme& th = ui.theme;
    float x0 = ui.W() - drawn_width(ui.W());
    float w = ui.W() - x0;
    ui.rect(x0, 0, w, ui.H(), ui.col(th.panel, 0.97f));
    ui.text(x0 + 32, 52, title, 26, ui.col(th.text), true, ALIGN_LEFT, w - 64);

    if (!items.empty() && !selectable(selected))
        selected = nearest(selected, 1);
    int rows = visible_rows(ui.H());
    rows_drawn_ = rows;
    keep_visible(rows);
    row_y0_ = TOP;
    row_h_ = ROW_H;

    if (items.empty())
        ui.text(x0 + 32, TOP + ROW_H / 2, empty_text, TEXT_SIZE, ui.col(th.subtext), false, ALIGN_LEFT, w - 64);

    for (int r = 0; r < rows; r++) {
        int i = scroll_ + r;
        if (i >= static_cast<int>(items.size()))
            break;
        const MenuItem& it = items[static_cast<size_t>(i)];
        float y = TOP + r * ROW_H;
        float cy = y + ROW_H / 2;
        if (it.separator) {
            ui.rect(x0 + 32, cy, w - 64, 1, ui.col(th.dim, 0.6f));
            continue;
        }
        if (it.header) {
            // The library's section header (small capitals over a thin
            // line), in the accent colour as DeckVideoPlayer's menus.
            ui.text(x0 + 32, y + ROW_H - 18, to_upper(it.label), 16, ui.col(th.accent), true, ALIGN_LEFT, w - 64);
            ui.rect(x0 + 32, y + ROW_H - 4, w - 64, 1, ui.col(th.dim, 0.5f));
            continue;
        }
        bool sel = i == selected;
        if (sel)
            ui.rect(x0 + PAD, y + 2, w - 2 * PAD, ROW_H - 4, ui.col(th.accent), 10);
        Color fg = sel ? ui.col(th.text) : it.marked ? ui.col(th.accent) : ui.col(th.text);
        Color sub = sel ? ui.col(th.text, 0.85f) : ui.col(th.subtext);

        // Value on the right, with ◀ ▶ around adjustable ones.
        float right = x0 + w - PAD - 18;
        float value_w = 0;
        std::string value = it.value ? it.value() : std::string();
        if (it.adjust) {
            float a = 6;
            Color arrow = sel ? ui.col(th.text) : ui.col(th.dim);
            ui.poly({right - a * 1.4f, cy - a, right, cy, right - a * 1.4f, cy + a}, arrow);
            float vw = ui.text(right - 14, cy, value, TEXT_SIZE, sub, false, ALIGN_RIGHT, w * 0.45f);
            float lx = right - 14 - vw - 12;
            ui.poly({lx, cy - a, lx - a * 1.4f, cy, lx, cy + a}, arrow);
            value_w = right - (lx - a * 1.4f) + 12;
        }
        float lx = x0 + 32 + it.indent * 22.0f;
        if (!it.adjust && !value.empty()) {
            // Label and value get their full width when both fit; otherwise
            // the label keeps up to 45% and the value the rest (as in
            // DeckVideoPlayer, its D32), both shortened with "…".
            const float gap = 16;
            float avail = right - lx - gap;
            float lw = ui.text_w(it.label, TEXT_SIZE, sel), vw = ui.text_w(value, TEXT_SIZE);
            float value_box = vw;
            if (lw + vw > avail) {
                float label_box = std::min(lw, std::max(avail * 0.45f, avail - vw));
                value_box = avail - label_box;
            }
            value_w = ui.text(right, cy, value, TEXT_SIZE, sub, false, ALIGN_RIGHT, std::max(40.0f, value_box)) + gap;
        }
        ui.text(lx, cy, it.label, TEXT_SIZE, fg, sel, ALIGN_LEFT, std::max(40.0f, right - value_w - lx));
    }

    // Scroll bar when the list is longer than the panel.
    int n = static_cast<int>(items.size());
    if (n > rows) {
        float track_h = rows * ROW_H;
        float bh = std::max(24.0f, track_h * rows / n);
        float by = TOP + (track_h - bh) * scroll_ / std::max(1, n - rows);
        ui.rect(ui.W() - 7, by, 3, bh, ui.col(th.dim), 1.5f);
    }

    std::vector<Hint> hints;
    bool adjustable = n && items[static_cast<size_t>(selected)].adjust;
    bool activatable = n && items[static_cast<size_t>(selected)].activate;
    if (adjustable)
        hints.push_back({Glyph::DPad, "Change"});
    else if (activatable)
        hints.push_back({Glyph::A, "Select"});
    if (n && items[static_cast<size_t>(selected)].secondary && !secondary_label.empty())
        hints.push_back({Glyph::X, secondary_label});
    hints.push_back({Glyph::B, "Back"});
    ui.hints_inline(hints, ui.W() - 30, ui.H() - 36);
}

bool Menu::tap(float x, float y, float canvas_w) {
    float x0 = canvas_w - drawn_width(canvas_w);
    if (x < x0)
        return false; // outside the panel: let the caller close the menu
    int r = static_cast<int>((y - row_y0_) / row_h_);
    if (y < row_y0_ || r < 0 || r >= rows_drawn_)
        return true;
    int i = scroll_ + r;
    if (i >= static_cast<int>(items.size()) || !selectable(i))
        return true;
    selected = i;
    MenuItem& it = items[static_cast<size_t>(i)];
    if (it.adjust) {
        // Left half of the value area lowers, right half raises.
        it.adjust(x < canvas_w - 90 ? -1 : 1);
    } else if (it.activate) {
        it.activate();
    }
    return true;
}

} // namespace dr
