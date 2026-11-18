#include "display.h"

#include "image_io.h"
#include "log.h"

namespace dr {

Display::~Display() {
    shutdown();
}

bool Display::init(const DisplayOptions& opt) {
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (opt.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    win_ = SDL_CreateWindow("DeckReader", opt.width, opt.height, flags);
    if (!win_) {
        LOG_E("cannot create the window: %s", SDL_GetError());
        return false;
    }
    ren_ = SDL_CreateRenderer(win_, opt.software ? SDL_SOFTWARE_RENDERER : nullptr);
    if (!ren_) {
        LOG_E("cannot create a renderer: %s", SDL_GetError());
        return false;
    }
    // VSync: a page turn is presented on the next refresh, never torn.
    SDL_SetRenderVSync(ren_, 1);
    LOG_I("renderer: %s, video driver: %s", SDL_GetRendererName(ren_), SDL_GetCurrentVideoDriver());
    update_size();
    return true;
}

void Display::shutdown() {
    if (overlay_)
        SDL_DestroyTexture(overlay_);
    overlay_ = nullptr;
    if (backdrop_)
        SDL_DestroyTexture(backdrop_);
    backdrop_ = nullptr;
    if (ren_)
        SDL_DestroyRenderer(ren_);
    ren_ = nullptr;
    if (win_)
        SDL_DestroyWindow(win_);
    win_ = nullptr;
}

bool Display::update_size() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(win_, &w, &h);
    if (w <= 0 || h <= 0 || (w == w_ && h == h_))
        return false;
    w_ = w;
    h_ = h;
    if (overlay_)
        SDL_DestroyTexture(overlay_);
    // Static textures: updated whole with SDL_UpdateTexture, so SDL keeps no
    // CPU-side copy (streaming textures carry a lock buffer each).
    overlay_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w_, h_);
    if (overlay_)
        SDL_SetTextureBlendMode(overlay_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    LOG_I("screen %dx%d", w_, h_);
    return true;
}

SDL_Texture* Display::create_page_texture() {
    SDL_Texture* t = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, w_, h_);
    if (t)
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    else
        LOG_E("cannot create a page texture: %s", SDL_GetError());
    return t;
}

void Display::destroy_texture(SDL_Texture* t) {
    if (t)
        SDL_DestroyTexture(t);
}

bool Display::upload(SDL_Texture* t, const uint32_t* px, int pitch_bytes) {
    return t && SDL_UpdateTexture(t, nullptr, px, pitch_bytes);
}

void Display::upload_overlay(const Canvas& c) {
    if (overlay_ && c.width() == w_ && c.height() == h_)
        SDL_UpdateTexture(overlay_, nullptr, c.data(), c.width() * 4);
}

void Display::set_backdrop(const Canvas& small) {
    if (backdrop_)
        SDL_DestroyTexture(backdrop_);
    backdrop_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, small.width(), small.height());
    if (!backdrop_)
        return;
    SDL_SetTextureScaleMode(backdrop_, SDL_SCALEMODE_LINEAR);
    SDL_UpdateTexture(backdrop_, nullptr, small.data(), small.width() * 4);
}

void Display::present(SDL_Texture* page, bool backdrop, bool overlay, uint32_t bg, const std::string& screenshot) {
    SDL_SetRenderDrawColor(ren_, static_cast<Uint8>(bg >> 16), static_cast<Uint8>(bg >> 8), static_cast<Uint8>(bg), 255);
    SDL_RenderClear(ren_);
    if (backdrop && backdrop_)
        SDL_RenderTexture(ren_, backdrop_, nullptr, nullptr);
    if (page)
        SDL_RenderTexture(ren_, page, nullptr, nullptr);
    if (overlay && overlay_)
        SDL_RenderTexture(ren_, overlay_, nullptr, nullptr);
    if (!screenshot.empty()) {
        SDL_Surface* s = SDL_RenderReadPixels(ren_, nullptr);
        SDL_Surface* conv = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_XRGB8888) : nullptr;
        if (conv && save_png_xrgb(screenshot, static_cast<const uint32_t*>(conv->pixels), conv->w, conv->h, conv->pitch))
            LOG_I("screenshot saved: %s", screenshot.c_str());
        else
            LOG_E("screenshot failed: %s", SDL_GetError());
        SDL_DestroySurface(conv);
        SDL_DestroySurface(s);
    }
    SDL_RenderPresent(ren_);
}

} // namespace dr
