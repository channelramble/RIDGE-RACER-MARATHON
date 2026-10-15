// Checks the in-app cores' input layer (src/libretro/Input.cpp) with SDL virtual gamepads, and the
// controller options the libretro adapter writes for each game (src/backends/LibretroControls.cpp).
// Needs no window, no cores and no games; run it from the repository root (for the catalog).
//
//   lr_input_test [--catalog DIR] [--data DIR]
//
// Covers: analog trigger travel (triggers used to read fully pressed past a quarter of their travel),
// the stick dead zone, stick to d-pad and d-pad to stick, a trigger pressing another button, analog
// triggers off, left stick Y off, player slots to ports (a chosen controller, unplug and replug, four
// pads), vibration off, window focus, pads missed while the pause menu was open, the rrm_* option
// parser, libretroControllerOptions() for every core x game x steering x pedals, and the real
// adapter's launch(), where a game's own advanced coreOptions still win. Prints one line per check
// and exits 1 if any failed.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "backends/Backend.h"
#include "backends/EmuCommon.h"
#include "backends/LibretroControls.h"
#include "core/Catalog.h"
#include "core/Controllers.h"
#include "core/Gamepads.h"
#include "core/Library.h"
#include "core/Platform.h"
#include "libretro.h"
#include "libretro/Input.h"

using namespace rrm;
using rrm::lr::Input;
using rrm::lr::InputConfig;

namespace {

int g_failures = 0, g_checks = 0;

void check(bool ok, const char* fmt, ...) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("%s ", ok ? "ok  " : "FAIL");
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
}

// ---- virtual gamepads --------------------------------------------------------------------------

struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* js = nullptr;
    int rumbleCalls = 0;

    static bool SDLCALL onRumble(void* user, Uint16, Uint16) {
        ++static_cast<VirtualPad*>(user)->rumbleCalls;
        return true;
    }
    bool attach(const char* name, Uint16 vendor, Uint16 product) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.vendor_id = vendor;
        desc.product_id = product;
        desc.name = name;
        desc.userdata = this;
        desc.Rumble = onRumble;
        id = SDL_AttachVirtualJoystick(&desc);
        js = id ? SDL_OpenJoystick(id) : nullptr;
        if (!js) std::printf("cannot attach a virtual gamepad: %s\n", SDL_GetError());
        return js != nullptr;
    }
    void detach() {
        if (js) SDL_CloseJoystick(js);
        if (id) SDL_DetachVirtualJoystick(id);
        js = nullptr;
        id = 0;
    }
    void button(SDL_GamepadButton b, bool down) { SDL_SetJoystickVirtualButton(js, b, down); }
    void stick(SDL_GamepadAxis a, double fraction) {  // -1..1
        SDL_SetJoystickVirtualAxis(js, a, static_cast<Sint16>(std::lround(std::clamp(fraction, -1.0, 1.0) * 32767)));
    }
    // Virtual gamepad triggers use the whole joystick range: -32768 released, 32767 fully pressed.
    void trigger(SDL_GamepadAxis a, double fraction) {
        const int t = static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 32767));
        SDL_SetJoystickVirtualAxis(js, a, static_cast<Sint16>(t * 2 - 32768 + (t == 32767)));
    }
    void neutral() {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) button(static_cast<SDL_GamepadButton>(b), false);
        for (auto a : {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY})
            stick(a, 0);
        trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
        trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    }
};

// Delivers SDL's events (gamepads added/removed) to the input layer, then snapshots it.
void sync(Input& in, bool deliver = true) {
    SDL_UpdateJoysticks();
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (deliver) in.handleEvent(e);
    in.poll();
}

void sendKey(Input& in, SDL_Scancode sc, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.down = down;
    in.handleEvent(e);
}

void sendWindow(Input& in, SDL_EventType type) {
    SDL_Event e{};
    e.type = type;
    in.handleEvent(e);
}

