// Interface text (not book text): Inter shaped with HarfBuzz and rasterised
// with FreeType in grayscale, drawn onto a Canvas. Book pages are rendered by
// crengine (book.h); both use the same FreeType, so they look alike.
#pragma once

#include "canvas.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;
typedef struct hb_font_t hb_font_t;
typedef struct hb_buffer_t hb_buffer_t;

namespace dr {

class TextRenderer {
public:
    TextRenderer() = default;
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;
    ~TextRenderer();

    // Loads the regular and bold font files. False if either is missing.
    bool load(const std::string& regular_path, const std::string& bold_path);

    // Width in pixels of s at a pixel size.
    float measure(const std::string& s, float px, bool bold);
    // Draws s with its baseline at y; returns the advance width.
    float draw(Canvas& c, float x, float baseline, const std::string& s, float px, bool bold, Color color);
    // Shortens s with "…" so it fits max_w (unchanged if it fits).
    std::string ellipsize(const std::string& s, float px, bool bold, float max_w);
    // Greedy word wrap; the last allowed line is ellipsized.
    std::vector<std::string> wrap(const std::string& s, float px, bool bold, float max_w, size_t max_lines);
    // Distance from baseline to the top of capital letters (for centring).
    float cap_height(float px, bool bold);

private:
    struct Glyph {
        int w = 0, h = 0, left = 0, top = 0;
        std::vector<uint8_t> bmp;
    };
    struct Sized {
        FT_Face face = nullptr;
        hb_font_t* hb = nullptr;
        float cap_height = 0;
        std::unordered_map<uint32_t, Glyph> glyphs;
    };
    Sized* sized(float px, bool bold);
    const Glyph& glyph(Sized* s, uint32_t gid);
    // Shapes s: fills glyph ids and pen positions (pixels); returns width.
    float shape(Sized* s, const std::string& text, std::vector<uint32_t>* gids, std::vector<float>* xs,
                std::vector<float>* ys);

    FT_Library lib_ = nullptr;
    std::string data_[2]; // font files (FreeType reads them in place)
    std::map<std::pair<int, int>, std::unique_ptr<Sized>> sizes_;
    hb_buffer_t* buf_ = nullptr;
};

} // namespace dr
