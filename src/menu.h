// Side drawer menus (DeckVideoPlayer's dvp/menu.lua): a panel on the right
// with rows that can be activated (A), adjusted (◀ ▶) or have a secondary
// action (X). Menus stack: B closes the top one.
#pragma once

#include "input.h"
#include "ui.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dr {

struct MenuItem {
    std::string label;
    std::function<std::string()> value;  // text on the right (optional)
    std::function<void(int)> adjust;      // ◀ -1 / ▶ +1 (optional)
    std::function<void()> activate;       // A (optional)
    std::function<void()> secondary;      // X (optional)
    int indent = 0;                       // TOC levels
    bool marked = false;                  // e.g. the current chapter
};

class Menu {
public:
    std::string title;
    std::vector<MenuItem> items;
    int selected = 0;
    float width = 500;               // panel width in units
    std::string secondary_label;     // hint for X, when items have one
    std::string empty_text = "Nothing here yet.";
    bool closed = false;             // set by an item's action to close it

    // Returns false when the menu wants to close (B).
    bool handle(const InputEvent& e);
    void draw(Ui& ui);
    // Tap at (x, y) in units: selects and activates a row. True if handled.
    bool tap(float x, float y, float canvas_w);
    // Width actually drawn: `width`, but never more than 85% of the canvas
    // (large ui_scale values make the canvas narrow).
    float drawn_width(float canvas_w) const;
    void select_marked(); // put the cursor on the marked row (current chapter)

private:
    int visible_rows(float canvas_h) const;
    void keep_visible(int rows);
    int scroll_ = 0;
    float row_y0_ = 0, row_h_ = 52;
    int rows_drawn_ = 0;
};

} // namespace dr
