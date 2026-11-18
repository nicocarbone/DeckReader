// The window and the GPU side: page textures, the interface overlay, present.
//
// A frame is at most two textured quads: the page (opaque) and the overlay
// (premultiplied alpha), so presenting a pre-rendered page costs next to
// nothing and page turns stay well under a frame (requirements 7.1).
// Frames are only drawn when something changed; nothing renders while a
// page is just being read (D1).
#pragma once

#include "canvas.h"

#include <SDL3/SDL.h>

#include <string>

namespace dr {

struct DisplayOptions {
    bool fullscreen = true;
    int width = 1280, height = 800; // window size when not fullscreen
    bool software = false;          // SDL software renderer (headless tests)
};

class Display {
public:
    ~Display();
    bool init(const DisplayOptions& opt);
    void shutdown();

    int width() const { return w_; }   // pixels
    int height() const { return h_; }
    // Re-reads the window's pixel size; true if it changed.
    bool update_size();

    SDL_Texture* create_page_texture();
    void destroy_texture(SDL_Texture* t);
    bool upload(SDL_Texture* t, const uint32_t* px, int pitch_bytes);
    void upload_overlay(const Canvas& c);

    // The library's backdrop: a small gradient the GPU stretches to the
    // screen (a full-size copy would cost 33 MB at 4K).
    void set_backdrop(const Canvas& small);

    // Clears to bg, draws the backdrop or the page (may be null) and, if
    // requested, the overlay. A non-empty screenshot path saves the frame as
    // PNG before presenting.
    void present(SDL_Texture* page, bool backdrop, bool overlay, uint32_t bg, const std::string& screenshot = {});

    SDL_Window* window() const { return win_; }

private:
    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* overlay_ = nullptr;
    SDL_Texture* backdrop_ = nullptr;
    int w_ = 0, h_ = 0;
};

} // namespace dr
