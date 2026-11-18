#include "canvas.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dr {

static inline uint32_t premul(Color c, float coverage, uint32_t* r, uint32_t* g, uint32_t* b) {
    float a = std::clamp(c.a * coverage, 0.0f, 1.0f);
    uint32_t ai = static_cast<uint32_t>(a * 255.0f + 0.5f);
    *r = (c.r * ai + 127) / 255;
    *g = (c.g * ai + 127) / 255;
    *b = (c.b * ai + 127) / 255;
    return ai;
}

inline void Canvas::blend(uint32_t& dst, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb) {
    if (sa == 0)
        return;
    if (sa == 255) {
        dst = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
        return;
    }
    uint32_t inv = 255 - sa;
    uint32_t da = dst >> 24, dr_ = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    da = sa + (da * inv + 127) / 255;
    dr_ = sr + (dr_ * inv + 127) / 255;
    dg = sg + (dg * inv + 127) / 255;
    db = sb + (db * inv + 127) / 255;
    dst = (da << 24) | (dr_ << 16) | (dg << 8) | db;
}

void Canvas::resize(int w, int h) {
    w_ = std::max(w, 1);
    h_ = std::max(h, 1);
    px_.assign(static_cast<size_t>(w_) * h_, 0);
    reset_clip();
}

void Canvas::release() {
    std::vector<uint32_t>().swap(px_);
    w_ = h_ = 0;
    cx0_ = cy0_ = cx1_ = cy1_ = 0;
}

void Canvas::blend_span(uint32_t* row, int x0, int x1, uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    if (x0 >= x1 || a == 0)
        return;
    if (a == 255) {
        std::fill(row + x0, row + x1, 0xFF000000u | (r << 16) | (g << 8) | b);
        return;
    }
    for (int x = x0; x < x1; x++)
        blend(row[x], a, r, g, b);
}

void Canvas::clear(uint32_t argb) {
    std::fill(px_.begin(), px_.end(), argb);
}

void Canvas::copy_from(const Canvas& other) {
    if (other.w_ == w_ && other.h_ == h_)
        std::memcpy(px_.data(), other.px_.data(), px_.size() * sizeof(uint32_t));
}

void Canvas::set_clip(int x0, int y0, int x1, int y1) {
    cx0_ = std::clamp(x0, 0, w_);
    cy0_ = std::clamp(y0, 0, h_);
    cx1_ = std::clamp(x1, cx0_, w_);
    cy1_ = std::clamp(y1, cy0_, h_);
}

void Canvas::reset_clip() {
    cx0_ = cy0_ = 0;
    cx1_ = w_;
    cy1_ = h_;
}

template <typename CoverageFn>
void Canvas::fill_shape(float fx0, float fy0, float fx1, float fy1, Color c, CoverageFn cov) {
    int x0 = std::max(cx0_, static_cast<int>(std::floor(fx0)));
    int y0 = std::max(cy0_, static_cast<int>(std::floor(fy0)));
    int x1 = std::min(cx1_, static_cast<int>(std::ceil(fx1)));
    int y1 = std::min(cy1_, static_cast<int>(std::ceil(fy1)));
    if (x0 >= x1 || y0 >= y1 || c.a <= 0.0f)
        return;
    uint32_t fr, fg, fb;
    uint32_t fa = premul(c, 1.0f, &fr, &fg, &fb);
    for (int y = y0; y < y1; y++) {
        uint32_t* row = px_.data() + static_cast<size_t>(y) * w_;
        float py = y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float k = cov(x + 0.5f, py);
            if (k <= 0.0f)
                continue;
            if (k >= 1.0f) {
                blend(row[x], fa, fr, fg, fb);
            } else {
                uint32_t r, g, b;
                uint32_t a = premul(c, k, &r, &g, &b);
                blend(row[x], a, r, g, b);
            }
        }
    }
}

