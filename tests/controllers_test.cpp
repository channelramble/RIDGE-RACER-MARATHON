// Unit tests for Marathon's controller logic (docs/CONTROLLERS.md):
//   (a) ControllerSettings: defaults, JSON round trip, validation, settings.json v1 -> v2, per-game overrides
//   (b) assignPlayers: automatic order, chosen controllers, "none", sticky slots, identical controllers
//   (c) applyStickDeadzone
//   (d) QuitChordDetector
//   (e) GamepadMonitor with SDL virtual gamepads: plug messages, player slots, name numbering, the chord
//   (f) requestProcessExit / killProcess (macOS and Linux)
//
//   controllers_test            runs everything; exits 1 on any failure
//
// It writes nothing outside a fresh folder under the system temp directory.
#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "core/Controllers.h"
#include "core/Gamepads.h"
#include "core/Process.h"
#include "core/Settings.h"

using namespace rrm;

namespace {

int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failures;                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        ++g_checks;                                                                                        \
        const auto& va_ = (a);                                                                             \
        const auto& vb_ = (b);                                                                             \
        if (!(va_ == vb_)) {                                                                               \
            ++g_failures;                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got: %s\n  want: %s\n", __FILE__, \
                         __LINE__, #a, #b, toStr(va_).c_str(), toStr(vb_).c_str());                       \
        }                                                                                                  \
    } while (0)

std::string toStr(const std::string& s) { return "\"" + s + "\""; }
std::string toStr(const char* s) { return toStr(std::string(s)); }
std::string toStr(const json& j) { return j.dump(); }
std::string toStr(bool b) { return b ? "true" : "false"; }
template <class T>
std::string toStr(const T& v) {
    if constexpr (std::is_enum_v<T>) return std::to_string(static_cast<int>(v));
    else return std::to_string(v);
}

const char* kGuidA = "030000005e040000120b000000000000";
const char* kGuidB = "030000004c050000e60c000000000000";

ConnectedPad pad(uint32_t id, const std::string& guid, const std::string& name = "Pad") {
    ConnectedPad p;
    p.id = id;
    p.guid = guid;
    p.name = name;
    return p;
}

std::array<PlayerChoice, 4> autoChoices() { return {}; }

// ---- (a) settings ------------------------------------------------------------------------------

void testSettings() {
    ControllerSettings d;
    CHECK_EQ(d.steering, "stick");
    CHECK_EQ(d.pedals, "triggers");
    CHECK_EQ(d.vibration, true);
    CHECK_EQ(d.deadzone, 8);
    CHECK_EQ(d.quitChord, "guide-start");
    CHECK_EQ(d.quitHoldMs, 1000);
    for (auto& p : d.players) CHECK(p.controller.empty() && p.name.empty());
    CHECK(ControllerSettings::fromJson(json::object()) == d);
    CHECK(ControllerSettings::fromJson(json()) == d);

    // Round trip.
    ControllerSettings c;
    c.steering = "dpad";
    c.pedals = "buttons";
    c.vibration = false;
    c.deadzone = 23;
    c.players[0] = {kGuidA, "Xbox Series X Controller"};
    c.players[1] = {"none", ""};
    c.quitChord = "back-start";
    c.quitHoldMs = 2500;
    CHECK(ControllerSettings::fromJson(c.toJson()) == c);
    CHECK(ControllerSettings::fromJson(json::parse(c.toJson().dump())) == c);
    CHECK_EQ(c.toJson()["players"].size(), size_t(4));

    // Validation.
    json bad = {{"steering", "wheel"},  {"pedals", 3},       {"vibration", "yes"},     {"deadzone", 75},
                {"quitChord", "guide"}, {"quitHoldMs", 100}, {"players", json::array({{{"controller", "0300ABCDEF00000000000000000000FF"}, {"name", "Upper"}},
                                                                                       {{"controller", "xyz"}, {"name", "Bad"}},
                                                                                       {{"controller", "none"}},
                                                                                       {{"controller", "0300"}},
                                                                                       {{"controller", kGuidB}}})}};
    ControllerSettings v = ControllerSettings::fromJson(bad);
    CHECK_EQ(v.steering, "stick");
    CHECK_EQ(v.pedals, "triggers");
    CHECK_EQ(v.vibration, true);
    CHECK_EQ(v.deadzone, 40);
    CHECK_EQ(v.quitChord, "guide-start");
    CHECK_EQ(v.quitHoldMs, 500);
    CHECK_EQ(v.players[0].controller, "0300abcdef00000000000000000000ff");  // lower-cased
    CHECK_EQ(v.players[0].name, "Upper");
    CHECK_EQ(v.players[1].controller, "");                                  // not a GUID: automatic
    CHECK_EQ(v.players[2].controller, "none");
    CHECK_EQ(v.players[3].controller, "");                                  // too short
    CHECK_EQ(ControllerSettings::fromJson({{"deadzone", -5}, {"quitHoldMs", 99999}}).deadzone, 0);
    CHECK_EQ(ControllerSettings::fromJson({{"quitHoldMs", 99999}}).quitHoldMs, 3000);
    const ControllerSettings oneEntry = ControllerSettings::fromJson({{"players", json::array({{{"controller", "none"}}})}});
    CHECK_EQ(oneEntry.players[0].controller, "none");
    CHECK_EQ(oneEntry.players[1].controller, "");  // missing entries are automatic
    const ControllerSettings notList = ControllerSettings::fromJson({{"players", "nope"}});
    CHECK_EQ(notList.players[0].controller, "");

    // settings.json version 1 (no "version", no "controllers") loads the defaults and saves as 2.
    json v1 = {{"defaults", {{"resolution", "4k"}}}, {"uiScale", 1.25}, {"showUnsupported", false}};
    GlobalSettings g = GlobalSettings::fromJson(v1);
    CHECK_EQ(g.version, 1);
    CHECK(g.controllers == ControllerSettings());
    CHECK_EQ(g.defaults.resolution, "4k");
    json saved = g.toJson();
    CHECK_EQ(saved["version"], json(2));
    CHECK(saved["controllers"].is_object());
    CHECK_EQ(saved["controllers"]["steering"], json("stick"));
    GlobalSettings g2 = GlobalSettings::fromJson(saved);
    CHECK_EQ(g2.version, 2);
    CHECK(g2.controllers == g.controllers);
    CHECK_EQ(g2.uiScale, 1.25f);
    g2.controllers = c;
    CHECK(GlobalSettings::fromJson(g2.toJson()).controllers == c);

    // Per-game overrides: steering, pedals, vibration and dead zone only.
    json extra = {{"controllers",
                   {{"steering", "dpad"}, {"pedals", "buttons"}, {"vibration", false}, {"deadzone", 55},
                    {"players", json::array({{{"controller", "none"}}})}, {"quitChord", "off"}, {"quitHoldMs", 3000}}},
                  {"cfg", {{"x", 1}}}};
    ControllerSettings base;
    base.players[0] = {kGuidA, "A"};
    ControllerSettings o = base.withOverrides(extra);
    CHECK_EQ(o.steering, "dpad");
    CHECK_EQ(o.pedals, "buttons");
    CHECK_EQ(o.vibration, false);
    CHECK_EQ(o.deadzone, 40);
    CHECK_EQ(o.players[0].controller, kGuidA);  // players stay global
    CHECK_EQ(o.quitChord, "guide-start");        // the quit chord too
    CHECK_EQ(o.quitHoldMs, 1000);
    // Partial and invalid overrides keep the global value.
    base.steering = "dpad";
    base.deadzone = 12;
    ControllerSettings p = base.withOverrides({{"controllers", {{"pedals", "buttons"}, {"steering", "wheel"}}}});
    CHECK_EQ(p.steering, "dpad");
    CHECK_EQ(p.pedals, "buttons");
    CHECK_EQ(p.deadzone, 12);
    CHECK(base.withOverrides(json::object()) == base);
    CHECK(base.withOverrides({{"controllers", "stick"}}) == base);
    CHECK(base.withOverrides(json()) == base);
}

// ---- (b) player slots --------------------------------------------------------------------------

void testAssign() {
    using Slots = std::array<uint32_t, 4>;
    // Automatic: connection order.
    std::vector<ConnectedPad> pads = {pad(11, kGuidA), pad(12, kGuidB), pad(13, kGuidA)};
    CHECK(assignPlayers(pads, autoChoices()) == (Slots{11, 12, 13, 0}));

    // A chosen model takes Player 1 even though it connected second.
    auto choices = autoChoices();
    choices[0].controller = kGuidB;
    CHECK(assignPlayers(pads, choices) == (Slots{12, 11, 13, 0}));

    // "none" leaves the slot empty and the pads move on.
    choices = autoChoices();
    choices[1].controller = "none";
    CHECK(assignPlayers(pads, choices) == (Slots{11, 0, 12, 13}));

    // Sticky: Player 1 leaves, Player 2 keeps its pad and Player 1 stays empty...
    std::vector<ConnectedPad> two = {pad(1, kGuidA), pad(2, kGuidB)};
    Slots s = assignPlayers(two, autoChoices());
    CHECK(s == (Slots{1, 2, 0, 0}));
    std::vector<ConnectedPad> onlyB = {pad(2, kGuidB)};
    s = assignPlayers(onlyB, autoChoices(), s);
    CHECK(s == (Slots{0, 2, 0, 0}));
    // ... and a returning pad (new joystick id, now last in connection order) fills Player 1.
    std::vector<ConnectedPad> back = {pad(2, kGuidB), pad(3, kGuidA)};
    s = assignPlayers(back, autoChoices(), s);
    CHECK(s == (Slots{3, 2, 0, 0}));
    applyAssignment(back, s);
    CHECK_EQ(back[0].player, 1);
    CHECK_EQ(back[1].player, 0);

    // Two identical controllers chosen for both players follow connection order.
    choices = autoChoices();
    choices[0].controller = kGuidA;
    choices[1].controller = kGuidA;
    std::vector<ConnectedPad> twins = {pad(5, kGuidA, "Twin"), pad(6, kGuidA, "Twin")};
    CHECK(assignPlayers(twins, choices) == (Slots{5, 6, 0, 0}));

    // A chosen controller that arrives later bumps the automatic holder to the next free slot.
    choices = autoChoices();
    choices[0].controller = kGuidB;
    std::vector<ConnectedPad> early = {pad(7, kGuidA)};
    s = assignPlayers(early, choices);
    CHECK(s == (Slots{7, 0, 0, 0}));  // nobody of that model yet: Player 1 is filled automatically
    std::vector<ConnectedPad> later = {pad(7, kGuidA), pad(8, kGuidB)};
    s = assignPlayers(later, choices, s);
    CHECK(s == (Slots{8, 7, 0, 0}));

    // A holder that is no longer connected is dropped; a stale duplicate is not kept twice.
    s = assignPlayers({pad(9, kGuidA)}, autoChoices(), Slots{4, 9, 9, 0});
    CHECK(s == (Slots{0, 9, 0, 0}));  // Player 2 keeps it (rule 2), Player 1 has nothing left
    // No pads: nobody.
    CHECK(assignPlayers({}, autoChoices(), Slots{1, 2, 3, 4}) == (Slots{0, 0, 0, 0}));
}

// ---- (c) dead zone -----------------------------------------------------------------------------

void testDeadzone() {
    const int dz = deadzoneUnits(8);
    CHECK_EQ(dz, 2621);
    CHECK_EQ(deadzoneUnits(0), 0);
    CHECK_EQ(deadzoneUnits(100), 32767);
    auto run = [dz](int x, int y) {
        int16_t ox = static_cast<int16_t>(x), oy = static_cast<int16_t>(y);
        applyStickDeadzone(ox, oy, dz);
        return std::pair<int, int>(ox, oy);
    };
    CHECK(run(1638, 0) == std::make_pair(0, 0));          // 5 % at an 8 % dead zone
    CHECK(run(0, -1638) == std::make_pair(0, 0));
    CHECK(run(32767, 0) == std::make_pair(32767, 0));     // full deflection still reaches full
    CHECK(run(0, -32768).second == -32768);
    auto half = run(17694, 0);                            // 54 % of travel
    CHECK(std::abs(half.first - 16384) < 200);            // about 50 %
    CHECK_EQ(half.second, 0);
    // Radial: each axis alone is inside the dead zone, the diagonal is not.
    auto diag = run(2000, 2000);
    CHECK(diag.first > 0 && diag.second > 0);
    CHECK(run(1800, 1800) == std::make_pair(0, 0));
    auto d45 = run(16384, 16384);
    CHECK_EQ(d45.first, d45.second);                      // direction kept
    CHECK(d45.first < 16384);
    // Zero dead zone changes nothing.
    int16_t x = 123, y = -456;
    applyStickDeadzone(x, y, 0);
    CHECK(x == 123 && y == -456);
}

// ---- (d) quit chord ----------------------------------------------------------------------------

ChordButtons buttons(bool guide, bool back, bool start, bool hasGuide = true) {
    ChordButtons b;
    b.guide = guide;
    b.back = back;
    b.start = start;
    b.hasGuide = hasGuide;
    return b;
}

void testChord() {
    QuitChordDetector q;
    q.configure(QuitChordMode::GuideStart, 1000);
    const auto held = buttons(true, false, true);
    const auto none = buttons(false, false, false);
    CHECK(!q.update(1, held, 5000));
    CHECK_EQ(q.progress(5000), 0.0f);
    CHECK(!q.update(1, held, 5500));
    CHECK(std::fabs(q.progress(5500) - 0.5f) < 1e-4f);
    CHECK(!q.update(1, held, 5999));                       // holdMs - 1: not yet
    CHECK(q.update(1, held, 6000));                        // exactly holdMs
    CHECK(!q.update(1, held, 6001));                       // only once
    CHECK(!q.update(1, held, 9000));
    CHECK_EQ(q.progress(9000), 0.0f);                      // fired: nothing to show
    // Release and press again re-arms.
    CHECK(!q.update(1, none, 9100));
    CHECK(!q.update(1, held, 9200));
    CHECK(q.update(1, held, 10200));
    // Letting go of Guide for one frame restarts the hold.
    CHECK(!q.update(1, none, 11000));
    CHECK(!q.update(1, held, 11000));
    CHECK(!q.update(1, buttons(false, false, true), 11500));
    CHECK(!q.update(1, held, 11600));
    CHECK(!q.update(1, held, 12500));
    CHECK(q.update(1, held, 12600));
    // Without a Guide button, Back + Start works...
    QuitChordDetector ng;
    ng.configure(QuitChordMode::GuideStart, 800);
    CHECK(!ng.update(2, buttons(false, true, true, false), 0));
    CHECK(ng.update(2, buttons(false, true, true, false), 800));
    // ... but with one, Back + Start does nothing in Guide + Start mode.
    QuitChordDetector g;
    g.configure(QuitChordMode::GuideStart, 800);
    CHECK(!g.update(3, buttons(false, true, true), 0));
    CHECK(!g.update(3, buttons(false, true, true), 5000));
    // Back + Start mode.
    QuitChordDetector bs;
    bs.configure(QuitChordMode::BackStart, 500);
    CHECK(!bs.update(4, buttons(true, false, true), 0));
    CHECK(!bs.update(4, buttons(true, false, true), 600));  // Guide + Start is not it
    CHECK(!bs.update(4, buttons(false, true, true), 700));
    CHECK(bs.update(4, buttons(false, true, true), 1200));
    // Off never fires.
    QuitChordDetector off;
    off.configure(QuitChordMode::Off, 500);
    CHECK(!off.update(5, buttons(true, true, true), 0));
    CHECK(!off.update(5, buttons(true, true, true), 100000));
    CHECK_EQ(off.progress(100000), 0.0f);
    // reset() while held: no firing until released and pressed again.
    QuitChordDetector r;
    r.configure(QuitChordMode::GuideStart, 1000);
    CHECK(!r.update(6, held, 0));
    r.reset();
    CHECK(!r.update(6, held, 1000));
    CHECK(!r.update(6, held, 5000));
    CHECK(!r.update(6, none, 5100));
    CHECK(!r.update(6, held, 5200));
    CHECK(r.update(6, held, 6200));
    // Two pads are independent; progress follows the one closest to firing.
    QuitChordDetector two;
    two.configure(QuitChordMode::GuideStart, 1000);
    CHECK(!two.update(7, held, 0));
    CHECK(!two.update(8, none, 0));
    CHECK(!two.update(8, held, 400));
    CHECK(std::fabs(two.progress(600) - 0.6f) < 1e-4f);
    CHECK(two.update(7, held, 1000));
    CHECK(!two.update(8, held, 1000));
    CHECK(std::fabs(two.progress(1000) - 0.6f) < 1e-4f);  // pad 7 fired; pad 8 is at 60 %
    CHECK(two.update(8, held, 1400));
    two.forget(8);
    CHECK_EQ(two.progress(1400), 0.0f);
    CHECK(quitChordMode("back-start") == QuitChordMode::BackStart);
    CHECK(quitChordMode("off") == QuitChordMode::Off);
    CHECK(quitChordMode("guide-start") == QuitChordMode::GuideStart);
    CHECK(quitChordMode("???") == QuitChordMode::GuideStart);
}

// ---- (e) GamepadMonitor with virtual gamepads ------------------------------------------------------

struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* js = nullptr;
    int rumbles = 0;
    Uint16 low = 0, high = 0;
};

bool SDLCALL onRumble(void* user, Uint16 low, Uint16 high) {
    auto* v = static_cast<VirtualPad*>(user);
    if (low || high) ++v->rumbles;
    v->low = low;
    v->high = high;
    return true;
}

VirtualPad* attach(const char* name, Uint16 vendor, Uint16 product) {
    auto* v = new VirtualPad();
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.name = name;
    desc.Rumble = onRumble;
    desc.userdata = v;
    v->id = SDL_AttachVirtualJoystick(&desc);
    if (!v->id) std::fprintf(stderr, "attach %s: %s\n", name, SDL_GetError());
    v->js = SDL_OpenJoystick(v->id);
    // Triggers of a virtual gamepad use the full range: -32768 is released.
    SDL_SetJoystickVirtualAxis(v->js, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    SDL_SetJoystickVirtualAxis(v->js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    return v;
}

void detach(VirtualPad* v) {
    SDL_CloseJoystick(v->js);
    SDL_DetachVirtualJoystick(v->id);
    delete v;
}

void pump(GamepadMonitor& m) {
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) m.handleEvent(e);
}

int count(const std::vector<PadEvent>& ev, PadEvent::Kind kind) {
    int n = 0;
    for (auto& e : ev) n += e.kind == kind;
    return n;
}

void testMonitor() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        ++g_failures;
        return;
    }
    VirtualPad* xbox = attach("Marathon Test Xbox", 0x045E, 0x0B12);
    SDL_UpdateJoysticks();
    GamepadMonitor m;
    m.open();
    ControllerSettings c;
    uint64_t t = 100000;
    pump(m);
    auto ev = m.update(t, c);
    CHECK(ev.empty());  // present at open(): no message
    auto pads = m.pads();
    CHECK_EQ(pads.size(), size_t(1));
    if (pads.size() == 1) {
        CHECK_EQ(pads[0].player, 0);
        CHECK_EQ(pads[0].type, "xboxone");
        CHECK_EQ(padFamily(pads[0].type), "xbox");
        CHECK_EQ(padTypeLabel(pads[0].type), "Xbox One / Series");
        CHECK_EQ(pads[0].guid.size(), size_t(32));
        CHECK(pads[0].virtualPad);
        CHECK(pads[0].hasGuide);
        CHECK(pads[0].hasRumble);
        CHECK_EQ(pads[0].vendor, uint16_t(0x045E));
        CHECK_EQ(pads[0].nameIndex, 1);
    }
    CHECK_EQ(m.lastUsed(), uint32_t(xbox->id));
    CHECK(m.gamepad(xbox->id) != nullptr);

