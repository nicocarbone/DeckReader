// Controller and keyboard -> abstract actions, the same set and the same
// keyboard keys as DeckVideoPlayer's dvp/input.lua, so the button meaning
// carries over between the two apps (DeckVideoPlayer D10, DeckReader D8).
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <vector>

namespace dr {

enum class Action {
    None,
    Up, Down, Left, Right,
    Confirm, // A
    Back,    // B
    X, Y,
    Menu,    // ☰ (Start)
    View,    // ⧉ (Select)
    L1, R1, L2, R2,
    Quit,
};

enum class Source { Keyboard, Gamepad, Stick, Script };

struct InputEvent {
    Action action = Action::None;
    Source source = Source::Keyboard;
    bool repeat = false;
};

const char* action_name(Action a);
Action action_from_name(const char* name); // "right", "confirm", ... (test scripts)

class Input {
public:
    // Hold-to-repeat timing for D-pad, stick and L1/R1 (not the triggers).
    static constexpr uint64_t REPEAT_DELAY_MS = 320;
    static constexpr uint64_t REPEAT_INTERVAL_MS = 70;

    // Turns one SDL event into zero or more actions.
    void handle(const SDL_Event& e, uint64_t now, std::vector<InputEvent>& out);
    // Emits due repeats; call once per loop iteration.
    void tick(uint64_t now, std::vector<InputEvent>& out);
    // Milliseconds until the next repeat (-1: nothing held).
    int64_t next_due(uint64_t now) const;

    // In the reader, the left stick's horizontal axis scrubs through the
    // book instead of acting as D-pad left/right (requirements 5.1).
    void set_stick_scrub(bool on);
    // Horizontal deflection beyond the dead zone, -1..1 (0 when centred).
    float scrub_x() const;

    // Test scripts: move the left stick's horizontal axis (-1..1).
    void inject_stick_x(float x, uint64_t now, std::vector<InputEvent>& out) {
        Pad& p = pads_[0];
        p.lx = x;
        stick_update(0, p, now, out);
    }

    static Action key_action(SDL_Keycode key, SDL_Keymod mod);
    static Action button_action(SDL_GamepadButton b);
    static bool repeatable(Action a);

private:
    // Each controller has its own held buttons and axes, so a second pad
    // (docked) can't release or overwrite the first one's (D22).
    struct Held {
        uint64_t id;   // controller << 16 | button / stick direction / trigger
        Action action;
        Source source;
        uint64_t next; // next repeat time (0 = no repeat)
    };
    struct Pad {
        float lx = 0, ly = 0; // left stick, -1..1
        float lt = 0, rt = 0; // triggers, 0..1
        int stick_dir = 0;    // currently pressed stick direction (0 none)
    };
    void press(uint64_t id, Action a, Source src, uint64_t now, std::vector<InputEvent>& out);
    void release(uint64_t id);
    void stick_update(uint32_t pad, Pad& p, uint64_t now, std::vector<InputEvent>& out);

    std::vector<Held> held_;
    std::map<uint32_t, Pad> pads_;
    bool scrub_ = false;
};

} // namespace dr
