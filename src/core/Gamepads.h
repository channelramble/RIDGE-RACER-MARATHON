// Connected gamepads through SDL: descriptions for the UI and the backends, and the monitor
// that keeps every pad open for hot-plug messages and the quit chord (docs/CONTROLLERS.md).
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/Controllers.h"

namespace rrm {

ConnectedPad describePad(SDL_JoystickID id);     // fills every field except player
std::vector<ConnectedPad> connectedPads();       // SDL_GetGamepads order (= connection order), nameIndex set

struct PadEvent {
    enum class Kind { Connected, Disconnected, QuitChord } kind;
    ConnectedPad pad;
};

// Marathon keeps every gamepad open while it runs, also while another program has focus
// (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS), so it can tell when one is plugged in or out
// and watch for the quit chord. Main thread only.
class GamepadMonitor {
public:
    void open();                                 // opens every connected gamepad (SDL handles are ref-counted)
    void close();
    void handleEvent(const SDL_Event& e);        // ADDED/REMOVED; any button/axis past 50 % marks the pad "last used"
    std::vector<PadEvent> update(uint64_t nowMs, const ControllerSettings& c);  // hot-plug + chord events
    std::vector<ConnectedPad> pads() const;      // connection order, player slots from the sticky assignment
    SDL_Gamepad* gamepad(uint32_t id) const;
    uint32_t lastUsed() const;
    bool rumble(uint32_t id, uint16_t low, uint16_t high, uint32_t ms);
    void resetChord();
    float chordProgress(uint64_t nowMs) const;

    uint32_t playerPad(int player) const;        // joystick id in a player slot (0 = nobody)
    bool connected(uint32_t id) const;

private:
    struct Pad {
        ConnectedPad info;
        SDL_Gamepad* handle = nullptr;
    };
    // Plug and unplug messages for one controller model and name, at most one per 2 s.
    struct Notice {
        uint64_t lastAt = 0;
        bool hasLast = false;
        bool pending = false;     // a message held back by the 2 s limit
        PadEvent event{PadEvent::Kind::Connected, {}};
    };
    void add(SDL_JoystickID id);
    void remove(SDL_JoystickID id);
    void reassign();
    void queue(PadEvent::Kind kind, const ConnectedPad& pad);

    std::vector<Pad> pads_;                      // connection order
    std::array<uint32_t, 4> slots_{};
    std::array<PlayerChoice, 4> choices_{};
    QuitChordDetector chord_;
    std::vector<PadEvent> raw_;                  // plug events since the last update()
    std::map<std::string, Notice> notices_;      // by guid + name
    bool open_ = false;
    bool started_ = false;                       // update() ran since open()
    uint64_t quietUntil_ = 0;                    // no plug messages before this (start-up)
    uint32_t lastUsed_ = 0;
};

}  // namespace rrm