int16_t analogButton(const Input& in, unsigned id, unsigned port = 0) {
    return in.state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, id);
}
bool pressed(const Input& in, unsigned id, unsigned port = 0) { return in.state(port, RETRO_DEVICE_JOYPAD, 0, id) != 0; }
int16_t leftX(const Input& in, unsigned port = 0) {
    return in.state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
}
int16_t leftY(const Input& in, unsigned port = 0) {
    return in.state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
}
int16_t rightX(const Input& in, unsigned port = 0) {
    return in.state(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
}
// SwanStation's neGcon (UpdateControllersNeGcon): I = analog button B, else analog R2.
int16_t swanAxis(const Input& in, unsigned primary, unsigned fallback) {
    int16_t v = analogButton(in, primary);
    return v ? v : analogButton(in, fallback);
}
double percent(int v) { return 100.0 * v / 32767; }
// Trigger travel the core should see: the first 2 % is a dead band, the rest is rescaled.
double travelPercent(double pulled) { return std::max(0.0, (pulled - 0.02) / 0.98) * 100.0; }
bool near(double actual, double expected, double tolerance = 1.0) { return std::fabs(actual - expected) <= tolerance; }

InputConfig withDeadzone(int pct) {
    InputConfig c;
    c.deadzone = deadzoneUnits(pct);
    return c;
}

// ---- input layer -------------------------------------------------------------------------------

void testTriggers(VirtualPad& pad) {
    std::printf("\n-- analog triggers\n");
    Input in;
    in.configure(InputConfig{});
    in.open();
    check(in.connected(0), "the pad is on port 1");
    struct Case {
        double pulled;
        bool bit;
    } cases[] = {{0.0, false}, {0.01, false}, {0.10, false}, {0.20, false}, {0.30, true}, {0.50, true}, {1.0, true}};
    for (const Case& c : cases) {
        pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, c.pulled);
        sync(in);
        const double got = percent(analogButton(in, RETRO_DEVICE_ID_JOYPAD_R2));
        check(near(got, travelPercent(c.pulled)), "RT %3.0f%% reads %5.1f%% (expected %5.1f%%)", c.pulled * 100, got,
              travelPercent(c.pulled));
        check(pressed(in, RETRO_DEVICE_ID_JOYPAD_R2) == c.bit, "RT %3.0f%%: R2 bit %d", c.pulled * 100, c.bit);
        check(near(percent(swanAxis(in, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_R2)), travelPercent(c.pulled)),
              "RT %3.0f%%: SwanStation neGcon I follows the trigger", c.pulled * 100);
    }
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1.0);
    sync(in);
    check(analogButton(in, RETRO_DEVICE_ID_JOYPAD_R2) == 32767, "RT fully pulled reads 32767");
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    pad.trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.5);
    sync(in);
    check(near(percent(analogButton(in, RETRO_DEVICE_ID_JOYPAD_L2)), travelPercent(0.5)), "LT 50%% reads %.1f%%",
          percent(analogButton(in, RETRO_DEVICE_ID_JOYPAD_L2)));
    pad.trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    sendKey(in, SDL_SCANCODE_2, true);
    sync(in);
    check(analogButton(in, RETRO_DEVICE_ID_JOYPAD_R2) == 0x7FFF && pressed(in, RETRO_DEVICE_ID_JOYPAD_R2),
          "keyboard 2 (digital R2) reads fully pressed");
    sendKey(in, SDL_SCANCODE_2, false);
    pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    sync(in);
    check(swanAxis(in, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_R2) == 0x7FFF,
          "Cross / A held: neGcon I fully pressed");
    pad.neutral();
    in.close();
}

void testDeadzone(VirtualPad& pad) {
    std::printf("\n-- stick dead zone (8 %%)\n");
    Input in;
    in.configure(withDeadzone(8));
    in.open();
    const struct {
        double at, expected;
    } cases[] = {{0.05, 0}, {-0.05, 0}, {0.54, 50}, {-0.54, -50}, {1.0, 100}};
    for (auto c : cases) {
        pad.stick(SDL_GAMEPAD_AXIS_LEFTX, c.at);
        pad.stick(SDL_GAMEPAD_AXIS_RIGHTX, c.at);
        sync(in);
        check(near(percent(leftX(in)), c.expected), "left X %4.0f%% reads %6.1f%%", c.at * 100, percent(leftX(in)));
        check(near(percent(rightX(in)), c.expected), "right X %4.0f%% reads %6.1f%%", c.at * 100, percent(rightX(in)));
    }
    pad.stick(SDL_GAMEPAD_AXIS_LEFTX, 0.04);
    pad.stick(SDL_GAMEPAD_AXIS_LEFTY, 0.04);
    sync(in);
    check(leftX(in) == 0 && leftY(in) == 0, "diagonal inside the dead zone reads 0 (radial: 5.7%% of travel)");
    pad.neutral();
    sync(in);
    in.close();

    Input raw;
    raw.configure(InputConfig{});
    raw.open();
    pad.stick(SDL_GAMEPAD_AXIS_LEFTX, 0.05);
    sync(raw);
    check(near(percent(leftX(raw)), 5), "no dead zone: left X 5%% passes through (%.1f%%)", percent(leftX(raw)));
    pad.neutral();
    raw.close();
}

