// PNG files for cover thumbnails and test screenshots (libpng).
#pragma once

#include "canvas.h"

#include <string>

namespace dr {

// Premultiplied ARGB image <-> PNG (straight RGBA on disk).
bool save_png(const std::string& path, const Image& img);
bool load_png(const std::string& path, Image& img);
// Opaque XRGB pixels (rows of `pitch` bytes) to PNG.
bool save_png_xrgb(const std::string& path, const uint32_t* px, int w, int h, int pitch_bytes);

} // namespace dr