// Signed distance from p to a rounded box centred at (cx, cy).
static inline float sd_rrect(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = std::fabs(px - cx) - (hw - r);
    float qy = std::fabs(py - cy) - (hh - r);
    float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

void Canvas::fill_rrect(float x, float y, float w, float h, float radius, Color c) {
    if (w <= 0 || h <= 0 || c.a <= 0.0f)
        return;
    float hw = w / 2, hh = h / 2, cx = x + hw, cy = y + hh;
    float r = std::clamp(radius, 0.0f, std::min(hw, hh));
    int X0 = std::max(cx0_, static_cast<int>(std::floor(x)));
    int Y0 = std::max(cy0_, static_cast<int>(std::floor(y)));
    int X1 = std::min(cx1_, static_cast<int>(std::ceil(x + w)));
    int Y1 = std::min(cy1_, static_cast<int>(std::ceil(y + h)));
    if (X0 >= X1 || Y0 >= Y1)
        return;
    uint32_t fr, fg, fb;
    uint32_t fa = premul(c, 1.0f, &fr, &fg, &fb);
    // Pixels fully inside horizontally: the span between the side edges.
    int in0 = std::max(X0, static_cast<int>(std::ceil(x))), in1 = std::min(X1, static_cast<int>(std::floor(x + w)));
    for (int py = Y0; py < Y1; py++) {
        uint32_t* row = px_.data() + static_cast<size_t>(py) * w_;
        float top = py, bottom = py + 1.0f;
        // Rows clear of the corners and fully covered vertically: a solid
        // span plus at most one partial pixel at each side (D22).
        if (top >= y + r && bottom <= y + h - r && top >= y && bottom <= y + h) {
            blend_span(row, in0, in1, fa, fr, fg, fb);
            for (int px : {X0, X1 - 1}) {
                if (px >= in0 && px < in1)
                    continue;
                float k = std::min(px + 1.0f, x + w) - std::max(static_cast<float>(px), x);
                if (k > 0) {
                    uint32_t rr, gg, bb;
                    uint32_t a = premul(c, std::min(k, 1.0f), &rr, &gg, &bb);
                    blend(row[px], a, rr, gg, bb);
                }
                if (X1 - 1 == X0)
                    break;
            }
            continue;
        }
        // Corner and edge rows: anti-aliased by distance.
        float pyc = py + 0.5f;
        for (int px = X0; px < X1; px++) {
            float k = std::clamp(0.5f - sd_rrect(px + 0.5f, pyc, cx, cy, hw, hh, r), 0.0f, 1.0f);
            if (k <= 0.0f)
                continue;
            if (k >= 1.0f) {
                blend(row[px], fa, fr, fg, fb);
            } else {
                uint32_t rr, gg, bb;
                uint32_t a = premul(c, k, &rr, &gg, &bb);
                blend(row[px], a, rr, gg, bb);
            }
        }
    }
}

void Canvas::stroke_rrect(float x, float y, float w, float h, float radius, float t, Color c) {
    float hw = w / 2, hh = h / 2, cx = x + hw, cy = y + hh;
    float r = std::clamp(radius, 0.0f, std::min(hw, hh));
    float ri = std::max(r - t, 0.0f);
    fill_shape(x, y, x + w, y + h, c, [=](float px, float py) {
        float outer = std::clamp(0.5f - sd_rrect(px, py, cx, cy, hw, hh, r), 0.0f, 1.0f);
        float inner = std::clamp(0.5f - sd_rrect(px, py, cx, cy, hw - t, hh - t, ri), 0.0f, 1.0f);
        return outer - inner;
    });
}

void Canvas::fill_circle(float cx, float cy, float r, Color c) {
    fill_shape(cx - r, cy - r, cx + r, cy + r, c, [=](float px, float py) {
        float d = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r;
        return std::clamp(0.5f - d, 0.0f, 1.0f);
    });
}

void Canvas::stroke_line(float x0, float y0, float x1, float y1, float t, Color c) {
    float r = t / 2;
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = std::max(dx * dx + dy * dy, 1e-6f);
    fill_shape(std::min(x0, x1) - r, std::min(y0, y1) - r, std::max(x0, x1) + r, std::max(y0, y1) + r, c,
               [=](float px, float py) {
                   float k = std::clamp(((px - x0) * dx + (py - y0) * dy) / len2, 0.0f, 1.0f);
                   float qx = x0 + k * dx - px, qy = y0 + k * dy - py;
                   return std::clamp(0.5f - (std::sqrt(qx * qx + qy * qy) - r), 0.0f, 1.0f);
               });
}

void Canvas::fill_polygon(const float* xy, int n, Color c) {
    if (n < 3)
        return;
    float minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
    for (int i = 1; i < n; i++) {
        minx = std::min(minx, xy[2 * i]);
        maxx = std::max(maxx, xy[2 * i]);
        miny = std::min(miny, xy[2 * i + 1]);
        maxy = std::max(maxy, xy[2 * i + 1]);
    }
    // Orientation, so "inside" works for both windings.
    float area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
    }
    float sign = area >= 0 ? 1.0f : -1.0f;
    auto inside = [&](float px, float py) {
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            float ex = xy[2 * j] - xy[2 * i], ey = xy[2 * j + 1] - xy[2 * i + 1];
            if (sign * (ex * (py - xy[2 * i + 1]) - ey * (px - xy[2 * i])) < 0)
                return false;
        }
        return true;
    };
    fill_shape(minx, miny, maxx, maxy, c, [&](float px, float py) {
        int hits = 0;
        for (int sy = 0; sy < 4; sy++)
            for (int sx = 0; sx < 4; sx++)
                hits += inside(px - 0.5f + (sx + 0.5f) / 4, py - 0.5f + (sy + 0.5f) / 4);
        return hits / 16.0f;
    });
}