void testStickToDpad(VirtualPad& pad) {
    std::printf("\n-- stick to d-pad\n");
    Input in;
    InputConfig c = withDeadzone(8);
    c.stickToDpad = true;
    in.configure(c);
    in.open();
    pad.stick(SDL_GAMEPAD_AXIS_LEFTX, -0.6);
    sync(in);
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_LEFT) && !pressed(in, RETRO_DEVICE_ID_JOYPAD_RIGHT) && leftX(in) == 0,
          "left X -60%% presses LEFT and the stick reads 0");
    pad.stick(SDL_GAMEPAD_AXIS_LEFTX, 0.4);
    pad.stick(SDL_GAMEPAD_AXIS_LEFTY, 0.7);
    sync(in);
    check(!pressed(in, RETRO_DEVICE_ID_JOYPAD_RIGHT) && pressed(in, RETRO_DEVICE_ID_JOYPAD_DOWN) && leftY(in) == 0,
          "left X 40%% presses nothing, left Y 70%% presses DOWN");
    pad.neutral();
    in.close();
}

void testDpadToStick(VirtualPad& pad) {
    std::printf("\n-- d-pad to stick\n");
    Input in;
    InputConfig c = withDeadzone(8);
    c.dpadToStick = true;
    in.configure(c);
    in.open();
    sendKey(in, SDL_SCANCODE_LEFT, true);
    sync(in);
    check(leftX(in) == -32767 && pressed(in, RETRO_DEVICE_ID_JOYPAD_LEFT), "keyboard Left arrow: left X -32767 (%d)",
          leftX(in));
    sendKey(in, SDL_SCANCODE_LEFT, false);
    pad.stick(SDL_GAMEPAD_AXIS_LEFTX, 0.03);
    pad.button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    sync(in);
    check(leftX(in) == 32767, "d-pad Right with the stick inside the dead zone: left X +32767 (%d)", leftX(in));
    pad.stick(SDL_GAMEPAD_AXIS_LEFTX, -0.6);
    sync(in);
    check(leftX(in) < -16000 && leftX(in) > -20000, "a stick in use keeps its own value (%d)", leftX(in));
    pad.neutral();
    sync(in);
    check(leftX(in) == 0, "released: left X 0");

    InputConfig none;
    none.dpadToStick = true;  // no dead zone: a stick resting slightly off centre still counts as at rest
    in.configure(none);
    pad.stick(SDL_GAMEPAD_AXIS_LEFTX, 0.01);
    pad.button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    sync(in);
    check(leftX(in) == -32767, "no dead zone, stick at 1%%: d-pad Left gives -32767 (%d)", leftX(in));
    pad.neutral();
    in.close();
}

void testRemap(VirtualPad& pad) {
    std::printf("\n-- triggers pressing other buttons (R2 as B, L2 as Y)\n");
    Input in;
    InputConfig c;
    c.r2As = RETRO_DEVICE_ID_JOYPAD_B;
    c.l2As = RETRO_DEVICE_ID_JOYPAD_Y;
    in.configure(c);
    in.open();
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.8);
    sync(in);
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_B) && !pressed(in, RETRO_DEVICE_ID_JOYPAD_R2), "RT 80%%: B bit, no R2 bit");
    check(near(percent(analogButton(in, RETRO_DEVICE_ID_JOYPAD_B)), travelPercent(0.8)), "RT 80%%: analog B %.1f%%",
          percent(analogButton(in, RETRO_DEVICE_ID_JOYPAD_B)));
    check(analogButton(in, RETRO_DEVICE_ID_JOYPAD_R2) == 0, "RT 80%%: analog R2 0");
    check(near(percent(swanAxis(in, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_R2)), travelPercent(0.8)),
          "RT 80%%: SwanStation neGcon I %.1f%%", percent(swanAxis(in, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_R2)));
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.1);
    sync(in);
    check(!pressed(in, RETRO_DEVICE_ID_JOYPAD_B) && near(percent(analogButton(in, RETRO_DEVICE_ID_JOYPAD_B)), travelPercent(0.1)),
          "RT 10%%: no B bit yet, analog B %.1f%%", percent(analogButton(in, RETRO_DEVICE_ID_JOYPAD_B)));
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    pad.trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.6);
    sync(in);
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_Y) && !pressed(in, RETRO_DEVICE_ID_JOYPAD_L2) &&
              near(percent(analogButton(in, RETRO_DEVICE_ID_JOYPAD_Y)), travelPercent(0.6)),
          "LT 60%%: Y bit, no L2 bit, analog Y %.1f%%", percent(analogButton(in, RETRO_DEVICE_ID_JOYPAD_Y)));
    pad.trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    sync(in);
    check(analogButton(in, RETRO_DEVICE_ID_JOYPAD_B) == 0x7FFF, "Cross / A still gives full B");
    pad.neutral();
    in.close();
}