    // Plugged in while starting up (first 1.5 s): no message, but it gets Player 2.
    VirtualPad* ds = attach("Marathon Test DualSense", 0x054C, 0x0CE6);
    pump(m);
    ev = m.update(t + 1000, c);
    CHECK(ev.empty());
    pads = m.pads();
    CHECK_EQ(pads.size(), size_t(2));
    if (pads.size() == 2) {
        CHECK_EQ(pads[1].player, 1);
        CHECK_EQ(padFamily(pads[1].type), "playstation");
        CHECK_EQ(padTypeLabel(pads[1].type), "DualSense");
    }
    CHECK_EQ(m.playerPad(1), uint32_t(ds->id));

    // After start-up: unplug is reported with its player slot.
    std::string dsGuid = pads.size() == 2 ? pads[1].guid : "";
    detach(ds);
    pump(m);
    ev = m.update(t + 3000, c);
    CHECK_EQ(count(ev, PadEvent::Kind::Disconnected), 1);
    if (!ev.empty()) {
        CHECK_EQ(ev[0].pad.name, "Marathon Test DualSense");
        CHECK_EQ(ev[0].pad.player, 1);
    }
    CHECK_EQ(m.playerPad(1), uint32_t(0));
    // Back within 2 s: held back, then reported once the 2 s have passed.
    ds = attach("Marathon Test DualSense", 0x054C, 0x0CE6);
    pump(m);
    ev = m.update(t + 3500, c);
    CHECK(ev.empty());
    ev = m.update(t + 4999, c);
    CHECK(ev.empty());
    ev = m.update(t + 5000, c);
    CHECK_EQ(count(ev, PadEvent::Kind::Connected), 1);
    if (!ev.empty()) CHECK_EQ(ev[0].pad.player, 1);
    // Bluetooth flapping: out and in again within 2 s of the last message says nothing.
    detach(ds);
    pump(m);
    CHECK(m.update(t + 5500, c).empty());
    ds = attach("Marathon Test DualSense", 0x054C, 0x0CE6);
    pump(m);
    CHECK(m.update(t + 6000, c).empty());
    CHECK(m.update(t + 9000, c).empty());
    CHECK_EQ(m.pads().size(), size_t(2));

