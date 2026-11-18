#include "image_io.h"

#include "fsutil.h"

#include <cstdio>
#include <png.h>
#include <vector>

namespace dr {

static bool write_rgba(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    // Written next to the target, then renamed (a cache file is never half written).
    std::string tmp = path + ".part";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(f);
        std::remove(tmp.c_str());
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, static_cast<png_uint_32>(w), static_cast<png_uint_32>(h), 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, 6);
    png_write_info(png, info);
    for (int y = 0; y < h; y++)
        png_write_row(png, const_cast<png_bytep>(rgba.data() + static_cast<size_t>(y) * w * 4));
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    bool ok = std::fclose(f) == 0;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool save_png(const std::string& path, const Image& img) {
    if (img.empty())
        return false;
    std::vector<uint8_t> rgba(static_cast<size_t>(img.w) * img.h * 4);
    for (size_t i = 0; i < img.px.size(); i++) {
        uint32_t p = img.px[i];
        uint32_t a = p >> 24;
        uint8_t* o = &rgba[i * 4];
        if (a == 0) {
            o[0] = o[1] = o[2] = o[3] = 0;
            continue;
        }
        o[0] = static_cast<uint8_t>(std::min<uint32_t>(255, (((p >> 16) & 0xFF) * 255 + a / 2) / a));
        o[1] = static_cast<uint8_t>(std::min<uint32_t>(255, (((p >> 8) & 0xFF) * 255 + a / 2) / a));
        o[2] = static_cast<uint8_t>(std::min<uint32_t>(255, ((p & 0xFF) * 255 + a / 2) / a));
        o[3] = static_cast<uint8_t>(a);
    }
    return write_rgba(path, rgba, img.w, img.h);
}

bool save_png_xrgb(const std::string& path, const uint32_t* px, int w, int h, int pitch_bytes) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; y++) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(px) + static_cast<size_t>(y) * pitch_bytes);
        for (int x = 0; x < w; x++) {
            uint8_t* o = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            o[0] = static_cast<uint8_t>(row[x] >> 16);
            o[1] = static_cast<uint8_t>(row[x] >> 8);
            o[2] = static_cast<uint8_t>(row[x]);
            o[3] = 255;
        }
    }
    return write_rgba(path, rgba, w, h);
}

bool load_png(const std::string& path, Image& img) {
    png_image pi{};
    pi.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&pi, path.c_str()))
        return false;
    pi.format = PNG_FORMAT_RGBA;
    if (pi.width > 4096 || pi.height > 4096) {
        png_image_free(&pi);
        return false;
    }
    std::vector<uint8_t> rgba(PNG_IMAGE_SIZE(pi));
    if (!png_image_finish_read(&pi, nullptr, rgba.data(), 0, nullptr)) {
        png_image_free(&pi);
        return false;
    }
    img.w = static_cast<int>(pi.width);
    img.h = static_cast<int>(pi.height);
    img.px.resize(static_cast<size_t>(img.w) * img.h);
    for (size_t i = 0; i < img.px.size(); i++) {
        const uint8_t* p = &rgba[i * 4];
        uint32_t a = p[3];
        img.px[i] = (a << 24) | (((p[0] * a + 127) / 255) << 16) | (((p[1] * a + 127) / 255) << 8) | ((p[2] * a + 127) / 255);
    }
    return true;
}

} // namespace dr