void testAnalogTriggersOff(VirtualPad& pad) {
    std::printf("\n-- analog triggers off, left Y off\n");
    Input in;
    InputConfig c;
    c.analogTriggers = false;
    c.leftY = false;
    in.configure(c);
    in.open();
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.8);
    pad.trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.8);
    pad.stick(SDL_GAMEPAD_AXIS_LEFTX, 0.5);
    pad.stick(SDL_GAMEPAD_AXIS_LEFTY, 0.7);
    sync(in);
    check(analogButton(in, RETRO_DEVICE_ID_JOYPAD_R2) == 0 && analogButton(in, RETRO_DEVICE_ID_JOYPAD_L2) == 0,
          "analog L2 / R2 read 0");
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_R2) && pressed(in, RETRO_DEVICE_ID_JOYPAD_L2), "the L2 / R2 bits stay");
    check(leftY(in) == 0 && near(percent(leftX(in)), 50), "left Y reads 0, left X still %.1f%%", percent(leftX(in)));
    c.r2As = RETRO_DEVICE_ID_JOYPAD_B;
    in.configure(c);
    sync(in);
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_B) && analogButton(in, RETRO_DEVICE_ID_JOYPAD_B) == 0x7FFF,
          "with R2 as B: RT past a quarter is a plain B press");
    pad.neutral();
    in.close();
}

void testPorts(VirtualPad& a, VirtualPad& b) {
    std::printf("\n-- player slots and ports\n");
    VirtualPad c, d, e;
    const std::string guidB = describePad(b.id).guid;
    check(guidB.size() == 32 && guidB != describePad(a.id).guid, "the second pad has its own GUID (%s)", guidB.c_str());

    Input in;
    in.configure(InputConfig{});
    in.open();
    a.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    b.button(SDL_GAMEPAD_BUTTON_EAST, true);
    sync(in);
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_B, 0) && pressed(in, RETRO_DEVICE_ID_JOYPAD_A, 1),
          "automatic: first pad on port 1, second on port 2");

    InputConfig choose;
    choose.players[0].controller = guidB;
    in.configure(choose);
    sync(in);
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_A, 0) && pressed(in, RETRO_DEVICE_ID_JOYPAD_B, 1) && in.padOn(0) == b.id &&
              in.padOn(1) == a.id,
          "rrm_player1 = second pad's GUID: it plays on port 1, the first pad moves to port 2");
    in.close();

    // The same through the core options, as Host::start() does.
    json opts = {{"rrm_player1", guidB}};
    in.configure(lr::inputConfigFromOptions(opts));
    in.open();
    sync(in);
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_A, 0) && pressed(in, RETRO_DEVICE_ID_JOYPAD_B, 1),
          "the same from the rrm_player1 option");

    // Unplug and replug: the pad left on port 2 stays there; the chosen pad comes back to port 1.
    b.detach();
    sync(in);
    check(!in.connected(0) && in.connected(1) && pressed(in, RETRO_DEVICE_ID_JOYPAD_B, 1),
          "chosen pad unplugged: port 1 empty, the other pad stays on port 2");
    b.attach("Marathon test pad B", 0x054C, 0x0CE6);
    b.button(SDL_GAMEPAD_BUTTON_EAST, true);
    sync(in);
    check(in.connected(0) && pressed(in, RETRO_DEVICE_ID_JOYPAD_A, 0) && pressed(in, RETRO_DEVICE_ID_JOYPAD_B, 1),
          "chosen pad replugged: back on port 1");
    in.close();

    in.configure(InputConfig{});
    in.open();
    a.detach();
    sync(in);
    check(!in.connected(0) && in.connected(1), "automatic: first pad unplugged, the second does not move up");
    a.attach("Marathon test pad A", 0x045E, 0x0B12);
    a.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    sync(in);
    check(in.connected(0) && pressed(in, RETRO_DEVICE_ID_JOYPAD_B, 0), "replugged: it fills port 1 again");

    // Four pads fill four ports; a fifth plays nowhere and its buttons are not the game's.
    c.attach("Marathon test pad C", 0x045E, 0x02EA);
    d.attach("Marathon test pad D", 0x057E, 0x2009);
    e.attach("Marathon test pad E", 0x045E, 0x028E);
    d.button(SDL_GAMEPAD_BUTTON_NORTH, true);
    sync(in);
    check(Input::kPorts == 4 && in.connected(2) && in.connected(3), "four pads: ports 1-4 taken");
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_X, 3), "the fourth pad plays on port 4");
    SDL_Event ev{};
    ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    ev.gbutton.which = e.id;
    ev.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    check(!in.handleEvent(ev), "the fifth pad's buttons stay with Marathon");
    ev.gbutton.which = c.id;
    check(in.handleEvent(ev), "the third pad's buttons go to the game");
    ev.gbutton.button = SDL_GAMEPAD_BUTTON_GUIDE;
    check(!in.handleEvent(ev), "Guide stays with Marathon");

    InputConfig nobody;
    nobody.players[1].controller = "none";
    in.configure(nobody);
    sync(in);
    check(!in.connected(1) && in.connected(0) && in.connected(2) && in.connected(3), "Player 2 = nobody: port 2 stays empty");
    in.close();

    // Pads that came or went while events did not reach the input layer (the pause menu was open).
    in.configure(InputConfig{});
    in.open();
    c.detach();
    d.detach();
    e.detach();
    sync(in, false);
    in.refresh(true);
    in.poll();
    check(in.connected(0) && in.connected(1) && !in.connected(2) && !in.connected(3), "refresh(): unplugged pads are gone");
    c.attach("Marathon test pad C", 0x045E, 0x02EA);
    c.button(SDL_GAMEPAD_BUTTON_WEST, true);
    sync(in, false);
    check(!in.connected(2), "a pad plugged in unseen is not used yet");
    in.refresh(true);
    in.poll();
    check(in.connected(2) && pressed(in, RETRO_DEVICE_ID_JOYPAD_Y, 2), "refresh(): it takes port 3");
    in.close();
    c.detach();
    a.neutral();
    b.neutral();
    sync(in, false);
}

