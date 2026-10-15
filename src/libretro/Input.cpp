#include "libretro/Input.h"

#include <algorithm>

namespace rrm::lr {

namespace {

constexpr int16_t kTriggerThreshold = 8000;  // ~25% travel counts as a digital L2/R2 press

struct ButtonMap {
    SDL_GamepadButton sdl;
    unsigned retro;
};

// libretro layout: SDL SOUTH (Xbox A / PS Cross) is RetroPad B, EAST is A, WEST is Y, NORTH is X.
constexpr ButtonMap kPadButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, RETRO_DEVICE_ID_JOYPAD_B},
    {SDL_GAMEPAD_BUTTON_EAST, RETRO_DEVICE_ID_JOYPAD_A},
    {SDL_GAMEPAD_BUTTON_WEST, RETRO_DEVICE_ID_JOYPAD_Y},
    {SDL_GAMEPAD_BUTTON_NORTH, RETRO_DEVICE_ID_JOYPAD_X},
    {SDL_GAMEPAD_BUTTON_BACK, RETRO_DEVICE_ID_JOYPAD_SELECT},
    {SDL_GAMEPAD_BUTTON_START, RETRO_DEVICE_ID_JOYPAD_START},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, RETRO_DEVICE_ID_JOYPAD_L},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, RETRO_DEVICE_ID_JOYPAD_R},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, RETRO_DEVICE_ID_JOYPAD_L3},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, RETRO_DEVICE_ID_JOYPAD_R3},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, RETRO_DEVICE_ID_JOYPAD_UP},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},
};

int keyToRetro(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return RETRO_DEVICE_ID_JOYPAD_UP;
    case SDL_SCANCODE_DOWN: return RETRO_DEVICE_ID_JOYPAD_DOWN;
    case SDL_SCANCODE_LEFT: return RETRO_DEVICE_ID_JOYPAD_LEFT;
    case SDL_SCANCODE_RIGHT: return RETRO_DEVICE_ID_JOYPAD_RIGHT;
    case SDL_SCANCODE_Z: return RETRO_DEVICE_ID_JOYPAD_B;
    case SDL_SCANCODE_X: return RETRO_DEVICE_ID_JOYPAD_A;
    case SDL_SCANCODE_A: return RETRO_DEVICE_ID_JOYPAD_Y;
    case SDL_SCANCODE_S: return RETRO_DEVICE_ID_JOYPAD_X;
    case SDL_SCANCODE_Q: return RETRO_DEVICE_ID_JOYPAD_L;
    case SDL_SCANCODE_W: return RETRO_DEVICE_ID_JOYPAD_R;
    case SDL_SCANCODE_1: return RETRO_DEVICE_ID_JOYPAD_L2;
    case SDL_SCANCODE_2: return RETRO_DEVICE_ID_JOYPAD_R2;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return RETRO_DEVICE_ID_JOYPAD_START;
    case SDL_SCANCODE_RSHIFT:
    case SDL_SCANCODE_BACKSPACE: return RETRO_DEVICE_ID_JOYPAD_SELECT;
    default: return -1;
    }
}

int16_t clampAxis(int v) { return static_cast<int16_t>(std::clamp(v, -32768, 32767)); }

}  // namespace

void Input::open() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) attach(ids[i]);
    SDL_free(ids);
}

void Input::close() {
    stopRumble();
    for (auto& p : ports_) {
        if (p.pad) SDL_CloseGamepad(p.pad);
        p = Port{};
    }
    snap_ = {};
    keys_.fill(false);
    virtual_ = PadState{};
}