    // Two controllers with the same name are numbered in connection order.
    VirtualPad* xbox2 = attach("Marathon Test Xbox", 0x045E, 0x0B12);
    pump(m);
    ev = m.update(t + 12000, c);
    CHECK_EQ(count(ev, PadEvent::Kind::Connected), 1);
    pads = m.pads();
    CHECK_EQ(pads.size(), size_t(3));
    if (pads.size() == 3) {
        CHECK_EQ(pads[0].nameIndex, 1);
        CHECK_EQ(pads[2].nameIndex, 2);
        CHECK_EQ(pads[2].player, 2);
        CHECK_EQ(pads[1].nameIndex, 1);
    }
    if (!ev.empty()) CHECK_EQ(ev[0].pad.player, 2);

    // Choosing the DualSense model for Player 1 moves it there on the next update.
    ControllerSettings chosen = c;
    chosen.players[0] = {dsGuid, "Marathon Test DualSense"};
    m.update(t + 12100, chosen);
    CHECK_EQ(m.playerPad(0), uint32_t(ds->id));
    CHECK_EQ(m.playerPad(1), uint32_t(xbox->id));   // the Xbox lost Player 1 and takes the lowest free slot
    CHECK_EQ(m.playerPad(2), uint32_t(xbox2->id));  // holders never move
    m.update(t + 12200, c);