void testRumble(VirtualPad& pad) {
    std::printf("\n-- vibration\n");
    Input in;
    InputConfig off;
    off.rumble = false;
    in.configure(off);
    in.open();
    pad.rumbleCalls = 0;
    const bool sent = in.rumble(0, RETRO_RUMBLE_STRONG, 40000) || in.rumble(0, RETRO_RUMBLE_WEAK, 12000);
    in.stopRumble();
    SDL_UpdateJoysticks();
    check(!sent && pad.rumbleCalls == 0, "vibration off: rumble() is false and the pad is never asked (%d calls)",
          pad.rumbleCalls);
    in.close();

    in.configure(InputConfig{});
    in.open();
    const bool ok = in.rumble(0, RETRO_RUMBLE_STRONG, 40000);
    SDL_UpdateJoysticks();
    check(ok && pad.rumbleCalls > 0, "vibration on: the pad rumbles (%d calls)", pad.rumbleCalls);
    in.stopRumble();
    in.close();
}

void testFocus(VirtualPad& pad) {
    std::printf("\n-- window focus\n");
    Input in;
    in.configure(InputConfig{});
    in.open();
    pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    pad.stick(SDL_GAMEPAD_AXIS_LEFTX, 0.6);
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.6);
    sendKey(in, SDL_SCANCODE_Z, true);
    sync(in);
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_B) && leftX(in) > 0, "focused: the pad plays");
    sendWindow(in, SDL_EVENT_WINDOW_FOCUS_LOST);
    sync(in);
    check(!pressed(in, RETRO_DEVICE_ID_JOYPAD_B) && leftX(in) == 0 && analogButton(in, RETRO_DEVICE_ID_JOYPAD_R2) == 0,
          "focus lost: pad and keys read neutral");
    sendWindow(in, SDL_EVENT_WINDOW_FOCUS_GAINED);
    sync(in);
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_B) && leftX(in) > 0, "focus back: the pad plays again");
    in.refresh(false);
    in.poll();
    check(!pressed(in, RETRO_DEVICE_ID_JOYPAD_B), "refresh(unfocused): neutral");
    in.refresh(true);
    in.poll();
    check(pressed(in, RETRO_DEVICE_ID_JOYPAD_B), "refresh(focused): live");
    pad.neutral();
    in.close();
}

void testOptionParser() {
    std::printf("\n-- rrm_* option parser\n");
    InputConfig d = lr::inputConfigFromOptions(json::object());
    check(d.deadzone == 0 && !d.stickToDpad && !d.dpadToStick && d.r2As < 0 && d.l2As < 0 && d.analogTriggers && d.leftY &&
              d.rumble && d.players[0].controller.empty(),
          "no keys: the plain RetroPad");
    json o = {{"rrm_player1", "03000000DE280000FF11000001000000"}, {"rrm_player2", "none"}, {"rrm_player3", "xbox"},
              {"rrm_input_deadzone", "8"},  {"rrm_input_stick_to_dpad", "1"},  {"rrm_input_dpad_to_stick", "enabled"},
              {"rrm_input_r2_as", "b"},     {"rrm_input_l2_as", "Y"},          {"rrm_input_analog_triggers", "0"},
              {"rrm_input_left_y", "false"}, {"rrm_rumble", "0"}};
    InputConfig c = lr::inputConfigFromOptions(o);
    check(c.players[0].controller == "03000000de280000ff11000001000000", "GUID lower-cased");
    check(c.players[1].controller == "none" && c.players[2].controller.empty() && c.players[3].controller.empty(),
          "\"none\" kept, anything else automatic");
    check(c.deadzone == deadzoneUnits(8), "dead zone 8%% = %d", c.deadzone);
    check(c.stickToDpad && c.dpadToStick && !c.analogTriggers && !c.leftY && !c.rumble, "flags");
    check(c.r2As == RETRO_DEVICE_ID_JOYPAD_B && c.l2As == RETRO_DEVICE_ID_JOYPAD_Y, "R2 as B, L2 as Y");
    InputConfig big = lr::inputConfigFromOptions(json{{"rrm_input_deadzone", "75"}, {"rrm_input_r2_as", "turbo"}});
    check(big.deadzone == deadzoneUnits(40) && big.r2As < 0, "dead zone capped at 40%%, unknown button ignored");
}

