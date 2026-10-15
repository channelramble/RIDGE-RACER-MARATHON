// What Marathon's controller settings mean for each game on the in-app cores.
//
// PS1 racers (SwanStation, PCSX-ReARMed): all three support Namco's neGcon, whose twist steering and
// I / II pedals are analog, so it is the default: left stick X twists, RT / LT press I / II as far as
// they are pulled, and Cross / Square still give full throttle / brake (both cores read I from
// RetroPad B or R2, II from Y or L2). Neither core's neGcon rumbles; R4 also takes a DualShock, which
// does (pedals on the buttons). Digital steering is the original digital pad, the stick folded onto
// the d-pad.
// PSP (PPSSPP): the core never reads L2/R2, so the triggers press Cross / Square (digital: the PSP
// has no analog buttons).
// Rave Racer (MAME): the wheel and pedals are analog already (left stick X, RT, LT); the d-pad and
// arrow keys steer at full lock, and the stick's Y axis is cut so diagonal steering cannot shift gears.
//
// Marathon applies the stick dead zone itself (libretro/Input.cpp), so the cores' own steering dead
// zones are zero.
#include "backends/LibretroControls.h"

#include <algorithm>

namespace rrm {

namespace {

enum class Ps1Pad { NeGcon, DualShock, Digital };

bool ps1Core(const std::string& core) { return core == "swanstation" || core == "pcsx_rearmed"; }
bool ps1Racer(const std::string& id) { return id == "ridge-racer" || id == "rage-racer" || id == "r4"; }
bool pspRacer(const std::string& id) { return id == "ridge-racer-psp" || id == "ridge-racer-2-psp"; }

bool digitalSteering(const ControllerSettings& c) { return c.steering == "dpad"; }
bool buttonPedals(const ControllerSettings& c) { return c.pedals == "buttons"; }

Ps1Pad ps1Pad(const std::string& gameId, const ControllerSettings& c) {
    if (digitalSteering(c)) return Ps1Pad::Digital;
    if (buttonPedals(c) && gameId == "r4") return Ps1Pad::DualShock;
    return Ps1Pad::NeGcon;
}

const char* flag(bool on) { return on ? "1" : "0"; }

constexpr const char* kMarathonMenu = "Guide or Esc: Marathon's pause menu";

// Menu buttons on the neGcon (I = Cross / RT, II = Square / LT, A = Circle, B = Triangle). Rage Racer's
// menus say "OK: A, I / CANCEL: B, II" and R4's "I Button: O.K."; Ridge Racer follows its manual's
// Circle / Triangle (its menus also take Start).
const char* negconMenus(const std::string& gameId) {
    if (gameId == "rage-racer")
        return "Menus: A or B (Cross or Circle) confirms, X or Y (Square or Triangle) goes back";
    if (gameId == "r4") return "Menus: A (Cross) confirms";
    return "Menus: B (Circle) confirms, Y (Triangle) goes back";
}

}  // namespace

json libretroControllerOptions(const std::string& core, const std::string& gameId, const ControllerSettings& c) {
    json o = json::object();
    o["rrm_input_deadzone"] = std::to_string(std::clamp(c.deadzone, 0, 40));
    o["rrm_rumble"] = flag(c.vibration);
    for (size_t i = 0; i < c.players.size(); ++i) o["rrm_player" + std::to_string(i + 1)] = c.players[i].controller;

    if (ps1Core(core) && ps1Racer(gameId)) {
        const Ps1Pad pad = ps1Pad(gameId, c);
        const char* device = pad == Ps1Pad::NeGcon ? "negcon" : pad == Ps1Pad::DualShock ? "dualshock" : "";
        o["rrm_port0_device"] = device;  // matched against the names the core declares
        o["rrm_port1_device"] = device;
        if (pad == Ps1Pad::NeGcon) {
            o["rrm_input_dpad_to_stick"] = "1";
            if (buttonPedals(c)) o["rrm_input_analog_triggers"] = "0";
        } else if (pad == Ps1Pad::Digital) {
            o["rrm_input_stick_to_dpad"] = "1";
            if (!buttonPedals(c)) {
                o["rrm_input_r2_as"] = "b";  // RT: Cross, accelerate
                o["rrm_input_l2_as"] = "y";  // LT: Square, brake
            }
        }
    } else if (core == "ppsspp" && pspRacer(gameId)) {
        if (digitalSteering(c)) o["rrm_input_stick_to_dpad"] = "1";
        if (!buttonPedals(c)) {
            o["rrm_input_r2_as"] = "b";  // Cross: accelerate
            o["rrm_input_l2_as"] = "y";  // Square: brake
        }
    } else if (core == "mame" && gameId == "rave-racer") {
        o["rrm_input_dpad_to_stick"] = "1";
        o["rrm_input_left_y"] = "0";
    }

    if (core == "swanstation") {
        o["swanstation_Controller1_SteeringDeadzone"] = "0.00";
        o["swanstation_Controller2_SteeringDeadzone"] = "0.00";
        o["swanstation_Controller1_TwistResponse"] = "quadratic";  // finer near the centre (the core's advice for pads)
        o["swanstation_Controller2_TwistResponse"] = "quadratic";
        o["swanstation_Controller_EnableRumble"] = c.vibration ? "true" : "false";
    } else if (core == "pcsx_rearmed") {
        o["pcsx_rearmed_negcon_deadzone"] = "0";
        o["pcsx_rearmed_negcon_response"] = "quadratic";
        o["pcsx_rearmed_vibration"] = c.vibration ? "enabled" : "disabled";
    } else if (core == "ppsspp") {
        o["ppsspp_analog_deadzone"] = "0.0";
    } else if (core == "mame") {
        o["mame_joystick_deadzone"] = "0.00";
    }
    return o;
}

ControlsInfo libretroControls(const std::string& core, const std::string& gameId, const ControllerSettings& c) {
    ControlsInfo i;
    i.deadzone = true;      // Marathon's own input layer applies these in every in-app game
    i.playerChoice = true;
    i.menu = kMarathonMenu;
    const bool triggers = !buttonPedals(c);

    if (ps1Core(core) && ps1Racer(gameId)) {
        const Ps1Pad pad = ps1Pad(gameId, c);
        i.steering = i.pedals = true;
        i.vibration = pad == Ps1Pad::DualShock;  // neither core's neGcon nor the digital pad rumbles
        i.players = gameId == "r4" ? 2 : 1;      // R4's VS Battle is split screen
        switch (pad) {
            case Ps1Pad::NeGcon:
                i.layout = {"Left stick: steer (analog neGcon twist); the d-pad steers at full lock",
                            triggers ? "RT / LT: accelerate / brake (analog); "
                                       "A / X (Cross / Square): full throttle / brake"
                                     : "A / X (Cross / Square): accelerate / brake",
                            "LB / RB: neGcon L / R", negconMenus(gameId), "Start: pause"};
                break;
            case Ps1Pad::DualShock:
                i.layout = {"Left stick: steer (analog)", "A / X (Cross / Square): accelerate / brake",
                            "LB/LT shift down, RB/RT shift up", "Start: pause"};
                break;
            case Ps1Pad::Digital:
                i.layout = {"D-pad or left stick: steer (full lock)", "A (Cross): accelerate, X (Square): brake"};
                if (triggers) {
                    i.layout.push_back("RT / LT: accelerate / brake");
                    i.layout.push_back("LB / RB: shift (manual)");
                } else {
                    i.layout.push_back("LB/RB and LT/RT: shift (manual)");
                }
                i.layout.push_back("Start: pause");
                break;
        }
    } else if (core == "ppsspp" && pspRacer(gameId)) {
        i.steering = i.pedals = true;
        i.layout = {digitalSteering(c) ? "D-pad or left stick: steer (full lock)" : "Left stick or d-pad: steer",
                    triggers ? "RT or A (Cross): accelerate; LT or X (Square): brake"
                             : "A (Cross): accelerate; X (Square): brake",
                    "LB: camera, RB: nitrous", "Y / B (Triangle / Circle): shift"};
        i.note = "The PSP has no analog pedals.";
    } else if (core == "mame" && gameId == "rave-racer") {
        // The cabinet's wheel and pedals are analog: Steering and Accelerate and brake do not apply.
        i.layout = {"Left stick: steering wheel (analog); d-pad left/right: full lock",
                    "RT / LT: gas / brake (analog); A / B: full", "D-pad up/down: shift (manual)", "Y: view",
                    "Back: insert coin"};
    } else {
        i.vibration = true;  // passed on when the core vibrates
    }
    return i;
}

}  // namespace rrm
