// RetroPad input: SDL gamepads (port 0 and 1), keyboard and on-screen pad (port 0).
#pragma once

#include <array>
#include <cstdint>

#include <SDL3/SDL.h>

#include "libretro.h"
#include "libretro/Host.h"

namespace rrm::lr {

class Input {
public:
    static constexpr unsigned kPorts = 2;

    void open();   // claims gamepads that are already connected
    void close();
    bool handleEvent(const SDL_Event& e);
    void setVirtualPad(const PadState& pad) { virtual_ = pad; }
    void clearKeys() { keys_.fill(false); }

    void poll();   // snapshots the state the core reads during the next retro_run
    int16_t state(unsigned port, unsigned device, unsigned index, unsigned id) const;
    bool rumble(unsigned port, retro_rumble_effect effect, uint16_t strength);
    void stopRumble();
    bool connected(unsigned port) const { return port < kPorts && ports_[port].pad; }

private:
    struct Port {
        SDL_Gamepad* pad = nullptr;
        SDL_JoystickID id = 0;
        uint16_t strong = 0, weak = 0;
    };
    struct Snapshot {
        uint32_t buttons = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0, l2 = 0, r2 = 0;
    };
    void attach(SDL_JoystickID id);
    void detach(SDL_JoystickID id);
    int portOf(SDL_JoystickID id) const;

    std::array<Port, kPorts> ports_{};
    std::array<Snapshot, kPorts> snap_{};
    std::array<bool, SDL_SCANCODE_COUNT> keys_{};
    PadState virtual_{};
};

}  // namespace rrm::lr
