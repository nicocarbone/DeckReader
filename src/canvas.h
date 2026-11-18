// A CPU drawing surface for the interface layer (library, HUD, menus).
//
// Pixels are premultiplied ARGB (0xAARRGGBB), uploaded as one texture and
// blended over the page by the GPU (display.h). Shapes are anti-aliased with
// signed distance functions, so rounded panels and pills look like
// DeckVideoPlayer's libass-drawn ones at any resolution.
#pragma once

#include <cstdint>
#include <vector>

namespace dr {

struct Color {
    uint8_t r = 0, g = 0, b = 0;
    float a = 1.0f;
    static Color hex(uint32_t rgb, float alpha = 1.0f) {
        return Color{static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb), alpha};
    }
    Color with_alpha(float alpha) const { return Color{r, g, b, alpha}; }
    Color mul_alpha(float m) const { return Color{r, g, b, a * m}; }
};

// Premultiplied ARGB image.
struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    bool empty() const { return w <= 0 || h <= 0; }
};

// Area-averaging resize (good for the large downscales of cover images).
Image scale_image(const Image& src, int w, int h);

class Canvas {
public:
    void resize(int w, int h);
    // Frees the pixels (width() and height() become 0) until resize().
    void release();
    int width() const { return w_; }
    int height() const { return h_; }
    uint32_t* data() { return px_.data(); }
    const uint32_t* data() const { return px_.data(); }

    void clear(uint32_t argb = 0);
    void copy_from(const Canvas& other); // same size required

    // Coordinates are pixels (floats: edges are anti-aliased).
    void fill_rect(float x, float y, float w, float h, Color c) { fill_rrect(x, y, w, h, 0, c); }
    void fill_rrect(float x, float y, float w, float h, float radius, Color c);
    void stroke_rrect(float x, float y, float w, float h, float radius, float thickness, Color c);
    void fill_circle(float cx, float cy, float r, Color c);
    // Convex polygon, xy = {x0, y0, x1, y1, ...}.
    void fill_polygon(const float* xy, int n, Color c);
    // A thick line segment with round caps (check marks, chevrons).
    void stroke_line(float x0, float y0, float x1, float y1, float thickness, Color c);
    // Radial-ish background like DeckVideoPlayer's library: lighter at the
    // top left, fading to `base`.
    void fill_background(uint32_t base, uint32_t glow);

    // 8-bit coverage mask (glyphs) tinted with c.
    void blend_mask(int x, int y, const uint8_t* mask, int w, int h, int pitch, Color c);
    // Image scaled to w x h (bilinear; use scale_image first for big shrinks).
    void draw_image(const Image& img, float x, float y, float w, float h, float opacity = 1.0f);

    void set_clip(int x0, int y0, int x1, int y1);
    void reset_clip();

private:
    template <typename CoverageFn> void fill_shape(float x0, float y0, float x1, float y1, Color c, CoverageFn cov);
    inline void blend(uint32_t& dst, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb);
    void blend_span(uint32_t* row, int x0, int x1, uint32_t a, uint32_t r, uint32_t g, uint32_t b);

    int w_ = 0, h_ = 0;
    int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0; // clip
    std::vector<uint32_t> px_;
};

} // namespace dr