// ---- adapter options ---------------------------------------------------------------------------

// What each game should get, written out on its own rather than derived from LibretroControls.cpp:
// the keys besides the ones every game gets, by core, game, steering and pedals.
std::map<std::string, std::string> expectedGameKeys(const std::string& core, const std::string& game, bool dpad,
                                                    bool buttons) {
    const bool ps1 = (core == "swanstation" || core == "pcsx_rearmed") &&
                     (game == "ridge-racer" || game == "rage-racer" || game == "r4");
    if (ps1) {
        if (!dpad && !buttons)
            return {{"rrm_port0_device", "negcon"}, {"rrm_port1_device", "negcon"}, {"rrm_input_dpad_to_stick", "1"}};
        if (!dpad && buttons && game == "r4") return {{"rrm_port0_device", "dualshock"}, {"rrm_port1_device", "dualshock"}};
        if (!dpad && buttons)
            return {{"rrm_port0_device", "negcon"},
                    {"rrm_port1_device", "negcon"},
                    {"rrm_input_dpad_to_stick", "1"},
                    {"rrm_input_analog_triggers", "0"}};
        if (dpad && !buttons)
            return {{"rrm_port0_device", ""},
                    {"rrm_port1_device", ""},
                    {"rrm_input_stick_to_dpad", "1"},
                    {"rrm_input_r2_as", "b"},
                    {"rrm_input_l2_as", "y"}};
        return {{"rrm_port0_device", ""}, {"rrm_port1_device", ""}, {"rrm_input_stick_to_dpad", "1"}};
    }
    if (core == "ppsspp" && (game == "ridge-racer-psp" || game == "ridge-racer-2-psp")) {
        std::map<std::string, std::string> k;
        if (dpad) k["rrm_input_stick_to_dpad"] = "1";
        if (!buttons) {
            k["rrm_input_r2_as"] = "b";
            k["rrm_input_l2_as"] = "y";
        }
        return k;
    }
    if (core == "mame" && game == "rave-racer") return {{"rrm_input_dpad_to_stick", "1"}, {"rrm_input_left_y", "0"}};
    return {};
}

std::map<std::string, std::string> expectedCoreKeys(const std::string& core, bool vibration) {
    if (core == "swanstation")
        return {{"swanstation_Controller1_SteeringDeadzone", "0.00"},
                {"swanstation_Controller2_SteeringDeadzone", "0.00"},
                {"swanstation_Controller1_TwistResponse", "quadratic"},
                {"swanstation_Controller2_TwistResponse", "quadratic"},
                {"swanstation_Controller_EnableRumble", vibration ? "true" : "false"}};
    if (core == "pcsx_rearmed")
        return {{"pcsx_rearmed_negcon_deadzone", "0"},
                {"pcsx_rearmed_negcon_response", "quadratic"},
                {"pcsx_rearmed_vibration", vibration ? "enabled" : "disabled"}};
    if (core == "ppsspp") return {{"ppsspp_analog_deadzone", "0.0"}};
    if (core == "mame") return {{"mame_joystick_deadzone", "0.00"}};
    return {};
}

void testControllerOptions(const std::vector<std::string>& games) {
    std::printf("\n-- libretroControllerOptions: every core x game x steering x pedals\n");
    const char* cores[] = {"swanstation", "pcsx_rearmed", "ppsspp", "mame"};
    int combos = 0, wrong = 0;
    for (const char* core : cores) {
        for (const auto& game : games) {
            for (bool dpad : {false, true}) {
                for (bool buttons : {false, true}) {
                    for (bool vibration : {true, false}) {
                        ControllerSettings c;
                        c.steering = dpad ? "dpad" : "stick";
                        c.pedals = buttons ? "buttons" : "triggers";
                        c.vibration = vibration;
                        c.deadzone = vibration ? 8 : 15;
                        c.players[0] = {"030000005e040000ea02000000007801", "Xbox Wireless Controller"};
                        c.players[1].controller = "none";
                        std::map<std::string, std::string> want = expectedGameKeys(core, game, dpad, buttons);
                        for (auto& [k, v] : expectedCoreKeys(core, vibration)) want[k] = v;
                        want["rrm_input_deadzone"] = std::to_string(c.deadzone);
                        want["rrm_rumble"] = vibration ? "1" : "0";
                        want["rrm_player1"] = c.players[0].controller;
                        want["rrm_player2"] = "none";
                        want["rrm_player3"] = "";
                        want["rrm_player4"] = "";
                        const json got = libretroControllerOptions(core, game, c);
                        std::map<std::string, std::string> have;
                        bool strings = got.is_object();
                        for (auto& [k, v] : got.items()) {
                            strings = strings && v.is_string();
                            if (v.is_string()) have[k] = v.get<std::string>();
                        }
                        ++combos;
                        if (have == want && strings) continue;
                        ++wrong;
                        check(false, "%s / %s / %s / %s / vibration %d: %s", core, game.c_str(), c.steering.c_str(),
                              c.pedals.c_str(), vibration, got.dump().c_str());
                    }
                }
            }
        }
    }
    check(wrong == 0 && combos > 0, "%d combinations match the table, all values strings", combos - wrong);
}