int Input::portOf(SDL_JoystickID id) const {
    for (unsigned i = 0; i < kPorts; ++i) {
        if (ports_[i].pad && ports_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

void Input::attach(SDL_JoystickID id) {
    if (portOf(id) >= 0) return;
    for (auto& p : ports_) {
        if (p.pad) continue;
        p.pad = SDL_OpenGamepad(id);  // reference counted; the app may hold its own handle
        if (p.pad) {
            p.id = id;
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "[libretro] gamepad \"%s\" -> port %d",
                        SDL_GetGamepadName(p.pad), static_cast<int>(&p - ports_.data()) + 1);
        }
        return;
    }
}

void Input::detach(SDL_JoystickID id) {
    int port = portOf(id);
    if (port < 0) return;
    SDL_CloseGamepad(ports_[port].pad);
    ports_[port] = Port{};
    snap_[port] = Snapshot{};
}

bool Input::handleEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        attach(e.gdevice.which);
        return false;
    case SDL_EVENT_GAMEPAD_REMOVED:
        detach(e.gdevice.which);
        return false;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int id = keyToRetro(e.key.scancode);
        if (id < 0) return false;
        keys_[e.key.scancode] = e.key.down;
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        // Guide/misc buttons stay with the app (menu); everything else belongs to the game.
        return portOf(e.gbutton.which) >= 0 && e.gbutton.button != SDL_GAMEPAD_BUTTON_GUIDE &&
               e.gbutton.button != SDL_GAMEPAD_BUTTON_MISC1 && e.gbutton.button != SDL_GAMEPAD_BUTTON_TOUCHPAD;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return portOf(e.gaxis.which) >= 0;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        clearKeys();
        return false;
    default:
        return false;
    }
}

void Input::poll() {
    for (unsigned port = 0; port < kPorts; ++port) {
        Snapshot s;
        if (SDL_Gamepad* pad = ports_[port].pad) {
            for (const auto& m : kPadButtons) {
                if (SDL_GetGamepadButton(pad, m.sdl)) s.buttons |= 1u << m.retro;
            }
            s.lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
            s.ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            s.rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
            s.ry = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
            s.l2 = std::max<int16_t>(0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            s.r2 = std::max<int16_t>(0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            if (s.l2 > kTriggerThreshold) s.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_L2;
            if (s.r2 > kTriggerThreshold) s.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_R2;
        }
        if (port == 0) {
            for (int sc = 0; sc < SDL_SCANCODE_COUNT; ++sc) {
                if (!keys_[sc]) continue;
                int id = keyToRetro(static_cast<SDL_Scancode>(sc));
                if (id >= 0) s.buttons |= 1u << id;
            }
            s.buttons |= virtual_.buttons & 0xFFFFu;
            s.lx = clampAxis(s.lx + virtual_.lx);
            s.ly = clampAxis(s.ly + virtual_.ly);
            s.rx = clampAxis(s.rx + virtual_.rx);
            s.ry = clampAxis(s.ry + virtual_.ry);
        }
        // Digital-only sources still report full analog pressure for L2/R2.
        if (s.buttons & (1u << RETRO_DEVICE_ID_JOYPAD_L2)) s.l2 = 0x7FFF;
        if (s.buttons & (1u << RETRO_DEVICE_ID_JOYPAD_R2)) s.r2 = 0x7FFF;
        snap_[port] = s;
    }
}

int16_t Input::state(unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port >= kPorts) return 0;
    const Snapshot& s = snap_[port];
    switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<int16_t>(s.buttons & 0xFFFFu);
        return id < 16 ? static_cast<int16_t>((s.buttons >> id) & 1u) : 0;
    case RETRO_DEVICE_ANALOG:
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) return id == RETRO_DEVICE_ID_ANALOG_X ? s.lx : s.ly;
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) return id == RETRO_DEVICE_ID_ANALOG_X ? s.rx : s.ry;
        if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
            if (id == RETRO_DEVICE_ID_JOYPAD_L2) return s.l2;
            if (id == RETRO_DEVICE_ID_JOYPAD_R2) return s.r2;
            if (id < 16) return (s.buttons >> id) & 1u ? 0x7FFF : 0;
        }
        return 0;
    default:
        return 0;
    }
}

bool Input::rumble(unsigned port, retro_rumble_effect effect, uint16_t strength) {
    if (port >= kPorts || !ports_[port].pad) return false;
    Port& p = ports_[port];
    if (effect == RETRO_RUMBLE_STRONG) p.strong = strength;
    else p.weak = strength;
    // Long duration: cores only report changes; the next change (or stop) replaces it.
    return SDL_RumbleGamepad(p.pad, p.strong, p.weak, (p.strong || p.weak) ? 5000 : 0);
}

void Input::stopRumble() {
    for (auto& p : ports_) {
        if (p.pad && (p.strong || p.weak)) SDL_RumbleGamepad(p.pad, 0, 0, 0);
        p.strong = p.weak = 0;
    }
}

}  // namespace rrm::lr
