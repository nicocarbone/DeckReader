#include "text.h"

#include "fsutil.h"
#include "log.h"
#include "util.h"

#include <cmath>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

namespace dr {

TextRenderer::~TextRenderer() {
    for (auto& [key, s] : sizes_) {
        if (s->hb)
            hb_font_destroy(s->hb);
        if (s->face)
            FT_Done_Face(s->face);
    }
    if (buf_)
        hb_buffer_destroy(buf_);
    if (lib_)
        FT_Done_FreeType(lib_);
}

bool TextRenderer::load(const std::string& regular_path, const std::string& bold_path) {
    if (!read_file(regular_path, data_[0]) || !read_file(bold_path, data_[1])) {
        LOG_E("interface font missing: %s / %s", regular_path.c_str(), bold_path.c_str());
        return false;
    }
    if (FT_Init_FreeType(&lib_) != 0)
        return false;
    buf_ = hb_buffer_create();
    return true;
}

TextRenderer::Sized* TextRenderer::sized(float px, bool bold) {
    int size = std::max(1, static_cast<int>(std::lround(px)));
    auto key = std::make_pair(size, bold ? 1 : 0);
    auto it = sizes_.find(key);
    if (it != sizes_.end())
        return it->second.get();
    // Window resizes (desktop) create new pixel sizes; keep the cache small.
    if (sizes_.size() >= 48) {
        for (auto& [k, sz] : sizes_) {
            if (sz->hb)
                hb_font_destroy(sz->hb);
            if (sz->face)
                FT_Done_Face(sz->face);
        }
        sizes_.clear();
    }
    auto s = std::make_unique<Sized>();
    const std::string& data = data_[bold ? 1 : 0];
    if (FT_New_Memory_Face(lib_, reinterpret_cast<const FT_Byte*>(data.data()), static_cast<FT_Long>(data.size()), 0,
                           &s->face) != 0) {
        LOG_E("cannot load the interface font");
        return nullptr;
    }
    FT_Set_Pixel_Sizes(s->face, 0, static_cast<FT_UInt>(size));
    s->hb = hb_ft_font_create_referenced(s->face);
    hb_ft_font_set_load_flags(s->hb, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT);
    // Cap height from the "H" outline.
    FT_UInt h = FT_Get_Char_Index(s->face, 'H');
    if (h && FT_Load_Glyph(s->face, h, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) == 0)
        s->cap_height = s->face->glyph->metrics.horiBearingY / 64.0f;
    else
        s->cap_height = size * 0.72f;
    Sized* raw = s.get();
    sizes_[key] = std::move(s);
    return raw;
}

const TextRenderer::Glyph& TextRenderer::glyph(Sized* s, uint32_t gid) {
    auto it = s->glyphs.find(gid);
    if (it != s->glyphs.end())
        return it->second;
    Glyph g;
    if (FT_Load_Glyph(s->face, gid, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) == 0 &&
        FT_Render_Glyph(s->face->glyph, FT_RENDER_MODE_NORMAL) == 0) {
        const FT_Bitmap& bm = s->face->glyph->bitmap;
        g.w = static_cast<int>(bm.width);
        g.h = static_cast<int>(bm.rows);
        g.left = s->face->glyph->bitmap_left;
        g.top = s->face->glyph->bitmap_top;
        g.bmp.resize(static_cast<size_t>(g.w) * g.h);
        // A negative pitch means rows are stored bottom-up.
        for (int y = 0; y < g.h; y++) {
            const unsigned char* row = bm.pitch >= 0 ? bm.buffer + y * bm.pitch
                                                     : bm.buffer + (g.h - 1 - y) * -bm.pitch;
            for (int x = 0; x < g.w; x++)
                g.bmp[static_cast<size_t>(y) * g.w + x] = row[x];
        }
    }
    return s->glyphs.emplace(gid, std::move(g)).first->second;
}

float TextRenderer::shape(Sized* s, const std::string& text, std::vector<uint32_t>* gids, std::vector<float>* xs,
                          std::vector<float>* ys) {
    hb_buffer_clear_contents(buf_);
    hb_buffer_add_utf8(buf_, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(buf_);
    hb_shape(s->hb, buf_, nullptr, 0);
    unsigned int n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf_, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf_, &n);
    float x = 0;
    for (unsigned int i = 0; i < n; i++) {
        if (gids) {
            gids->push_back(info[i].codepoint);
            xs->push_back(x + pos[i].x_offset / 64.0f);
            ys->push_back(-pos[i].y_offset / 64.0f);
        }
        x += pos[i].x_advance / 64.0f;
    }
    return x;
}

float TextRenderer::measure(const std::string& s, float px, bool bold) {
    Sized* sz = sized(px, bold);
    if (!sz || s.empty())
        return 0;
    return shape(sz, s, nullptr, nullptr, nullptr);
}

float TextRenderer::draw(Canvas& c, float x, float baseline, const std::string& s, float px, bool bold, Color color) {
    Sized* sz = sized(px, bold);
    if (!sz || s.empty())
        return 0;
    std::vector<uint32_t> gids;
    std::vector<float> xs, ys;
    float width = shape(sz, s, &gids, &xs, &ys);
    int by = static_cast<int>(std::lround(baseline));
    for (size_t i = 0; i < gids.size(); i++) {
        const Glyph& g = glyph(sz, gids[i]);
        if (g.bmp.empty())
            continue;
        int gx = static_cast<int>(std::lround(x + xs[i])) + g.left;
        int gy = by + static_cast<int>(std::lround(ys[i])) - g.top;
        c.blend_mask(gx, gy, g.bmp.data(), g.w, g.h, g.w, color);
    }
    return width;
}

std::string TextRenderer::ellipsize(const std::string& s, float px, bool bold, float max_w) {
    if (max_w <= 0 || measure(s, px, bold) <= max_w)
        return s;
    static const std::string ell = "\xE2\x80\xA6"; // …
    if (measure(ell, px, bold) > max_w)
        return {}; // not even "…" fits
    // Binary search over code point boundaries for the longest prefix.
    // cuts[k] = byte length of the first k code points.
    std::vector<size_t> cuts;
    for (size_t i = 0; i < s.size(); i++)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
            cuts.push_back(i);
    cuts.push_back(s.size());
    size_t lo = 0, hi = cuts.size() - 1;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (measure(trim(s.substr(0, cuts[mid])) + ell, px, bold) <= max_w)
            lo = mid;
        else
            hi = mid - 1;
    }
    return trim(s.substr(0, cuts[lo])) + ell;
}

std::vector<std::string> TextRenderer::wrap(const std::string& s, float px, bool bold, float max_w, size_t max_lines) {
    std::vector<std::string> lines;
    std::string cur;
    std::vector<std::string> words = split(s, ' ');
    for (size_t i = 0; i < words.size(); i++) {
        const std::string& w = words[i];
        if (w.empty())
            continue;
        std::string cand = cur.empty() ? w : cur + " " + w;
        if (measure(cand, px, bold) <= max_w || cur.empty()) {
            cur = cand;
            continue;
        }
        lines.push_back(cur);
        cur = w;
        if (lines.size() + 1 == max_lines) {
            // Last line: the rest of the text, ellipsized.
            for (size_t k = i + 1; k < words.size(); k++)
                if (!words[k].empty())
                    cur += " " + words[k];
            break;
        }
    }
    if (!cur.empty())
        lines.push_back(cur);
    for (std::string& l : lines)
        l = ellipsize(l, px, bold, max_w);
    if (lines.size() > max_lines)
        lines.resize(max_lines);
    return lines;
}

float TextRenderer::cap_height(float px, bool bold) {
    Sized* sz = sized(px, bold);
    return sz ? sz->cap_height : px * 0.72f;
}

} // namespace dr
