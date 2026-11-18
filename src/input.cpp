#include "input.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dr {

namespace {
// Ids for held inputs: gamepad buttons use their SDL value; these are above.
constexpr int ID_STICK_UP = 1000, ID_STICK_DOWN = 1001, ID_STICK_LEFT = 1002, ID_STICK_RIGHT = 1003;
constexpr int ID_L2 = 1010, ID_R2 = 1011;
constexpr float STICK_PRESS = 0.55f, STICK_RELEASE = 0.35f;
constexpr float TRIGGER_PRESS = 0.6f, TRIGGER_RELEASE = 0.3f;
constexpr float SCRUB_DEADZONE = 0.3f;

struct NamedAction {
    const char* name;
    Action action;
};
const NamedAction NAMES[] = {
    {"up", Action::Up},       {"down", Action::Down}, {"left", Action::Left}, {"right", Action::Right},
    {"confirm", Action::Confirm}, {"a", Action::Confirm}, {"back", Action::Back}, {"b", Action::Back},
    {"x", Action::X},         {"y", Action::Y},       {"menu", Action::Menu}, {"view", Action::View},
    {"l1", Action::L1},       {"r1", Action::R1},     {"l2", Action::L2},     {"r2", Action::R2},
    {"quit", Action::Quit},
};
} // namespace

const char* action_name(Action a) {
    for (const NamedAction& n : NAMES)
        if (n.action == a)
            return n.name;
    return "none";
}

Action action_from_name(const char* name) {
    for (const NamedAction& n : NAMES)
        if (std::strcmp(n.name, name) == 0)
            return n.action;
    return Action::None;
}

bool Input::repeatable(Action a) {
    switch (a) {
    case Action::Up:
    case Action::Down:
    case Action::Left:
    case Action::Right:
    case Action::L1:
    case Action::R1:
        return true;
    default:
        return false; // holding R2 must not skip several chapters
    }
}

Action Input::key_action(SDL_Keycode key, SDL_Keymod mod) {
    bool ctrl = (mod & SDL_KMOD_CTRL) != 0, shift = (mod & SDL_KMOD_SHIFT) != 0;
    switch (key) {
    case SDLK_UP: return Action::Up;
    case SDLK_DOWN: return Action::Down;
    case SDLK_LEFT: return Action::Left;
    case SDLK_RIGHT: return Action::Right;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE: return Action::Confirm;
    case SDLK_BACKSPACE:
    case SDLK_ESCAPE: return Action::Back;
    case SDLK_X:
    case SDLK_S: return Action::X;
    case SDLK_Y: return Action::Y;
    case SDLK_A: return Action::Y; // as DeckVideoPlayer (s/a sit under x/y)
    case SDLK_M:
    case SDLK_TAB: return Action::Menu;
    case SDLK_I:
    case SDLK_V: return Action::View;
    case SDLK_PAGEUP: return Action::L1;
    case SDLK_PAGEDOWN: return Action::R1;
    case SDLK_HOME: return Action::L2;
    case SDLK_END: return Action::R2;
    case SDLK_Q: return (ctrl || shift) ? Action::Quit : Action::None;
    default: return Action::None;
    }
}

Action Input::button_action(SDL_GamepadButton b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return Action::Confirm;
    case SDL_GAMEPAD_BUTTON_EAST: return Action::Back;
    case SDL_GAMEPAD_BUTTON_WEST: return Action::X;
    case SDL_GAMEPAD_BUTTON_NORTH: return Action::Y;
    case SDL_GAMEPAD_BUTTON_BACK: return Action::View;
    case SDL_GAMEPAD_BUTTON_START: return Action::Menu;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Action::L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Action::R1;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return Action::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Action::Down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Action::Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Action::Right;
    default: return Action::None; // Guide/Steam and "..." stay with Steam
    }
}

static uint64_t held_id(uint32_t pad, int what) {
    return (static_cast<uint64_t>(pad) << 16) | static_cast<uint64_t>(what & 0xFFFF);
}

void Input::press(uint64_t id, Action a, Source src, uint64_t now, std::vector<InputEvent>& out) {
    release(id);
    out.push_back({a, src, false});
    held_.push_back({id, a, src, repeatable(a) ? now + REPEAT_DELAY_MS : 0});
}

void Input::release(uint64_t id) {
    for (size_t i = 0; i < held_.size(); i++)
        if (held_[i].id == id) {
            held_.erase(held_.begin() + static_cast<long>(i));
            return;
        }
}