void testControlsInfo() {
    std::printf("\n-- libretroControls (game page)\n");
    ControllerSettings def;
    ControlsInfo rr = libretroControls("swanstation", "ridge-racer", def);
    check(rr.steering && rr.pedals && rr.deadzone && rr.playerChoice && !rr.vibration && rr.players == 1 &&
              !rr.layout.empty() && rr.layout[0].rfind("Left stick: steer (analog neGcon twist)", 0) == 0 &&
              rr.menu == "Guide or Esc: Marathon's pause menu",
          "Ridge Racer: neGcon layout, no vibration, 1 player");
    ControllerSettings ds;
    ds.pedals = "buttons";
    ControlsInfo r4 = libretroControls("pcsx_rearmed", "r4", ds);
    check(r4.vibration && r4.players == 2 && r4.layout[0] == "Left stick: steer (analog)", "R4, pedals on buttons: DualShock, vibration, 2 players");
    ControlsInfo r4n = libretroControls("pcsx_rearmed", "r4", def);
    check(!r4n.vibration && r4n.players == 2, "R4 with the neGcon: no vibration");
    check(r4n.layout.size() == 5 && r4n.layout[3] == "Menus: A (Cross) confirms" &&
              libretroControls("swanstation", "rage-racer", def).layout[3] ==
                  "Menus: A or B (Cross or Circle) confirms, X or Y (Square or Triangle) goes back" &&
              rr.layout[3] == "Menus: B (Circle) confirms, Y (Triangle) goes back",
          "neGcon menu buttons as each game shows them");
    ControllerSettings dig;
    dig.steering = "dpad";
    ControlsInfo rrd = libretroControls("swanstation", "rage-racer", dig);
    check(rrd.layout[0] == "D-pad or left stick: steer (full lock)" &&
              std::find(rrd.layout.begin(), rrd.layout.end(), "RT / LT: accelerate / brake") != rrd.layout.end(),
          "digital steering: digital pad layout with trigger pedals");
    ControlsInfo psp = libretroControls("ppsspp", "ridge-racer-2-psp", def);
    check(psp.steering && psp.pedals && !psp.vibration && psp.note == "The PSP has no analog pedals." &&
              psp.layout[1] == "RT or A (Cross): accelerate; LT or X (Square): brake",
          "PSP: trigger pedals, the note");
    ControlsInfo mame = libretroControls("mame", "rave-racer", def);
    check(!mame.steering && !mame.pedals && !mame.vibration && mame.deadzone && mame.playerChoice && mame.layout.size() == 5,
          "Rave Racer: analog wheel and pedals, only the dead zone applies");
}