    // Quit chord: Guide + Start held for quitHoldMs (synthetic time), then a confirmation pulse.
    SDL_SetJoystickVirtualButton(xbox->js, SDL_GAMEPAD_BUTTON_GUIDE, true);
    SDL_SetJoystickVirtualButton(xbox->js, SDL_GAMEPAD_BUTTON_START, true);
    pump(m);
    const uint64_t c0 = t + 20000;
    CHECK_EQ(count(m.update(c0, c), PadEvent::Kind::QuitChord), 0);
    CHECK(std::fabs(m.chordProgress(c0 + 500) - 0.5f) < 1e-3f);
    CHECK_EQ(count(m.update(c0 + 999, c), PadEvent::Kind::QuitChord), 0);
    ev = m.update(c0 + 1000, c);
    CHECK_EQ(count(ev, PadEvent::Kind::QuitChord), 1);
    if (!ev.empty()) CHECK_EQ(ev[0].pad.id, uint32_t(xbox->id));
    SDL_UpdateJoysticks();
    CHECK_EQ(xbox->rumbles, 1);
    CHECK_EQ(xbox->low, Uint16(0x6666));
    CHECK_EQ(count(m.update(c0 + 3000, c), PadEvent::Kind::QuitChord), 0);  // once per hold
    // resetChord() while held (a game starting): nothing until released and held again.
    SDL_SetJoystickVirtualButton(xbox->js, SDL_GAMEPAD_BUTTON_START, false);
    pump(m);
    m.update(c0 + 3100, c);
    SDL_SetJoystickVirtualButton(xbox->js, SDL_GAMEPAD_BUTTON_START, true);
    pump(m);
    m.update(c0 + 3200, c);
    m.resetChord();
    CHECK_EQ(count(m.update(c0 + 6000, c), PadEvent::Kind::QuitChord), 0);
    // Without vibration there is no pulse; with Back + Start mode Guide + Start does nothing.
    SDL_SetJoystickVirtualButton(xbox->js, SDL_GAMEPAD_BUTTON_GUIDE, false);
    SDL_SetJoystickVirtualButton(xbox->js, SDL_GAMEPAD_BUTTON_START, false);
    pump(m);
    m.update(c0 + 6100, c);
    ControllerSettings quiet = c;
    quiet.vibration = false;
    quiet.quitChord = "back-start";
    quiet.quitHoldMs = 500;
    SDL_SetJoystickVirtualButton(ds->js, SDL_GAMEPAD_BUTTON_BACK, true);
    SDL_SetJoystickVirtualButton(ds->js, SDL_GAMEPAD_BUTTON_START, true);
    pump(m);
    CHECK_EQ(count(m.update(c0 + 7000, quiet), PadEvent::Kind::QuitChord), 0);
    ev = m.update(c0 + 7500, quiet);
    CHECK_EQ(count(ev, PadEvent::Kind::QuitChord), 1);
    if (!ev.empty()) CHECK_EQ(ev[0].pad.id, uint32_t(ds->id));
    SDL_UpdateJoysticks();
    CHECK_EQ(ds->rumbles, 0);
    SDL_SetJoystickVirtualButton(ds->js, SDL_GAMEPAD_BUTTON_BACK, false);
    SDL_SetJoystickVirtualButton(ds->js, SDL_GAMEPAD_BUTTON_START, false);
    pump(m);