void Input::handle(const SDL_Event& e, uint64_t now, std::vector<InputEvent>& out) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        Action a = key_action(e.key.key, e.key.mod);
        if (a == Action::None)
            return;
        if (e.key.repeat && !repeatable(a))
            return;
        out.push_back({a, Source::Keyboard, e.key.repeat});
        return;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        Action a = button_action(static_cast<SDL_GamepadButton>(e.gbutton.button));
        if (a != Action::None)
            press(held_id(e.gbutton.which, e.gbutton.button), a, Source::Gamepad, now, out);
        return;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        release(held_id(e.gbutton.which, e.gbutton.button));
        return;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        Pad& p = pads_[e.gaxis.which];
        float v = e.gaxis.value / 32767.0f;
        v = std::fmax(-1.0f, std::fmin(1.0f, v));
        switch (e.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            p.lx = v;
            stick_update(e.gaxis.which, p, now, out);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            p.ly = v;
            stick_update(e.gaxis.which, p, now, out);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: {
            bool left = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
            float& t = left ? p.lt : p.rt;
            float prev = t;
            t = v;
            uint64_t id = held_id(e.gaxis.which, left ? ID_L2 : ID_R2);
            if (prev < TRIGGER_PRESS && t >= TRIGGER_PRESS)
                press(id, left ? Action::L2 : Action::R2, Source::Gamepad, now, out);
            else if (prev > TRIGGER_RELEASE && t <= TRIGGER_RELEASE)
                release(id);
            break;
        }
        default:
            break;
        }
        return;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        // Forget only this controller's buttons and axes.
        const uint32_t pad = e.gdevice.which;
        held_.erase(std::remove_if(held_.begin(), held_.end(),
                                   [&](const Held& h) { return (h.id >> 16) == pad; }),
                    held_.end());
        pads_.erase(pad);
        return;
    }
    default:
        return;
    }
}

void Input::stick_update(uint32_t pad, Pad& p, uint64_t now, std::vector<InputEvent>& out) {
    // The dominant axis decides the direction, with hysteresis.
    float ax = scrub_ ? 0.0f : p.lx; // scrubbing takes the horizontal axis
    float mag_x = std::fabs(ax), mag_y = std::fabs(p.ly);
    int dir = 0;
    if (p.stick_dir != 0) {
        float m = (p.stick_dir == ID_STICK_LEFT || p.stick_dir == ID_STICK_RIGHT) ? mag_x : mag_y;
        if (m > STICK_RELEASE)
            dir = p.stick_dir;
    }
    if (dir == 0 && std::fmax(mag_x, mag_y) >= STICK_PRESS) {
        if (mag_x > mag_y)
            dir = ax < 0 ? ID_STICK_LEFT : ID_STICK_RIGHT;
        else
            dir = p.ly < 0 ? ID_STICK_UP : ID_STICK_DOWN;
    }
    if (dir == p.stick_dir)
        return;
    if (p.stick_dir)
        release(held_id(pad, p.stick_dir));
    p.stick_dir = dir;
    if (dir) {
        Action a = dir == ID_STICK_UP ? Action::Up : dir == ID_STICK_DOWN ? Action::Down
                 : dir == ID_STICK_LEFT ? Action::Left : Action::Right;
        press(held_id(pad, dir), a, Source::Stick, now, out);
    }
}

void Input::tick(uint64_t now, std::vector<InputEvent>& out) {
    for (Held& h : held_) {
        if (h.next && now >= h.next) {
            out.push_back({h.action, h.source, true});
            h.next = now + REPEAT_INTERVAL_MS;
        }
    }
}

int64_t Input::next_due(uint64_t now) const {
    int64_t best = -1;
    for (const Held& h : held_) {
        if (!h.next)
            continue;
        int64_t d = h.next > now ? static_cast<int64_t>(h.next - now) : 0;
        if (best < 0 || d < best)
            best = d;
    }
    return best;
}

void Input::set_stick_scrub(bool on) {
    if (scrub_ == on)
        return;
    scrub_ = on;
    if (on)
        for (auto& [pad, p] : pads_)
            if (p.stick_dir == ID_STICK_LEFT || p.stick_dir == ID_STICK_RIGHT) {
                release(held_id(pad, p.stick_dir));
                p.stick_dir = 0;
            }
}

float Input::scrub_x() const {
    if (!scrub_)
        return 0.0f;
    // The most deflected stick among the controllers.
    float lx = 0;
    for (const auto& [pad, p] : pads_)
        if (std::fabs(p.lx) > std::fabs(lx))
            lx = p.lx;
    if (std::fabs(lx) < SCRUB_DEADZONE)
        return 0.0f;
    float s = (std::fabs(lx) - SCRUB_DEADZONE) / (1.0f - SCRUB_DEADZONE);
    return lx < 0 ? -s : s;
}

} // namespace dr