std::string readFile(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The real adapter: options merged into launch(), a game's own coreOptions still win.
void testAdapter(const std::string& catalogDir) {
    std::printf("\n-- Libretro adapter launch() (dry run)\n");
    Catalog catalog;
    std::string error;
    const std::string dir = catalogDir + "/";
    if (!catalog.loadFromStrings(readFile(dir + "games.json"), readFile(dir + "backends.json"), error,
                                 readFile(dir + "recomps.json"))) {
        check(false, "catalog %s: %s", catalogDir.c_str(), error.c_str());
        return;
    }
    emu::dryRun() = true;
    const char* ext = currentOs() == Os::Windows ? ".dll" : currentOs() == Os::MacOS ? ".dylib" : ".so";
    Library lib;
    for (const char* id : {"lr-swanstation", "lr-ppsspp"}) {
        const BackendDef* def = catalog.backend(id);
        if (!def) continue;
        BackendInstall bi;
        bi.backendId = id;
        bi.version = "test";
        bi.root = pathToUtf8(paths().cores);
        bi.executable = pathToUtf8(paths().cores / utf8ToPath(def->core + "_libretro" + ext));
        bi.userProvided = true;
        std::ofstream(utf8ToPath(bi.executable)) << "not a core";
        lib.setInstall(bi);
    }
    const fs::path image = paths().userFiles / "test.cue";
    std::ofstream(image) << "FILE \"test.bin\" BINARY\n";
    auto launch = [&](const char* gameId, const char* backendId, const ControllerSettings& c, const json& extra,
                      ControlsInfo* info) {
        LaunchResult r;
        const GameDef* g = catalog.game(gameId);
        const BackendDef* def = catalog.backend(backendId);
        if (!g || !def) return r;
        lib.entry(gameId).files.path = pathToUtf8(image);
        auto b = createBackend(*def);
        LaunchRequest req;
        req.game = g;
        req.backend = def;
        for (const GameBackendOption* o : catalog.optionsFor(*g, currentOs()))
            if (o->backend == backendId) req.option = o;
        req.entry = lib.find(gameId);
        req.library = &lib;
        req.settings.extra = extra;
        req.controllers = c;
        if (info) *info = b->controls(req);
        return b->launch(req);
    };
    ControllerSettings c;
    ControlsInfo info;
    LaunchResult r = launch("r4", "lr-swanstation", c, json::object(), &info);
    check(r.ok && r.coreOptions.value("rrm_port0_device", "") == "negcon" &&
              r.coreOptions.value("rrm_input_dpad_to_stick", "") == "1" &&
              r.coreOptions.value("swanstation_Controller1_TwistResponse", "") == "quadratic" &&
              r.coreOptions.value("swanstation_GPU_Renderer", "") == "Auto",
          "R4 on SwanStation: neGcon options next to the video options%s%s", r.ok ? "" : ": ", r.error.c_str());
    check(info.players == 2 && info.steering && !info.layout.empty(), "controls() through the adapter");
    json extra = {{"coreOptions",
                   {{"rrm_port0_device", "dualshock"}, {"swanstation_Controller1_TwistResponse", "linear"}, {"rrm_rumble", "0"}}}};
    r = launch("r4", "lr-swanstation", c, extra, nullptr);
    check(r.ok && r.coreOptions.value("rrm_port0_device", "") == "dualshock" &&
              r.coreOptions.value("rrm_port1_device", "") == "negcon" &&
              r.coreOptions.value("swanstation_Controller1_TwistResponse", "") == "linear" &&
              r.coreOptions.value("rrm_rumble", "") == "0",
          "the game's advanced coreOptions win over Marathon's");
    ControllerSettings dig;
    dig.steering = "dpad";
    r = launch("ridge-racer-psp", "lr-ppsspp", dig, json::object(), nullptr);
    check(r.ok && r.coreOptions.value("rrm_input_stick_to_dpad", "") == "1" &&
              r.coreOptions.value("rrm_input_r2_as", "") == "b" && r.coreOptions.value("ppsspp_analog_deadzone", "") == "0.0",
          "Ridge Racer PSP on PPSSPP, digital steering%s%s", r.ok ? "" : ": ", r.error.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    std::string catalogDir = "catalog", dataDir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--catalog" && i + 1 < argc) catalogDir = argv[++i];
        else if (a == "--data" && i + 1 < argc) dataDir = argv[++i];
        else {
            std::fprintf(stderr, "usage: lr_input_test [--catalog DIR] [--data DIR]\n");
            return 2;
        }
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::printf("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (dataDir.empty()) dataDir = pathToUtf8(fs::temp_directory_path() / ("rrm-lr-input-" + std::to_string(SDL_GetTicksNS())));
    initPaths(dataDir.c_str());
    std::printf("data folder: %s\n", dataDir.c_str());

    // Two pad models (Xbox Series, DualSense ids), so each has its own GUID.
    VirtualPad a, b;
    if (!a.attach("Marathon test pad A", 0x045E, 0x0B12)) return 1;
    {
        // Every test below starts with pad A as the only pad.
        Input probe;
        probe.open();
        check(probe.connected(0) && !probe.connected(1), "virtual pad found (%s)", describePad(a.id).name.c_str());
        probe.close();
    }
    testTriggers(a);
    testDeadzone(a);
    testStickToDpad(a);
    testDpadToStick(a);
    testRemap(a);
    testAnalogTriggersOff(a);
    testRumble(a);
    testFocus(a);
    if (b.attach("Marathon test pad B", 0x054C, 0x0CE6)) testPorts(a, b);
    testOptionParser();

    std::vector<std::string> games = {"ridge-racer", "rage-racer", "r4", "ridge-racer-psp", "ridge-racer-2-psp",
                                      "rave-racer", "ridge-racer-v", "ridge-racer-6", "ridge-racer-7"};
    testControllerOptions(games);
    testControlsInfo();
    testAdapter(catalogDir);

    a.detach();
    b.detach();
    SDL_Quit();
    std::printf("\n%d of %d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