    // Last used: a button press, or an axis past 50 %.
    SDL_SetJoystickVirtualAxis(ds->js, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    pump(m);
    CHECK_EQ(m.lastUsed(), uint32_t(ds->id));
    SDL_SetJoystickVirtualButton(xbox2->js, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump(m);
    CHECK_EQ(m.lastUsed(), uint32_t(xbox2->id));
    SDL_SetJoystickVirtualButton(xbox2->js, SDL_GAMEPAD_BUTTON_SOUTH, false);

    // Rumble through the monitor (the Controllers page's Vibrate button).
    CHECK(m.rumble(xbox->id, 0x9000, 0x9000, 400));
    SDL_UpdateJoysticks();
    CHECK_EQ(xbox->high, Uint16(0x9000));

    // connectedPads(): the same list straight from SDL.
    auto direct = connectedPads();
    CHECK_EQ(direct.size(), size_t(3));
    if (direct.size() == 3) {
        CHECK_EQ(direct[2].nameIndex, 2);
        CHECK_EQ(direct[0].player, -1);
    }

    m.close();
    CHECK(m.pads().empty());
    detach(xbox);
    detach(xbox2);
    detach(ds);
    SDL_Quit();
}

// ---- (f) process exit --------------------------------------------------------------------------------

#if !defined(_WIN32)
bool waitGone(int64_t pid, int ms) {
    for (int i = 0; i < ms / 20; ++i) {
        if (!processRunning(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !processRunning(pid);
}
#endif

// POSIX only: Windows asks windows to close (WM_CLOSE), which needs a program with a window.
void testProcessExit() {
#if !defined(_WIN32)
    std::string error;
    ProcessSpec polite;
    polite.argv = {"/bin/sh", "-c", "exec sleep 30"};
    int64_t pid = spawnProcess(polite, error);
    CHECK(pid > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(processRunning(pid));
    CHECK(requestProcessExit(pid));
    CHECK(waitGone(pid, 2000));
    CHECK(!requestProcessExit(pid));  // already gone: nothing to ask

    ProcessSpec stubborn;
    stubborn.argv = {"/bin/sh", "-c", "trap \"\" TERM; while :; do sleep 1; done"};
    pid = spawnProcess(stubborn, error);
    CHECK(pid > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(requestProcessExit(pid));
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    CHECK(processRunning(pid));  // ignores SIGTERM
    killProcess(pid);
    CHECK(waitGone(pid, 1000));
#endif
}

}  // namespace

int main() {
    testSettings();
    testAssign();
    testDeadzone();
    testChord();
    testMonitor();
    testProcessExit();
    std::printf("controllers_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