void Canvas::fill_background(uint32_t base, uint32_t glow) {
    Color b = Color::hex(base), g = Color::hex(glow);
    float diag = std::sqrt(static_cast<float>(w_) * w_ + static_cast<float>(h_) * h_);
    for (int y = 0; y < h_; y++) {
        uint32_t* row = px_.data() + static_cast<size_t>(y) * w_;
        for (int x = 0; x < w_; x++) {
            float d = std::sqrt(static_cast<float>(x) * x * 0.6f + static_cast<float>(y) * y * 1.4f) / (diag * 0.75f);
            float k = std::clamp(1.0f - d, 0.0f, 1.0f);
            k = k * k;
            uint32_t r = static_cast<uint32_t>(b.r + (g.r - b.r) * k + 0.5f);
            uint32_t gg = static_cast<uint32_t>(b.g + (g.g - b.g) * k + 0.5f);
            uint32_t bb = static_cast<uint32_t>(b.b + (g.b - b.b) * k + 0.5f);
            row[x] = 0xFF000000u | (r << 16) | (gg << 8) | bb;
        }
    }
}

void Canvas::blend_mask(int x, int y, const uint8_t* mask, int w, int h, int pitch, Color c) {
    int x0 = std::max(x, cx0_), y0 = std::max(y, cy0_);
    int x1 = std::min(x + w, cx1_), y1 = std::min(y + h, cy1_);
    if (x0 >= x1 || y0 >= y1)
        return;
    uint32_t base_a = static_cast<uint32_t>(std::clamp(c.a, 0.0f, 1.0f) * 255.0f + 0.5f);
    for (int yy = y0; yy < y1; yy++) {
        const uint8_t* m = mask + static_cast<size_t>(yy - y) * pitch + (x0 - x);
        uint32_t* row = px_.data() + static_cast<size_t>(yy) * w_;
        for (int xx = x0; xx < x1; xx++, m++) {
            if (!*m)
                continue;
            uint32_t a = (*m * base_a + 127) / 255;
            blend(row[xx], a, (c.r * a + 127) / 255, (c.g * a + 127) / 255, (c.b * a + 127) / 255);
        }
    }
}

