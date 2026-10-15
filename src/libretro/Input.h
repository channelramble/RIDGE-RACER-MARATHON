// RetroPad input: SDL gamepads (ports 1 to 4, chosen like Marathon's player slots), keyboard and
// on-screen pad (port 1).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

#include "core/Controllers.h"
#include "core/Json.h"
#include "core/Settings.h"
#include "libretro.h"
#include "libretro/Host.h"

namespace rrm::lr {

// How the pads reach the core, from Marathon's controller settings (the rrm_* core options, see
// Host.cpp above applyPortDevices). The defaults are the plain RetroPad.
struct InputConfig {
    std::array<PlayerChoice, 4> players;   // from rrm_player1..4
    int deadzone = 0;                      // 0..32767, both sticks, scaled radial (applyStickDeadzone)
    bool stickToDpad = false;              // left stick past 50 % presses the d-pad; the left stick then reads 0
    bool dpadToStick = false;              // d-pad / arrow keys / touch left-right push left X to full lock
                                           // (when the pad's own stick X is inside the dead zone)
    int r2As = -1, l2As = -1;              // RETRO_DEVICE_ID_JOYPAD_* a trigger presses instead of R2/L2
    bool analogTriggers = true;            // false: analog L2/R2 read 0 (digital bits unchanged)
    bool leftY = true;                     // false: left stick Y reads 0 (lr-mame: no shifting by diagonal steering)
    bool rumble = true;
};
// Reads the rrm_* keys of HostConfig::coreOptions; absent or unknown values keep the defaults.
InputConfig inputConfigFromOptions(const json& coreOptions);

class Input {
public:
    static constexpr unsigned kPorts = 4;

    void configure(const InputConfig& config);  // before open(); reassigns the pads when called later
    const InputConfig& config() const { return cfg_; }

    void open();   // claims gamepads that are already connected
    void close();
    // Catches up with gamepads that came or went while events did not reach the host (pause menu)
    // and with the window focus.
    void refresh(bool focused);
    bool handleEvent(const SDL_Event& e);
    void setVirtualPad(const PadState& pad) { virtual_ = pad; }
    void clearKeys() { keys_.fill(false); }

    void poll();   // snapshots the state the core reads during the next retro_run
    int16_t state(unsigned port, unsigned device, unsigned index, unsigned id) const;
    bool rumble(unsigned port, retro_rumble_effect effect, uint16_t strength);
    void stopRumble();
    bool connected(unsigned port) const { return port < kPorts && ports_[port].pad; }
    SDL_JoystickID padOn(unsigned port) const { return connected(port) ? ports_[port].id : 0; }

private:
    struct Port {
        SDL_Gamepad* pad = nullptr;
        SDL_JoystickID id = 0;
        uint16_t strong = 0, weak = 0;
    };
    struct Snapshot {
        uint32_t buttons = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
        std::array<int16_t, 16> pressure{};  // RETRO_DEVICE_INDEX_ANALOG_BUTTON, by RetroPad id
    };
    void attach(SDL_JoystickID id);
    void detach(SDL_JoystickID id);
    void assign();  // player slots -> ports (rrm::assignPlayers), opening and closing handles
    int portOf(SDL_JoystickID id) const;

    InputConfig cfg_;
    std::vector<ConnectedPad> pads_;  // every connected gamepad, connection order
    std::array<Port, kPorts> ports_{};
    std::array<Snapshot, kPorts> snap_{};
    std::array<bool, SDL_SCANCODE_COUNT> keys_{};
    PadState virtual_{};
    bool focused_ = true;  // pads read neutral while another window has the focus
};

}  // namespace rrm::lr
