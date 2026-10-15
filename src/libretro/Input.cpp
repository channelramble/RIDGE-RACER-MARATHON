#include "libretro/Input.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "core/Gamepads.h"
#include "libretro/CoreOptions.h"

namespace rrm::lr {

namespace {

constexpr int16_t kTriggerThreshold = 8000;  // ~25% travel counts as a digital L2/R2 press
constexpr int kTriggerBand = 655;            // 2%: a trigger at rest reads 0, the rest is rescaled
constexpr int16_t kStickToDpad = 16384;      // stickToDpad: half travel presses the d-pad
constexpr int16_t kStickRest = 655;          // dpadToStick without a dead zone: a stick this near the centre rests

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

constexpr uint32_t bit(unsigned id) { return 1u << id; }

int16_t clampAxis(int v) { return static_cast<int16_t>(std::clamp(v, -32768, 32767)); }

// SDL trigger (0..32767) -> travel after the fixed dead band, still reaching 32767 at the end.
int16_t triggerTravel(int16_t raw) {
    const int v = std::max<int>(0, raw);
    if (v <= kTriggerBand) return 0;
    return static_cast<int16_t>(std::min(32767, (v - kTriggerBand) * 32767 / (32767 - kTriggerBand)));
}

std::string lowerTrim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    for (auto& c : s) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string optionText(const json& options, const std::string& key) {
    if (!options.is_object()) return {};
    auto it = options.find(key);
    return it == options.end() || it->is_null() ? std::string() : lowerTrim(optionValueString(*it));
}

// "1"/"0", also libretro's "enabled"/"disabled" and "true"/"false"; anything else keeps the default.
bool optionFlag(const json& options, const std::string& key, bool fallback) {
    const std::string v = optionText(options, key);
    if (v == "1" || v == "true" || v == "enabled" || v == "on" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "disabled" || v == "off" || v == "no") return false;
    return fallback;
}

int retroButton(const std::string& name) {
    static const struct {
        const char* name;
        int id;
    } kNames[] = {{"b", RETRO_DEVICE_ID_JOYPAD_B},   {"a", RETRO_DEVICE_ID_JOYPAD_A},
                  {"y", RETRO_DEVICE_ID_JOYPAD_Y},   {"x", RETRO_DEVICE_ID_JOYPAD_X},
                  {"l", RETRO_DEVICE_ID_JOYPAD_L},   {"r", RETRO_DEVICE_ID_JOYPAD_R},
                  {"l3", RETRO_DEVICE_ID_JOYPAD_L3}, {"r3", RETRO_DEVICE_ID_JOYPAD_R3},
                  {"select", RETRO_DEVICE_ID_JOYPAD_SELECT}, {"start", RETRO_DEVICE_ID_JOYPAD_START}};
    for (const auto& n : kNames)
        if (name == n.name) return n.id;
    return -1;
}

// "" (automatic), "none", or an SDL GUID (32 hex digits); anything else is automatic.
PlayerChoice playerChoice(const std::string& v) {
    PlayerChoice c;
    if (v == "none") c.controller = v;
    else if (v.size() == 32 && v.find_first_not_of("0123456789abcdef") == std::string::npos) c.controller = v;
    return c;
}

}  // namespace

InputConfig inputConfigFromOptions(const json& o) {
    InputConfig c;
    for (unsigned i = 0; i < c.players.size(); ++i)
        c.players[i] = playerChoice(optionText(o, "rrm_player" + std::to_string(i + 1)));
    const std::string dz = optionText(o, "rrm_input_deadzone");
    if (!dz.empty())
        c.deadzone = deadzoneUnits(std::clamp(static_cast<int>(std::strtol(dz.c_str(), nullptr, 10)), 0, 40));
    c.stickToDpad = optionFlag(o, "rrm_input_stick_to_dpad", c.stickToDpad);
    c.dpadToStick = optionFlag(o, "rrm_input_dpad_to_stick", c.dpadToStick);
    c.r2As = retroButton(optionText(o, "rrm_input_r2_as"));
    c.l2As = retroButton(optionText(o, "rrm_input_l2_as"));
    c.analogTriggers = optionFlag(o, "rrm_input_analog_triggers", c.analogTriggers);
    c.leftY = optionFlag(o, "rrm_input_left_y", c.leftY);
    c.rumble = optionFlag(o, "rrm_rumble", c.rumble);
    return c;
}

void Input::configure(const InputConfig& config) {
    cfg_ = config;
    if (!cfg_.rumble) stopRumble();
    if (!pads_.empty()) assign();
}

void Input::open() {
    pads_ = connectedPads();
    focused_ = true;
    assign();
}

void Input::close() {
    stopRumble();
    for (auto& p : ports_) {
        if (p.pad) SDL_CloseGamepad(p.pad);
        p = Port{};
    }
    pads_.clear();
    snap_ = {};
    keys_.fill(false);
    virtual_ = PadState{};
    focused_ = true;
}

void Input::refresh(bool focused) {
    focused_ = focused;
    std::vector<ConnectedPad> now = connectedPads();
    std::erase_if(pads_, [&](const ConnectedPad& p) {
        return std::none_of(now.begin(), now.end(), [&](const ConnectedPad& n) { return n.id == p.id; });
    });
    for (auto& n : now) {
        if (std::none_of(pads_.begin(), pads_.end(), [&](const ConnectedPad& p) { return p.id == n.id; }))
            pads_.push_back(n);
    }
    assign();
}

int Input::portOf(SDL_JoystickID id) const {
    for (unsigned i = 0; i < kPorts; ++i) {
        if (ports_[i].pad && ports_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

void Input::assign() {
    std::array<uint32_t, 4> current{};
    for (unsigned i = 0; i < kPorts; ++i) current[i] = ports_[i].pad ? ports_[i].id : 0;
    const std::array<uint32_t, 4> slots = assignPlayers(pads_, cfg_.players, current);
    std::array<Port, kPorts> next{};
    for (unsigned i = 0; i < kPorts; ++i) {
        if (!slots[i]) continue;
        if (int from = portOf(slots[i]); from >= 0) {
            next[i] = ports_[from];  // the handle moves along (or stays)
            ports_[from] = Port{};
            if (static_cast<unsigned>(from) != i && (next[i].strong || next[i].weak)) {
                SDL_RumbleGamepad(next[i].pad, 0, 0, 0);  // the core rumbles ports, not pads
                next[i].strong = next[i].weak = 0;
            }
            continue;
        }
        next[i].pad = SDL_OpenGamepad(slots[i]);  // reference counted; the app holds its own handles
        if (next[i].pad) next[i].id = slots[i];
    }
    for (auto& p : ports_) {  // pads that lost their port (unplugged, or not a player any more)
        if (!p.pad) continue;
        if (p.strong || p.weak) SDL_RumbleGamepad(p.pad, 0, 0, 0);
        SDL_CloseGamepad(p.pad);
    }
    for (unsigned i = 0; i < kPorts; ++i) {
        if (next[i].id == current[i] && (next[i].pad != nullptr) == (current[i] != 0)) continue;
        snap_[i] = Snapshot{};
        if (next[i].pad)
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "[libretro] gamepad \"%s\" -> port %u", SDL_GetGamepadName(next[i].pad),
                        i + 1);
    }
    ports_ = next;
}

void Input::attach(SDL_JoystickID id) {
    if (std::any_of(pads_.begin(), pads_.end(), [&](const ConnectedPad& p) { return p.id == id; })) return;
    ConnectedPad pad = describePad(id);
    pad.nameIndex = 1 + static_cast<int>(std::count_if(pads_.begin(), pads_.end(),
                                                       [&](const ConnectedPad& p) { return p.name == pad.name; }));
    pads_.push_back(std::move(pad));
    assign();
}

void Input::detach(SDL_JoystickID id) {
    const size_t before = pads_.size();
    std::erase_if(pads_, [&](const ConnectedPad& p) { return p.id == id; });
    if (pads_.size() != before || portOf(id) >= 0) assign();
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
        // Pad events still arrive in the background (for Marathon's quit chord); the game ignores them.
        clearKeys();
        focused_ = false;
        return false;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        focused_ = true;
        return false;
    default:
        return false;
    }
}

void Input::poll() {
    for (unsigned port = 0; port < kPorts; ++port) {
        Snapshot s;
        uint32_t digital = 0;             // keyboard and on-screen pad: no analog travel of their own
        uint32_t byTrigger = 0;           // bits only a trigger pressed: their pressure is its travel
        std::array<int16_t, 16> travel{}; // trigger travel, by the RetroPad button the trigger presses
        int16_t padX = 0;                 // the pad's own left stick X (dpadToStick)
        if (SDL_Gamepad* pad = ports_[port].pad; pad && focused_) {
            for (const auto& m : kPadButtons) {
                if (SDL_GetGamepadButton(pad, m.sdl)) s.buttons |= bit(m.retro);
            }
            s.lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
            s.ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            s.rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
            s.ry = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
            applyStickDeadzone(s.lx, s.ly, cfg_.deadzone);
            applyStickDeadzone(s.rx, s.ry, cfg_.deadzone);
            padX = s.lx;
            // A trigger presses L2/R2, or the button it is remapped to (the core then reads its
            // travel as that button's analog pressure).
            auto trigger = [&](SDL_GamepadAxis axis, unsigned own, int remap) {
                const int16_t v = triggerTravel(SDL_GetGamepadAxis(pad, axis));
                const unsigned target = remap >= 0 && remap < 16 ? static_cast<unsigned>(remap) : own;
                if (v > kTriggerThreshold && !(s.buttons & bit(target))) {
                    s.buttons |= bit(target);
                    byTrigger |= bit(target);
                }
                travel[target] = std::max(travel[target], v);
            };
            trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, RETRO_DEVICE_ID_JOYPAD_L2, cfg_.l2As);
            trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, RETRO_DEVICE_ID_JOYPAD_R2, cfg_.r2As);
        }
        if (port == 0) {
            for (int sc = 0; sc < SDL_SCANCODE_COUNT; ++sc) {
                if (!keys_[sc]) continue;
                int id = keyToRetro(static_cast<SDL_Scancode>(sc));
                if (id >= 0) digital |= bit(id);
            }
            digital |= virtual_.buttons & 0xFFFFu;
            s.buttons |= digital;
            byTrigger &= ~digital;
            s.lx = clampAxis(s.lx + virtual_.lx);
            s.ly = clampAxis(s.ly + virtual_.ly);
            s.rx = clampAxis(s.rx + virtual_.rx);
            s.ry = clampAxis(s.ry + virtual_.ry);
        }
        if (cfg_.stickToDpad) {
            if (s.lx <= -kStickToDpad) s.buttons |= bit(RETRO_DEVICE_ID_JOYPAD_LEFT);
            if (s.lx >= kStickToDpad) s.buttons |= bit(RETRO_DEVICE_ID_JOYPAD_RIGHT);
            if (s.ly <= -kStickToDpad) s.buttons |= bit(RETRO_DEVICE_ID_JOYPAD_UP);
            if (s.ly >= kStickToDpad) s.buttons |= bit(RETRO_DEVICE_ID_JOYPAD_DOWN);
            s.lx = s.ly = 0;
        }
        if (cfg_.dpadToStick) {
            const bool left = s.buttons & bit(RETRO_DEVICE_ID_JOYPAD_LEFT);
            const bool right = s.buttons & bit(RETRO_DEVICE_ID_JOYPAD_RIGHT);
            const int rest = cfg_.deadzone > 0 ? 0 : kStickRest;  // after the dead zone a resting stick reads 0
            if (left != right && std::abs(padX) <= rest) s.lx = left ? -32767 : 32767;
        }
        if (!cfg_.leftY) s.ly = 0;
        if (!cfg_.analogTriggers) {  // the triggers are plain buttons
            travel.fill(0);
            byTrigger = 0;
        }

        // Buttons report full pressure; a button a trigger pressed reports how far the trigger is
        // pulled. (Forcing 0x7FFF on a trigger's digital bit made analog pedals all-or-nothing past
        // a quarter of the travel.) Analog L2/R2 come from the triggers or digital sources only.
        const uint32_t full = s.buttons & ~byTrigger;
        for (unsigned id = 0; id < 16; ++id)
            s.pressure[id] = std::max<int16_t>((full >> id) & 1u ? 0x7FFF : 0, travel[id]);
        for (unsigned id : {RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2})
            s.pressure[id] = !cfg_.analogTriggers ? 0 : (digital & bit(id)) ? 0x7FFF : travel[id];
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
        if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) return id < 16 ? s.pressure[id] : 0;
        return 0;
    default:
        return 0;
    }
}

bool Input::rumble(unsigned port, retro_rumble_effect effect, uint16_t strength) {
    if (!cfg_.rumble || port >= kPorts || !ports_[port].pad) return false;
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