void Canvas::draw_image(const Image& img, float x, float y, float w, float h, float opacity) {
    if (img.empty() || w <= 0 || h <= 0)
        return;
    int x0 = std::max(cx0_, static_cast<int>(std::floor(x)));
    int y0 = std::max(cy0_, static_cast<int>(std::floor(y)));
    int x1 = std::min(cx1_, static_cast<int>(std::ceil(x + w)));
    int y1 = std::min(cy1_, static_cast<int>(std::ceil(y + h)));
    uint32_t op = static_cast<uint32_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
    float sx = img.w / w, sy = img.h / h;
    for (int py = y0; py < y1; py++) {
        float fy = (py + 0.5f - y) * sy - 0.5f;
        int iy = static_cast<int>(std::floor(fy));
        float ty = fy - iy;
        int iy0 = std::clamp(iy, 0, img.h - 1), iy1 = std::clamp(iy + 1, 0, img.h - 1);
        uint32_t* row = px_.data() + static_cast<size_t>(py) * w_;
        for (int px = x0; px < x1; px++) {
            float fx = (px + 0.5f - x) * sx - 0.5f;
            int ix = static_cast<int>(std::floor(fx));
            float tx = fx - ix;
            int ix0 = std::clamp(ix, 0, img.w - 1), ix1 = std::clamp(ix + 1, 0, img.w - 1);
            uint32_t p00 = img.px[static_cast<size_t>(iy0) * img.w + ix0], p10 = img.px[static_cast<size_t>(iy0) * img.w + ix1];
            uint32_t p01 = img.px[static_cast<size_t>(iy1) * img.w + ix0], p11 = img.px[static_cast<size_t>(iy1) * img.w + ix1];
            uint32_t ch[4];
            for (int k = 0; k < 4; k++) {
                int s = 24 - 8 * k;
                float top = ((p00 >> s) & 0xFF) * (1 - tx) + ((p10 >> s) & 0xFF) * tx;
                float bot = ((p01 >> s) & 0xFF) * (1 - tx) + ((p11 >> s) & 0xFF) * tx;
                ch[k] = static_cast<uint32_t>(top * (1 - ty) + bot * ty + 0.5f);
                ch[k] = (ch[k] * op + 127) / 255;
            }
            // Edge pixels partially covered by the destination rectangle.
            float cov = 1.0f;
            if (px < x)
                cov *= 1.0f - (x - px);
            if (px + 1 > x + w)
                cov *= (x + w) - px;
            if (py < y)
                cov *= 1.0f - (y - py);
            if (py + 1 > y + h)
                cov *= (y + h) - py;
            if (cov < 1.0f) {
                uint32_t ci = static_cast<uint32_t>(std::clamp(cov, 0.0f, 1.0f) * 255.0f + 0.5f);
                for (uint32_t& v : ch)
                    v = (v * ci + 127) / 255;
            }
            blend(row[px], ch[0], ch[1], ch[2], ch[3]);
        }
    }
}

Image scale_image(const Image& src, int w, int h) {
    Image out;
    if (src.empty() || w <= 0 || h <= 0)
        return out;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * h);
    double sx = static_cast<double>(src.w) / w, sy = static_cast<double>(src.h) / h;
    for (int y = 0; y < h; y++) {
        double fy0 = y * sy, fy1 = (y + 1) * sy;
        for (int x = 0; x < w; x++) {
            double fx0 = x * sx, fx1 = (x + 1) * sx;
            double acc[4] = {0, 0, 0, 0}, wsum = 0;
            for (int yy = static_cast<int>(fy0); yy < static_cast<int>(std::ceil(fy1)) && yy < src.h; yy++) {
                double wy = std::min(fy1, yy + 1.0) - std::max(fy0, static_cast<double>(yy));
                if (wy <= 0)
                    continue;
                for (int xx = static_cast<int>(fx0); xx < static_cast<int>(std::ceil(fx1)) && xx < src.w; xx++) {
                    double wx = std::min(fx1, xx + 1.0) - std::max(fx0, static_cast<double>(xx));
                    if (wx <= 0)
                        continue;
                    uint32_t p = src.px[static_cast<size_t>(yy) * src.w + xx];
                    double wgt = wx * wy;
                    acc[0] += ((p >> 24) & 0xFF) * wgt;
                    acc[1] += ((p >> 16) & 0xFF) * wgt;
                    acc[2] += ((p >> 8) & 0xFF) * wgt;
                    acc[3] += (p & 0xFF) * wgt;
                    wsum += wgt;
                }
            }
            uint32_t v = 0;
            if (wsum > 0)
                for (int k = 0; k < 4; k++)
                    v |= static_cast<uint32_t>(std::clamp(acc[k] / wsum + 0.5, 0.0, 255.0)) << (24 - 8 * k);
            out.px[static_cast<size_t>(y) * w + x] = v;
        }
    }
    return out;
}

} // namespace dr
