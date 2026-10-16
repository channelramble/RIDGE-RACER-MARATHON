// controller_configs_test: the controller configuration Marathon gives the external backends
// (src/backends/ControllerConfigs.*, the adapters, docs/CONTROLLERS.md).
//
//   controller_configs_test [--catalog DIR] [--work DIR]
//
//  1. cfg::Ini::setList / getList / replaceSection: order, other lines kept, idempotence, keys
//     with spaces, CRLF.
//  2. Every generator for steering x pedals x vibration (and the PPSSPP OS variants), compared
//     with the exact text or keys it must produce.
//  3. Dry runs of the real adapters with fake installs in a throwaway folder (default
//     /tmp/rrm-ctl-configs-<n>; HOME, CFFIXED_USER_HOME and XDG_* point inside it too): the files
//     written, the command lines and the "controllers:" log lines. Nothing is started.
//
// --catalog defaults to ./catalog, else the catalog beside this source file.
//
// Real programs (manual, not part of this test; no physical controller was available):
//  - RPCS3: the official macOS build, HOME=/tmp/<folder>, Marathon's firmware and a Ridge Racer 7
//    disc, through `launch_tool matrix <games> --only ridge-racer-7 --link rpcs3=<RPCS3.app>
//    --pad "Xbox Series X Controller" --spawn 30`. RPCS3.log must show "Loading input configuration:
//    .../input_configs/global/Default.yml", "Pad 0: device='Xbox Series X Controller 1',
//    handler=SDL", "Pad 6: device='Keyboard', handler=Keyboard, ... class_type=0xa004" and
//    Marathon's bindings in the dumped config.
//  - PPSSPP: only with HOME and CFFIXED_USER_HOME both under /tmp (it also reads macOS
//    preferences); controls.ini must keep [Marathon] and the mapping after PPSSPP exits.
//  - PCSX2 (needs Rosetta on Apple silicon), Xenia (no macOS build) and MAME (no binary here)
//    were checked against their source only.
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "backends/Backend.h"
#include "backends/ControllerConfigs.h"
#include "backends/EmuCommon.h"
#include "core/Catalog.h"
#include "core/ConfigFiles.h"
#include "core/Controllers.h"
#include "core/Library.h"
#include "core/Platform.h"

using namespace rrm;
using Strings = std::vector<std::string>;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const std::string& what, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL (line %d): %s\n", line, what.c_str());
}
#define CHECK(cond) check((cond), #cond, __LINE__)

// Exact text: on a mismatch, the first line that differs.
void checkText(const std::string& got, const std::string& want, const std::string& what, int line) {
    ++g_checks;
    if (got == want) return;
    ++g_failures;
    std::istringstream a(got), b(want);
    std::string la, lb;
    int n = 1;
    while (true) {
        const bool ea = !std::getline(a, la), eb = !std::getline(b, lb);
        if (ea && eb) break;
        if (ea || eb || la != lb) {
            std::printf("FAIL (line %d): %s: line %d is\n    \"%s\"\n  expected\n    \"%s\"\n", line, what.c_str(), n,
                        ea ? "<end>" : la.c_str(), eb ? "<end>" : lb.c_str());
            return;
        }
        ++n;
    }
    std::printf("FAIL (line %d): %s: differs (line endings)\n", line, what.c_str());
}
#define CHECK_TEXT(got, want) checkText((got), (want), #got, __LINE__)

template <typename T>
std::string show(const std::vector<T>& v) {
    std::string out = "{";
    for (auto& x : v) out += (out.size() > 1 ? ", " : "") + std::string("\"") + x + "\"";
    return out + "}";
}

void checkList(const Strings& got, const Strings& want, const std::string& what, int line) {
    check(got == want, what + " = " + show(got) + ", expected " + show(want), line);
}
#define CHECK_LIST(got, ...) checkList((got), Strings{__VA_ARGS__}, #got, __LINE__)

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
#define CHECK_HAS(text, part) check(contains((text), (part)), std::string(#text) + " contains \"" + (part) + "\"", __LINE__)
#define CHECK_LACKS(text, part) check(!contains((text), (part)), std::string(#text) + " lacks \"" + (part) + "\"", __LINE__)

ControllerSettings settings(const std::string& steering, const std::string& pedals, bool vibration, int deadzone = 8) {
    ControllerSettings c;
    c.steering = steering;
    c.pedals = pedals;
    c.vibration = vibration;
    c.deadzone = deadzone;
    return c;
}

ConnectedPad pad(uint32_t id, const std::string& name, const std::string& guid, int nameIndex = 1) {
    ConnectedPad p;
    p.id = id;
    p.name = name;
    p.guid = guid;
    p.type = "xboxone";
    p.nameIndex = nameIndex;
    return p;
}

std::vector<ConnectedPad> assigned(std::vector<ConnectedPad> pads, const ControllerSettings& c) {
    applyAssignment(pads, assignPlayers(pads, c.players));
    return pads;
}

const std::string kXbox = "Xbox Series X Controller";
const std::string kGuidA = "0300509d5e040000120b000011050000";
const std::string kGuidB = "050000004c050000e60c000000810000";

// ---- 1. cfg::Ini ---------------------------------------------------------------------------------

void testIni() {
    cfg::Ini ini;
    ini.parse("; PCSX2 settings\n[Pad1]\nType = DualShock2\nCross = Keyboard/K\nSquare = Keyboard/J\n"
              "Cross = SDL-0/FaceSouth\n\n[Pad2]\nCross = Keyboard/K\n");
    CHECK_LIST(ini.getList("Pad1", "Cross"), "Keyboard/K", "SDL-0/FaceSouth");
    CHECK_LIST(ini.getList("pad1", "CROSS"), "Keyboard/K", "SDL-0/FaceSouth");  // case-insensitive like get()
    CHECK_LIST(ini.getList("Pad2", "Cross"), "Keyboard/K");
    CHECK_LIST(ini.getList("Pad3", "Cross"));
    ini.setList("Pad1", "Cross", {"Keyboard/K", "SDL-0/FaceSouth", "SDL-0/+RightTrigger"});
    const std::string once = "; PCSX2 settings\n[Pad1]\nType = DualShock2\nCross = Keyboard/K\nCross = SDL-0/FaceSouth\n"
                             "Cross = SDL-0/+RightTrigger\nSquare = Keyboard/J\n\n[Pad2]\nCross = Keyboard/K\n";
    CHECK_TEXT(ini.text(), once);
    ini.setList("Pad1", "Cross", {"Keyboard/K", "SDL-0/FaceSouth", "SDL-0/+RightTrigger"});
    CHECK_TEXT(ini.text(), once);  // idempotent
    ini.setList("Pad1", "L2", {"SDL-0/+LeftTrigger"});  // a new key goes to the end of its section
    CHECK_TEXT(ini.text(), "; PCSX2 settings\n[Pad1]\nType = DualShock2\nCross = Keyboard/K\nCross = SDL-0/FaceSouth\n"
                           "Cross = SDL-0/+RightTrigger\nSquare = Keyboard/J\nL2 = SDL-0/+LeftTrigger\n\n[Pad2]\nCross = Keyboard/K\n");
    ini.setList("Pad1", "Cross", {});
    ini.setList("Pad1", "L2", {});
    CHECK_TEXT(ini.text(), "; PCSX2 settings\n[Pad1]\nType = DualShock2\nSquare = Keyboard/J\n\n[Pad2]\nCross = Keyboard/K\n");
    ini.setList("Hotkeys", "OpenPauseMenu", {"Keyboard/Escape", "SDL-0/Back & SDL-0/Start"});
    CHECK_TEXT(ini.text(), "; PCSX2 settings\n[Pad1]\nType = DualShock2\nSquare = Keyboard/J\n\n[Pad2]\nCross = Keyboard/K\n"
                           "\n[Hotkeys]\nOpenPauseMenu = Keyboard/Escape\nOpenPauseMenu = SDL-0/Back & SDL-0/Start\n");
    CHECK(ini.hasSection("hotkeys") && !ini.hasSection("Pad3"));

    // "Key=Value" files keep their style; CRLF files keep their line endings.
    cfg::Ini tight;
    tight.parse("[S]\nk=1\n");
    tight.setList("S", "k", {"a", "b"});
    CHECK_TEXT(tight.text(), "[S]\nk=a\nk=b\n");
    cfg::Ini crlf;
    crlf.parse("[S]\r\nk = 1\r\n");
    crlf.setList("S", "k", {"a", "b"});
    CHECK(crlf.text() == "[S]\r\nk = a\r\nk = b\r\n");

    // replaceSection: keys with spaces and parentheses, other sections untouched, idempotent.
    cfg::Ini controls;
    controls.parse("[General]\nA = 1\n\n[ControlMapping]\n; old mapping\nUp = 1-19\nPause (no menu) = 1-999\n\n"
                   "[Marathon]\nProfile = x\n");
    controls.replaceSection("ControlMapping", {{"Pause (no menu)", "1-138"}, {"An.Up", "1-37,10-4003"}});
    const std::string replaced = "[General]\nA = 1\n\n[ControlMapping]\nPause (no menu) = 1-138\nAn.Up = 1-37,10-4003\n\n"
                                 "[Marathon]\nProfile = x\n";
    CHECK_TEXT(controls.text(), replaced);
    CHECK_LIST(controls.getList("ControlMapping", "Pause (no menu)"), "1-138");
    CHECK(controls.get("ControlMapping", "An.Up") == "1-37,10-4003");
    controls.replaceSection("ControlMapping", {{"Pause (no menu)", "1-138"}, {"An.Up", "1-37,10-4003"}});
    CHECK_TEXT(controls.text(), replaced);
    cfg::Ini missing;
    missing.parse("[A]\nx = 1\n");
    missing.replaceSection("ControlMapping", {{"Up", "1-19"}});
    CHECK_TEXT(missing.text(), "[A]\nx = 1\n\n[ControlMapping]\nUp = 1-19\n");
    cfg::Ini repeated;
    repeated.parse("[C]\na = 1\n[D]\nb = 2\n[C]\nc = 3\n");
    repeated.replaceSection("C", {{"z", "9"}});
    CHECK_TEXT(repeated.text(), "[C]\nz = 9\n\n[D]\nb = 2\n");
    cfg::Ini last;
    last.parse("[A]\nx = 1\n\n[ControlMapping]\nUp = 1\n");
    last.replaceSection("ControlMapping", {{"Up", "2"}, {"Down", "3"}});
    CHECK_TEXT(last.text(), "[A]\nx = 1\n\n[ControlMapping]\nUp = 2\nDown = 3\n");
}

// ---- 2. generators -------------------------------------------------------------------------------

const char* kRpcs3Header =
    "# Written by Ridge Racer Marathon before each launch, from its controller settings.\n"
    "# Keys that are not listed take RPCS3's defaults for the handler.\n";

std::string nullPlayers(int from, int to) {
    std::string out;
    for (int i = from; i <= to; ++i) out += "Player " + std::to_string(i) + " Input:\n  Handler: \"Null\"\n";
    return out;
}

// Ridge Racer Marathon's default layout (stick, triggers, vibration, 8 %) for one player.
std::string rpcs3DefaultConfig(int deadzoneUnitsValue) {
    const std::string dz = std::to_string(deadzoneUnitsValue);
    return "  Config:\n"
           "    Left Stick Left: LS X-\n"
           "    Left Stick Down: LS Y-\n"
           "    Left Stick Right: LS X+\n"
           "    Left Stick Up: LS Y+\n"
           "    Right Stick Left: RS X-\n"
           "    Right Stick Down: RS Y-\n"
           "    Right Stick Right: RS X+\n"
           "    Right Stick Up: RS Y+\n"
           "    Start: Start\n"
           "    Select: Back\n"
           "    PS Button: \"Guide,Start&Back\"\n"
           "    Square: \"West,LT\"\n"
           "    Cross: \"South,RT\"\n"
           "    Circle: East\n"
           "    Triangle: North\n"
           "    Left: Left\n"
           "    Down: Down\n"
           "    Right: Right\n"
           "    Up: Up\n"
           "    R1: RS\n"
           "    R2: RB\n"
           "    R3: \"\"\n"
           "    L1: LS\n"
           "    L2: LB\n"
           "    L3: \"\"\n"
           "    Left Stick Deadzone: " + dz + "\n"
           "    Right Stick Deadzone: " + dz + "\n"
           "    Large Vibration Motor Multiplier: 100\n"
           "    Small Vibration Motor Multiplier: 100\n";
}

void testRpcs3() {
    const ControllerSettings c = settings("stick", "triggers", true);
    CHECK(deadzoneUnits(8) == 2621);
    // No controller: the keyboard is Player 1, nobody else (and no keyboard copilot).
    CHECK_TEXT(ctl::rpcs3InputConfig(c, {}, true), std::string(kRpcs3Header) +
                                                       "Player 1 Input:\n  Handler: Keyboard\n  Device: Keyboard\n" +
                                                       nullPlayers(2, 7));
    // One controller: Player 1 on SDL, the keyboard as its copilot on Player 7.
    const auto one = assigned({pad(7, kXbox, kGuidA)}, c);
    CHECK_TEXT(ctl::rpcs3InputConfig(c, one, true),
               std::string(kRpcs3Header) + "Player 1 Input:\n  Handler: SDL\n  Device: Xbox Series X Controller 1\n" +
                   rpcs3DefaultConfig(2621) + nullPlayers(2, 6) +
                   "Player 7 Input:\n  Handler: Keyboard\n  Device: Keyboard\n  Config:\n    Device Class Type: 40964\n");
    CHECK_TEXT(ctl::rpcs3InputConfig(c, one, false),
               std::string(kRpcs3Header) + "Player 1 Input:\n  Handler: SDL\n  Device: Xbox Series X Controller 1\n" +
                   rpcs3DefaultConfig(2621) + nullPlayers(2, 7));
    // Two controllers of the same model: "<name> 1" and "<name> 2", in connection order.
    const std::string ds = "DualSense Wireless Controller";
    const auto two = assigned({pad(3, ds, kGuidB, 1), pad(4, ds, kGuidB, 2)}, c);
    const std::string text = ctl::rpcs3InputConfig(c, two, true);
    CHECK_HAS(text, "Player 1 Input:\n  Handler: SDL\n  Device: DualSense Wireless Controller 1\n");
    CHECK_HAS(text, "Player 2 Input:\n  Handler: SDL\n  Device: DualSense Wireless Controller 2\n");
    CHECK_HAS(text, "Player 3 Input:\n  Handler: \"Null\"\n");
    // Player 2 chosen by model: the second controller becomes Player 1.
    ControllerSettings chosen = c;
    chosen.players[0].controller = kGuidB;
    const auto swapped = assigned({pad(1, kXbox, kGuidA), pad(2, ds, kGuidB)}, chosen);
    CHECK_HAS(ctl::rpcs3InputConfig(chosen, swapped, true), "Player 1 Input:\n  Handler: SDL\n  Device: DualSense Wireless Controller 1\n");
    CHECK_HAS(ctl::rpcs3InputConfig(chosen, swapped, true), "Player 2 Input:\n  Handler: SDL\n  Device: Xbox Series X Controller 1\n");
    // Digital steering, the game's own buttons, no vibration, no dead zone.
    const ControllerSettings d = settings("dpad", "buttons", false, 0);
    const std::string digital = ctl::rpcs3InputConfig(d, assigned({pad(1, kXbox, kGuidA)}, d), true);
    for (const char* line : {"    Left Stick Left: \"\"\n", "    Left Stick Down: \"\"\n", "    Left Stick Right: \"\"\n",
                             "    Left Stick Up: \"\"\n", "    Right Stick Left: RS X-\n", "    Left: \"Left,LS X-\"\n",
                             "    Down: \"Down,LS Y-\"\n", "    Right: \"Right,LS X+\"\n", "    Up: \"Up,LS Y+\"\n",
                             "    Square: West\n", "    Cross: South\n", "    R1: RB\n", "    R2: RT\n", "    R3: RS\n",
                             "    L1: LB\n", "    L2: LT\n", "    L3: LS\n", "    Left Stick Deadzone: 0\n",
                             "    Large Vibration Motor Multiplier: 0\n", "    Small Vibration Motor Multiplier: 0\n"})
        CHECK_HAS(digital, line);
    // Names YAML would misread are quoted.
    CHECK_HAS(ctl::rpcs3InputConfig(c, assigned({pad(1, "Pad: Pro #2", kGuidA)}, c), true), "  Device: \"Pad: Pro #2 1\"\n");
}

const char* kPcsx2Defaults =
    "[UI]\nSettingsVersion = 1\n\n[InputSources]\nKeyboard = true\nMouse = true\nSDL = false\n\n"
    "[Hotkeys]\nOpenPauseMenu = Keyboard/Escape\nToggleTurbo = Keyboard/Tab\n\n"
    "[Pad1]\nType = DualShock2\nUp = Keyboard/Up\nRight = Keyboard/Right\nDown = Keyboard/Down\nLeft = Keyboard/Left\n"
    "Triangle = Keyboard/I\nCircle = Keyboard/L\nCross = Keyboard/K\nSquare = Keyboard/J\nSelect = Keyboard/Backspace\n"
    "Start = Keyboard/Return\nL1 = Keyboard/Q\nL2 = Keyboard/1\nR1 = Keyboard/E\nR2 = Keyboard/3\nL3 = Keyboard/2\n"
    "R3 = Keyboard/4\nAnalog = SDL-0/Guide\nLUp = Keyboard/W\nLRight = Keyboard/D\nLDown = Keyboard/S\nLLeft = Keyboard/A\n"
    "RUp = Keyboard/T\nRRight = Keyboard/H\nRDown = Keyboard/G\nRLeft = Keyboard/F\n\n[Pad2]\nType = None\n";

void testPcsx2() {
    cfg::Ini ini;
    ini.parse(kPcsx2Defaults);
    ctl::applyPcsx2Controllers(ini, settings("stick", "triggers", true));
    CHECK(ini.get("InputSources", "SDL") == "true");
    CHECK(ini.get("InputSources", "Keyboard") == "true");
    CHECK_LIST(ini.getList("Hotkeys", "OpenPauseMenu"), "Keyboard/Escape", "SDL-0/Back & SDL-0/Start");
    CHECK_LIST(ini.getList("Hotkeys", "ToggleTurbo"), "Keyboard/Tab");
    CHECK(ini.get("Pad1", "Type") == "DualShock2");
    CHECK_LIST(ini.getList("Pad1", "Up"), "Keyboard/Up", "SDL-0/DPadUp");
    CHECK_LIST(ini.getList("Pad1", "Right"), "Keyboard/Right", "SDL-0/DPadRight");
    CHECK_LIST(ini.getList("Pad1", "Down"), "Keyboard/Down", "SDL-0/DPadDown");
    CHECK_LIST(ini.getList("Pad1", "Left"), "Keyboard/Left", "SDL-0/DPadLeft");
    CHECK_LIST(ini.getList("Pad1", "Triangle"), "Keyboard/I", "SDL-0/FaceNorth");
    CHECK_LIST(ini.getList("Pad1", "Circle"), "Keyboard/L", "SDL-0/FaceEast");
    CHECK_LIST(ini.getList("Pad1", "Cross"), "Keyboard/K", "SDL-0/FaceSouth", "SDL-0/+RightTrigger");
    CHECK_LIST(ini.getList("Pad1", "Square"), "Keyboard/J", "SDL-0/FaceWest", "SDL-0/+LeftTrigger");
    CHECK_LIST(ini.getList("Pad1", "Select"), "Keyboard/Backspace", "SDL-0/Back");
    CHECK_LIST(ini.getList("Pad1", "Start"), "Keyboard/Return", "SDL-0/Start");
    CHECK_LIST(ini.getList("Pad1", "L1"), "Keyboard/Q", "SDL-0/LeftShoulder");
    CHECK_LIST(ini.getList("Pad1", "R1"), "Keyboard/E", "SDL-0/RightShoulder");
    CHECK_LIST(ini.getList("Pad1", "L2"), "Keyboard/1");
    CHECK_LIST(ini.getList("Pad1", "R2"), "Keyboard/3", "SDL-0/RightStick");
    CHECK_LIST(ini.getList("Pad1", "L3"), "Keyboard/2");
    CHECK_LIST(ini.getList("Pad1", "R3"), "Keyboard/4");
    CHECK_LIST(ini.getList("Pad1", "Analog"));  // never bound
    CHECK_LIST(ini.getList("Pad1", "LUp"), "Keyboard/W", "SDL-0/-LeftY");
    CHECK_LIST(ini.getList("Pad1", "LDown"), "Keyboard/S", "SDL-0/+LeftY");
    CHECK_LIST(ini.getList("Pad1", "LLeft"), "Keyboard/A", "SDL-0/-LeftX");
    CHECK_LIST(ini.getList("Pad1", "LRight"), "Keyboard/D", "SDL-0/+LeftX");
    CHECK_LIST(ini.getList("Pad1", "RUp"), "Keyboard/T", "SDL-0/-RightY");
    CHECK_LIST(ini.getList("Pad1", "RDown"), "Keyboard/G", "SDL-0/+RightY");
    CHECK_LIST(ini.getList("Pad1", "RLeft"), "Keyboard/F", "SDL-0/-RightX");
    CHECK_LIST(ini.getList("Pad1", "RRight"), "Keyboard/H", "SDL-0/+RightX");
    CHECK_LIST(ini.getList("Pad1", "LargeMotor"), "SDL-0/LargeMotor");
    CHECK_LIST(ini.getList("Pad1", "SmallMotor"), "SDL-0/SmallMotor");
    CHECK(ini.get("Pad1", "Deadzone") == "0.08");
    CHECK(ini.get("Pad2", "Type") == "DualShock2");
    CHECK_LIST(ini.getList("Pad2", "Cross"), "SDL-1/FaceSouth", "SDL-1/+RightTrigger");
    CHECK_LIST(ini.getList("Pad2", "LLeft"), "SDL-1/-LeftX");
    CHECK_LIST(ini.getList("Pad2", "R2"), "SDL-1/RightStick");
    const std::string first = ini.text();
    ctl::applyPcsx2Controllers(ini, settings("stick", "triggers", true));
    CHECK_TEXT(ini.text(), first);  // run twice: nothing doubles

    // The game's own buttons, no vibration, Player 2 nobody, no dead zone: the same file changes over.
    ControllerSettings b = settings("stick", "buttons", false, 0);
    b.players[1].controller = "none";
    ctl::applyPcsx2Controllers(ini, b);
    CHECK_LIST(ini.getList("Pad1", "Cross"), "Keyboard/K", "SDL-0/FaceSouth");
    CHECK_LIST(ini.getList("Pad1", "Square"), "Keyboard/J", "SDL-0/FaceWest");
    CHECK_LIST(ini.getList("Pad1", "L2"), "Keyboard/1", "SDL-0/+LeftTrigger");
    CHECK_LIST(ini.getList("Pad1", "R2"), "Keyboard/3", "SDL-0/+RightTrigger");
    CHECK_LIST(ini.getList("Pad1", "L3"), "Keyboard/2", "SDL-0/LeftStick");
    CHECK_LIST(ini.getList("Pad1", "R3"), "Keyboard/4", "SDL-0/RightStick");
    CHECK_LIST(ini.getList("Pad1", "LargeMotor"));
    CHECK_LIST(ini.getList("Pad1", "SmallMotor"));
    CHECK(ini.get("Pad1", "Deadzone") == "0");
    CHECK(ini.get("Pad2", "Type") == "None");
    CHECK_LIST(ini.getList("Hotkeys", "OpenPauseMenu"), "Keyboard/Escape", "SDL-0/Back & SDL-0/Start");
    // Digital steering is not applied in PCSX2: the stick stays bound.
    cfg::Ini dpad;
    dpad.parse(kPcsx2Defaults);
    ctl::applyPcsx2Controllers(dpad, settings("dpad", "triggers", true, 40));
    CHECK_LIST(dpad.getList("Pad1", "LLeft"), "Keyboard/A", "SDL-0/-LeftX");
    CHECK(dpad.get("Pad1", "Deadzone") == "0.4");
}

const ctl::KeyValues kPpssppSdlDefault = {
    {"Up", "1-19,10-19"},          {"Down", "1-20,10-20"},          {"Left", "1-21,10-21"},
    {"Right", "1-22,10-22"},       {"Circle", "1-52,10-190"},       {"Cross", "1-54,10-189,10-4010"},
    {"Square", "1-29,10-191,10-4008"}, {"Triangle", "1-47,10-188"}, {"Start", "1-62,10-197"},
    {"Select", "1-66,10-196"},     {"L", "1-45,10-193"},            {"R", "1-51,10-192"},
    {"An.Up", "1-37,10-4003"},     {"An.Down", "1-39,10-4002"},     {"An.Left", "1-38,10-4001"},
    {"An.Right", "1-40,10-4000"},  {"RapidFire", "1-59"},           {"Fast-forward", "1-61"},
    {"SpeedToggle", "1-68"},       {"Pause", "1-111,10-4"},         {"Rewind", "1-67"},
    {"Analog limiter", "1-60"},    {"Toggle Debugger", "1-142"},    {"Pause (no menu)", "1-138"},
};
const ctl::KeyValues kPpssppWindowsDefault = {
    {"Up", "1-19,20-19,10-19"},
    {"Down", "1-20,20-20,10-20"},
    {"Left", "1-21,20-21,10-21"},
    {"Right", "1-22,20-22,10-22"},
    {"Circle", "1-52,20-97,10-190"},
    {"Cross", "1-54,20-96,20-4036,10-189,10-195"},
    {"Square", "1-29,20-99,20-4034,10-188,10-194"},
    {"Triangle", "1-47,20-100,10-191"},
    {"Start", "1-62,20-108,10-197"},
    {"Select", "1-50,20-109,10-196"},
    {"L", "1-45,20-102,10-192"},
    {"R", "1-51,20-103,10-193"},
    {"An.Up", "1-37,20-4002,10-4003"},
    {"An.Down", "1-39,20-4003,10-4002"},
    {"An.Left", "1-38,20-4001,10-4001"},
    {"An.Right", "1-40,20-4000,10-4000"},
    {"RapidFire", "1-59"},
    {"Fast-forward", "1-61"},
    {"SpeedToggle", "1-68,20-107"},
    {"Pause", "1-111,20-3,10-200"},
    {"Rewind", "1-67"},
    {"Analog limiter", "1-60"},
    {"Toggle Debugger", "1-142"},
    {"Pause (no menu)", "1-138"},
};

std::string valueOf(const ctl::KeyValues& kv, const std::string& key) {
    for (auto& [k, v] : kv)
        if (k == key) return v;
    return "<missing>";
}

ctl::KeyValues with(ctl::KeyValues kv, const ctl::KeyValues& changes) {
    for (auto& [key, value] : changes)
        for (auto& [k, v] : kv)
            if (k == key) v = value;
    return kv;
}

void checkMapping(const ctl::KeyValues& got, const ctl::KeyValues& want, const std::string& what, int line) {
    std::string diff;
    for (size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
        const std::string g = i < got.size() ? got[i].first + " = " + got[i].second : "<none>";
        const std::string w = i < want.size() ? want[i].first + " = " + want[i].second : "<none>";
        if (g != w && diff.empty()) diff = "line " + std::to_string(i + 1) + " is \"" + g + "\", expected \"" + w + "\"";
    }
    check(diff.empty(), what + ": " + diff, line);
}
#define CHECK_MAPPING(got, want) checkMapping((got), (want), #got, __LINE__)

void testPpsspp() {
    const ControllerSettings def = settings("stick", "triggers", true);
    CHECK_MAPPING(ctl::ppssppControlMapping(def, false), kPpssppSdlDefault);
    CHECK_MAPPING(ctl::ppssppControlMapping(def, true), kPpssppWindowsDefault);
    // The vibration and dead zone settings do not change the mapping.
    CHECK_MAPPING(ctl::ppssppControlMapping(settings("stick", "triggers", false, 30), false), kPpssppSdlDefault);
    // The game's own buttons: no trigger codes.
    CHECK_MAPPING(ctl::ppssppControlMapping(settings("stick", "buttons", true), false),
                  with(kPpssppSdlDefault, {{"Cross", "1-54,10-189"}, {"Square", "1-29,10-191"}}));
    CHECK_MAPPING(ctl::ppssppControlMapping(settings("stick", "buttons", true), true),
                  with(kPpssppWindowsDefault, {{"Cross", "1-54,20-96,10-189"}, {"Square", "1-29,20-99,10-188"}}));
    // Digital steering: the stick presses the d-pad; the analog nub keeps only the keyboard.
    const ctl::KeyValues sdlDpad = with(kPpssppSdlDefault, {{"Up", "1-19,10-19,10-4003"},
                                                            {"Down", "1-20,10-20,10-4002"},
                                                            {"Left", "1-21,10-21,10-4001"},
                                                            {"Right", "1-22,10-22,10-4000"},
                                                            {"An.Up", "1-37"},
                                                            {"An.Down", "1-39"},
                                                            {"An.Left", "1-38"},
                                                            {"An.Right", "1-40"}});
    CHECK_MAPPING(ctl::ppssppControlMapping(settings("dpad", "triggers", true), false), sdlDpad);
    CHECK_MAPPING(ctl::ppssppControlMapping(settings("dpad", "buttons", true), false),
                  with(sdlDpad, {{"Cross", "1-54,10-189"}, {"Square", "1-29,10-191"}}));
    CHECK_MAPPING(ctl::ppssppControlMapping(settings("dpad", "triggers", true), true),
                  with(kPpssppWindowsDefault, {{"Up", "1-19,20-19,10-19,20-4002,10-4003"},
                                               {"Down", "1-20,20-20,10-20,20-4003,10-4002"},
                                               {"Left", "1-21,20-21,10-21,20-4001,10-4001"},
                                               {"Right", "1-22,20-22,10-22,20-4000,10-4000"},
                                               {"An.Up", "1-37"},
                                               {"An.Down", "1-39"},
                                               {"An.Left", "1-38"},
                                               {"An.Right", "1-40"}}));

    // The marker: a fresh file gets the complete section and the marker.
    cfg::Ini fresh;
    CHECK(ctl::ppssppMappingDue(fresh, def));
    ctl::applyPpssppMapping(fresh, def, false);
    std::string expected = "[ControlMapping]\n";
    for (auto& [k, v] : kPpssppSdlDefault) expected += k + " = " + v + "\n";
    expected += "\n[Marathon]\nControlsVersion = 1\nProfile = stick/triggers\n";
    CHECK_TEXT(fresh.text(), expected);
    CHECK(!ctl::ppssppMappingDue(fresh, def));
    CHECK(!ctl::ppssppMappingDue(fresh, settings("stick", "triggers", false, 20)));  // same profile
    // A remap made in PPSSPP later stays until the profile changes.
    fresh.set("ControlMapping", "Cross", "1-54,10-190");
    CHECK(!ctl::ppssppMappingDue(fresh, def));
    CHECK(ctl::ppssppMappingDue(fresh, settings("stick", "buttons", true)));
    CHECK(ctl::ppssppMappingDue(fresh, settings("dpad", "triggers", true)));
    fresh.set("Marathon", "ControlsVersion", "0");
    CHECK(ctl::ppssppMappingDue(fresh, def));
    // PPSSPP's own file, without the marker.
    cfg::Ini own;
    own.parse("[ControlMapping]\nUp = 1-19\n");
    CHECK(ctl::ppssppMappingDue(own, def));
    CHECK(ctl::ppssppProfile(settings("dpad", "buttons", true)) == "dpad/buttons");
}

void testOthers() {
    const ControllerSettings def = settings("stick", "triggers", true);
    // MAME.
    CHECK_TEXT(ctl::mameControllerFile("raverace"),
               "<?xml version=\"1.0\"?>\n"
               "<mameconfig version=\"10\">\n"
               "  <system name=\"default\">\n"
               "    <input>\n"
               "      <port type=\"UI_MENU\"><newseq type=\"standard\">KEYCODE_TAB OR JOYCODE_1_SELECT JOYCODE_1_START</newseq></port>\n"
               "      <port type=\"UI_CANCEL\"><newseq type=\"standard\">KEYCODE_ESC OR JOYCODE_1_SELECT JOYCODE_1_BUTTON2</newseq></port>\n"
               "    </input>\n"
               "  </system>\n"
               "  <system name=\"raverace\">\n"
               "    <input>\n"
               "      <port type=\"P1_JOYSTICK_UP\"><newseq type=\"standard\">KEYCODE_UP OR JOYCODE_1_HAT1UP OR JOYCODE_1_BUTTON5</newseq></port>\n"
               "      <port type=\"P1_JOYSTICK_DOWN\"><newseq type=\"standard\">KEYCODE_DOWN OR JOYCODE_1_HAT1DOWN OR JOYCODE_1_BUTTON6</newseq></port>\n"
               "    </input>\n"
               "  </system>\n"
               "</mameconfig>\n");
    CHECK_LIST(ctl::mameControllerArgs(def, "/m/ctrlr"), "-joystick", "-joystick_deadzone", "0.08", "-ctrlrpath", "/m/ctrlr",
               "-ctrlr", "marathon");
    CHECK_LIST(ctl::mameControllerArgs(settings("dpad", "buttons", false, 0), "c"), "-joystick", "-joystick_deadzone", "0",
               "-ctrlrpath", "c", "-ctrlr", "marathon");
    CHECK(ctl::mameControllerArgs(settings("stick", "triggers", true, 40), "c")[2] == "0.4");
    CHECK(ctl::mameControllerArgs(settings("stick", "triggers", true, 5), "c")[2] == "0.05");

    // Xenia: TOML floats always with a decimal point.
    const ctl::KeyValues xenia = ctl::xeniaHidKeys(def);
    CHECK(xenia == ctl::KeyValues({{"HID.hid", "\"any\""},
                                   {"HID.vibration", "true"},
                                   {"HID.left_stick_deadzone_percentage", "0.08"},
                                   {"HID.right_stick_deadzone_percentage", "0.08"}}));
    const ctl::KeyValues xeniaOff = ctl::xeniaHidKeys(settings("dpad", "buttons", false, 0));
    CHECK(valueOf(xeniaOff, "HID.vibration") == "false");
    CHECK(valueOf(xeniaOff, "HID.left_stick_deadzone_percentage") == "0.0");
    CHECK(valueOf(ctl::xeniaHidKeys(settings("stick", "triggers", true, 40)), "HID.right_stick_deadzone_percentage") == "0.4");

    // PSXRecomp devices: GUID, nobody, automatic.
    PlayerChoice automatic, guid{kGuidA, "Xbox"}, nobody{"none", ""};
    CHECK(ctl::psxDevice(guid, std::nullopt, "", false, true) == kGuidA);
    CHECK(ctl::psxDevice(guid, std::string("keyboard"), "", false, true) == kGuidA);
    CHECK(ctl::psxDevice(automatic, std::nullopt, "", false, true) == "gamepad");
    CHECK(ctl::psxDevice(automatic, std::string("keyboard"), "", false, true) == "gamepad");
    CHECK(ctl::psxDevice(automatic, std::string(kGuidB), "", false, true) == kGuidB);  // the player's own choice
    CHECK(ctl::psxDevice(automatic, std::string(kGuidB), kGuidB, false, true) == "gamepad");  // Marathon's earlier choice
    CHECK(ctl::psxDevice(automatic, std::string("none"), "", false, true) == "none");
    CHECK(ctl::psxDevice(nobody, std::nullopt, "", false, true) == "gamepad");  // Player 1 cannot be nobody
    CHECK(ctl::psxDevice(nobody, std::string(kGuidB), "", true, false) == "none");
    CHECK(ctl::psxDevice(automatic, std::string("keyboard"), "", true, false) == "keyboard");  // Player 2's keyboard stays
    CHECK(ctl::psxDevice(automatic, std::string("none"), "none", true, false) == "gamepad");
    CHECK(ctl::psxControllerKeys(def, false, std::nullopt, std::nullopt, "", "") ==
          ctl::KeyValues({{"controller.p1_device", "\"gamepad\""}}));
    CHECK(ctl::psxControllerKeys(def, true, std::string("keyboard"), std::nullopt, "", "") ==
          ctl::KeyValues({{"controller.p1_device", "\"gamepad\""},
                          {"controller.p2_device", "\"gamepad\""},
                          {"controller.p1_mode", "\"analog\""},
                          {"controller.p2_mode", "\"analog\""}}));
    ControllerSettings r4 = settings("dpad", "buttons", false);
    r4.players[0].controller = kGuidA;
    r4.players[1].controller = "none";
    CHECK(ctl::psxControllerKeys(r4, true, std::nullopt, std::string(kGuidB), "", "") ==
          ctl::KeyValues({{"controller.p1_device", "\"" + kGuidA + "\""},
                          {"controller.p2_device", "\"none\""},
                          {"controller.p1_mode", "\"digital\""},
                          {"controller.p2_mode", "\"digital\""}}));
    auto mods = ctl::r4ControllerMods(def);
    CHECK(mods.size() == 3);
    CHECK(mods[0].package == "r4.modern-controls" && mods[0].feature == "modern-controls" && mods[0].enabled &&
          mods[0].values == ctl::KeyValues({{"scheme", "modern"}}));
    CHECK(mods[1].feature == "rumble" && mods[1].enabled && mods[1].values.empty());
    CHECK(mods[2].feature == "controls-tuning" && mods[2].enabled && mods[2].values == ctl::KeyValues({{"stick_deadzone", "8"}}));
    for (auto& c : {settings("stick", "buttons", true), settings("dpad", "triggers", true), settings("dpad", "buttons", true)})
        CHECK(ctl::r4ControllerMods(c)[0].values == ctl::KeyValues({{"scheme", "classic"}}));
    mods = ctl::r4ControllerMods(settings("stick", "triggers", false, 0));
    CHECK(!mods[1].enabled && mods[2].values == ctl::KeyValues({{"stick_deadzone", "0"}}));

    // Fukami: analog only switched on.
    auto fk = ctl::fukamiInputKeys(def);
    CHECK(fk.size() == 2 && fk[0].section == "input" && fk[0].key == "analog" && fk[0].value == "true" &&
          fk[1].key == "rumble" && fk[1].value == "true");
    fk = ctl::fukamiInputKeys(settings("dpad", "triggers", false));
    CHECK(fk.size() == 1 && fk[0].key == "rumble" && fk[0].value == "false");

    // RR6 recomp.
    CHECK_LIST(ctl::rr6ControllerFlags("/p/gamecontrollerdb.txt", false), "--hid_mappings_file=/p/gamecontrollerdb.txt");
    CHECK_LIST(ctl::rr6ControllerFlags("", true), "--rr6_log_input=true");
    CHECK_LIST(ctl::rr6ControllerFlags("", false));

    // Rage Racer PC and Rave Racer.
    CHECK_LIST(ctl::rageInputSets(def), "input.analog=true", "input.steering_deadzone=0.08");
    CHECK_LIST(ctl::rageInputSets(settings("dpad", "buttons", false, 0)), "input.analog=false", "input.steering_deadzone=0");
    CHECK(ctl::raveCfgKeys(def) == ctl::KeyValues({{"pad_deadzone", "2621"}}));
    CHECK(ctl::raveCfgKeys(settings("stick", "triggers", true, 0)) == ctl::KeyValues({{"pad_deadzone", "0"}}));

    // The log line.
    CHECK(ctl::noteLine(def, "x") == "controllers: steering=stick pedals=triggers vibration=on deadzone=8% -> x");
    CHECK(ctl::noteLine(settings("dpad", "buttons", false, 0), "left to y") ==
          "controllers: steering=dpad pedals=buttons vibration=off deadzone=0% -> left to y");
    CHECK(ctl::linkedNote("RPCS3") == "controllers: RPCS3 keeps its own controller settings (linked install)");
    CHECK(ctl::fraction(8) == "0.08" && ctl::fraction(10) == "0.1" && ctl::fraction(0) == "0" &&
          ctl::fraction(0, true) == "0.0" && ctl::fraction(40, true) == "0.4" && ctl::fraction(100) == "1");
}

// ---- 3. adapter dry runs ---------------------------------------------------------------------------

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& text, bool executable = false) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
    if (executable)
        fs::permissions(p, fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
}

struct Bench {
    Catalog catalog;
    Library lib;
    fs::path work;
};

void install(Bench& b, const std::string& backendId, const fs::path& root, const fs::path& exe, bool userProvided) {
    BackendInstall bi;
    bi.backendId = backendId;
    bi.version = "test";
    bi.root = pathToUtf8(root);
    bi.executable = pathToUtf8(exe);
    bi.userProvided = userProvided;
    b.lib.setInstall(bi);
}

void gameFiles(Bench& b, const std::string& gameId, const fs::path& path, const std::string& format) {
    LibraryEntry& e = b.lib.entry(gameId);
    e.files.path = pathToUtf8(path);
    e.files.format = format;
}

struct Run {
    LaunchResult result;
    std::string log;
    ControlsInfo info;
    std::string commandLine;  // arguments joined by "\n"
};

Run launch(Bench& b, const std::string& gameId, const std::string& backendId, const ControllerSettings& c,
           const std::vector<ConnectedPad>& pads, const json& settingsJson = json::object(),
           DisplayInfo display = DisplayInfo{2560, 1440, 60, 1.0f}) {
    Run run;
    const GameDef* g = b.catalog.game(gameId);
    const BackendDef* def = b.catalog.backend(backendId);
    if (!g || !def) {
        check(false, "catalog has " + gameId + " and " + backendId, __LINE__);
        return run;
    }
    const GameBackendOption* option = nullptr;
    for (auto& o : g->backends)
        if (o.backend == backendId && !option) option = &o;
    auto backend = createBackend(*def);
    LaunchRequest req;
    req.game = g;
    req.option = option;
    req.backend = def;
    req.entry = b.lib.find(gameId);
    req.library = &b.lib;
    req.settings = GameSettings::fromJson(settingsJson);
    req.display = display;
    req.controllers = c;
    req.pads = pads;
    const fs::path log = paths().logs / utf8ToPath(backendId + ".log");
    std::error_code ec;
    fs::remove(log, ec);
    run.result = backend->launch(req);
    run.log = readFile(log);
    run.info = backend->controls(req);
    for (auto& a : run.result.commandLine) run.commandLine += a + "\n";
    check(run.result.ok, backendId + " dry run for " + gameId + ": " + run.result.error, __LINE__);
    return run;
}

// prepareMameSet() (EmuCommon.h) keeps <set>.zip when its stamp matches the source: the test has
// no ROMs, so it writes a placeholder set with the stamp prepareMameSet() computes, and the device
// sets it would otherwise extract.
void fakeMameSet(Bench& b, const std::string& gameId, const std::string& backendId, const fs::path& outDir,
                 const Strings& deviceSets) {
    const GameDef* g = b.catalog.game(gameId);
    const BackendDef* def = b.catalog.backend(backendId);
    const LibraryEntry* entry = b.lib.find(gameId);
    if (!g || !def || !entry) return;
    std::string set = g->identify.mameSet;
    for (auto& o : g->backends)
        if (o.backend == backendId) set = jget<std::string>(o.params, "set", set);
    std::string stamp = entry->files.path;
    std::error_code ec;
    const auto t = fs::last_write_time(utf8ToPath(entry->files.path), ec);
    if (!ec) stamp += "|" + std::to_string(static_cast<long long>(t.time_since_epoch().count()));
    for (auto& d : g->identify.dumps) stamp += "|" + d.file;
    json extras = def->params.value("setExtras", json::object());
    if (extras.is_object())
        for (auto& e : extras.value(set, json::array())) {
            const std::string file = jget<std::string>(e, "file", ""), crc = jget<std::string>(e, "crc32", "");
            bool listed = false;
            for (auto& k : g->identify.dumps) listed = listed || emu::lower(k.file) == emu::lower(file);
            if (!file.empty() && !crc.empty() && !listed) stamp += "|+" + file;
        }
    writeFile(outDir / (set + ".zip"), "placeholder");
    writeFile(outDir / (set + ".zip.stamp"), stamp);
    for (auto& d : deviceSets) writeFile(outDir / (d + ".zip"), "placeholder");
}

void testAdapters(const fs::path& work, const std::string& catalogDir) {
    Bench b;
    b.work = work;
    std::string error;
    const std::string dir = catalogDir + "/";
    if (!b.catalog.loadFromStrings(readFile(dir + "games.json"), readFile(dir + "backends.json"), error,
                                   readFile(dir + "recomps.json"))) {
        check(false, "catalog loads: " + error, __LINE__);
        return;
    }
    b.lib.load();
    const fs::path games = work / "games", installs = work / "installs";
    const ControllerSettings def = settings("stick", "triggers", true);
    const auto onePad = assigned({pad(1, kXbox, kGuidA)}, def);
    const std::string defNote = "controllers: steering=stick pedals=triggers vibration=on deadzone=8% -> ";

    // RPCS3: Marathon's own folders.
    {
        writeFile(games / "rr7.iso", "iso");
        gameFiles(b, "ridge-racer-7", games / "rr7.iso", "iso");
        const fs::path exe = installs / "rpcs3" / "rpcs3";
        writeFile(exe, "#!/bin/sh\nexit 0\n", true);
        install(b, "rpcs3", exe.parent_path(), exe, false);
        fs::path configDir = paths().config / "rpcs3";
        switch (currentOs()) {
            case Os::Windows: configDir = configDir / "rpcs3" / "config"; break;
            case Os::Linux: configDir = configDir / "config" / "rpcs3"; break;
            default: configDir = configDir / "Library" / "Application Support" / "rpcs3"; break;
        }
        const fs::path emuDir = currentOs() == Os::Windows ? configDir.parent_path() : configDir;
        writeFile(emuDir / "dev_flash" / "vsh" / "etc" / "version.txt", "release:04.93:\n");
        Run r = launch(b, "ridge-racer-7", "rpcs3", def, onePad);
        const fs::path yml = configDir / "input_configs" / "global" / "Default.yml";
        CHECK_TEXT(readFile(yml), ctl::rpcs3InputConfig(def, onePad, true));
        CHECK_HAS(readFile(yml), "  Device: Xbox Series X Controller 1\n");
        CHECK_HAS(readFile(configDir / "config.yml"), "Pause Emulation During Home Menu: true");
        CHECK_HAS(r.log, defNote + pathToUtf8(yml) + " (Player 1 SDL 'Xbox Series X Controller 1', Player 2 nobody)");
        CHECK_LACKS(r.log, "win over Marathon's");
        CHECK(r.info.players == 2 && r.info.playerChoice && r.info.steering && r.info.pedals && !r.info.vibration);
        // RPCS3's own per-game pad file wins; without a controller the player is told to connect it first.
        writeFile(configDir / "input_configs" / "BLUS30001" / "Default.yml", "Player 1 Input:\n  Handler: Keyboard\n");
        r = launch(b, "ridge-racer-7", "rpcs3", def, {});
        CHECK_HAS(r.log, "controllers: RPCS3's own pad settings for this game win over Marathon's");
        CHECK_HAS(r.log, "controllers: connect the controller before starting: RPCS3 picks it at start");
        CHECK_HAS(readFile(yml), "Player 1 Input:\n  Handler: Keyboard\n  Device: Keyboard\n");

        // A linked RPCS3 in portable mode: nothing is written into its folders.
        const fs::path linked = installs / "rpcs3-portable" / "rpcs3";
        writeFile(linked, "#!/bin/sh\nexit 0\n", true);
        std::error_code ec;
        fs::create_directories(linked.parent_path() / "portable", ec);
        install(b, "rpcs3", linked.parent_path(), linked, true);
        r = launch(b, "ridge-racer-7", "rpcs3", def, onePad);
        CHECK(fs::is_empty(linked.parent_path() / "portable", ec));
        CHECK_HAS(r.log, defNote + "left to RPCS3");
        CHECK_HAS(r.log, ctl::linkedNote("RPCS3"));
        CHECK(!r.info.steering && !r.info.note.empty());
    }

    // PCSX2: bindings added to Marathon's PCSX2.ini, keyboard kept, nothing doubles on the next start.
    {
        writeFile(games / "rrv.iso", "iso");
        gameFiles(b, "ridge-racer-v", games / "rrv.iso", "iso");
        const fs::path exe = installs / "pcsx2" / "pcsx2-qt";
        writeFile(exe, "#!/bin/sh\nexit 0\n", true);
        install(b, "pcsx2", exe.parent_path(), exe, false);
        const fs::path ini = paths().config / "pcsx2" / "PCSX2" / "inis" / "PCSX2.ini";
        writeFile(ini, kPcsx2Defaults);
        Run r = launch(b, "ridge-racer-v", "pcsx2", def, onePad);
        cfg::Ini written;
        written.parse(readFile(ini));
        CHECK_LIST(written.getList("Pad1", "Cross"), "Keyboard/K", "SDL-0/FaceSouth", "SDL-0/+RightTrigger");
        CHECK_LIST(written.getList("Pad2", "Cross"), "SDL-1/FaceSouth", "SDL-1/+RightTrigger");
        CHECK_LIST(written.getList("Hotkeys", "OpenPauseMenu"), "Keyboard/Escape", "SDL-0/Back & SDL-0/Start");
        CHECK(written.get("Pad1", "Deadzone") == "0.08" && written.get("UI", "SettingsVersion") == "1");
        CHECK_HAS(r.log, defNote + "PCSX2.ini SDL bindings in [Pad1] and [Pad2]");
        const std::string first = readFile(ini);
        launch(b, "ridge-racer-v", "pcsx2", def, onePad);
        CHECK_TEXT(readFile(ini), first);
        r = launch(b, "ridge-racer-v", "pcsx2", settings("dpad", "triggers", true), onePad);
        CHECK_HAS(r.log, "controllers: the stick stays analog in PCSX2; the d-pad steers digitally");
        CHECK(r.info.players == 2 && !r.info.playerChoice && !r.info.steering && r.info.pedals);
    }

    // Xenia Canary: [HID] in Marathon's config.
    {
        writeFile(games / "rr6.iso", "iso");
        gameFiles(b, "ridge-racer-6", games / "rr6.iso", "iso");
        const fs::path exe = installs / "xenia" / "xenia_canary";
        writeFile(exe, "#!/bin/sh\nexit 0\n", true);
        install(b, "xenia-canary", exe.parent_path(), exe, false);
        Run r = launch(b, "ridge-racer-6", "xenia-canary", settings("stick", "triggers", false, 12), onePad);
        const std::string toml = readFile(paths().config / "xenia-canary" / "xenia-canary.config.toml");
        CHECK_HAS(toml, "[HID]");
        CHECK_HAS(toml, "hid = \"any\"");
        CHECK_HAS(toml, "vibration = false");
        CHECK_HAS(toml, "left_stick_deadzone_percentage = 0.12");
        CHECK_HAS(toml, "right_stick_deadzone_percentage = 0.12");
        CHECK_HAS(r.log, "controllers: steering=stick pedals=triggers vibration=off deadzone=12% -> HID.hid = \"any\"");
        CHECK(r.info.vibration && r.info.deadzone && !r.info.pedals && !r.info.note.empty());
    }

    // PPSSPP: controls.ini with the marker, a .bak of PPSSPP's own file, the per-game ini, the dead zone.
    {
        writeFile(games / "rr2.iso", "iso");
        gameFiles(b, "ridge-racer-2-psp", games / "rr2.iso", "iso");
        const fs::path exe = installs / "ppsspp" / "PPSSPPSDL";
        writeFile(exe, "#!/bin/sh\nexit 0\n", true);
        install(b, "ppsspp", exe.parent_path(), exe, false);
        fs::path system;
        switch (currentOs()) {
            case Os::Windows: system = paths().config / "ppsspp" / "memstick"; break;
            case Os::MacOS: system = paths().config / "ppsspp" / "home" / ".config" / "ppsspp"; break;
            default: system = paths().config / "ppsspp" / "xdg" / "ppsspp"; break;
        }
        system = system / "PSP" / "SYSTEM";
        const fs::path controls = system / "controls.ini", gameIni = system / "UCES00422_ppsspp.ini";
        writeFile(controls, "[ControlMapping]\nUp = 1-19\n");
        writeFile(gameIni, "[Graphics]\nInternalResolution = 2\n\n[ControlMapping]\nCross = 1-54\n");
        const json extra = {{"extra", {{"discId", "UCES00422"}}}};
        const bool windows = currentOs() == Os::Windows;
        Run r = launch(b, "ridge-racer-2-psp", "ppsspp", def, onePad, extra);
        cfg::Ini written;
        written.parse(readFile(controls));
        CHECK(readFile(utf8ToPath(pathToUtf8(controls) + ".bak")) == "[ControlMapping]\nUp = 1-19\n");
        CHECK(!ctl::ppssppMappingDue(written, def));
        CHECK(written.get("ControlMapping", "Cross") == valueOf(ctl::ppssppControlMapping(def, windows), "Cross"));
        CHECK(written.get("ControlMapping", "Pause (no menu)") == "1-138");
        cfg::Ini perGame;
        perGame.parse(readFile(gameIni));
        CHECK(perGame.get("Graphics", "InternalResolution") != "" && !ctl::ppssppMappingDue(perGame, def));
        CHECK(perGame.get("ControlMapping", "L") == valueOf(ctl::ppssppControlMapping(def, windows), "L"));
        CHECK(perGame.get("Control", "AnalogDeadzone") == "0.08");
        cfg::Ini mainIni;
        mainIni.parse(readFile(system / "ppsspp.ini"));
        CHECK(mainIni.get("Control", "AnalogDeadzone") == "0.08");
        CHECK_HAS(r.log, defNote + pathToUtf8(controls) + " [ControlMapping] stick/triggers");
        // A remap made in PPSSPP stays while the profile is the same; a new profile rewrites it.
        written.set("ControlMapping", "Cross", "1-54,10-190");
        writeFile(controls, written.text());
        r = launch(b, "ridge-racer-2-psp", "ppsspp", def, onePad, extra);
        CHECK(readFile(controls) == written.text());
        CHECK_HAS(r.log, pathToUtf8(controls) + " kept");
        r = launch(b, "ridge-racer-2-psp", "ppsspp", settings("dpad", "buttons", true, 20), onePad, extra);
        written.parse(readFile(controls));
        CHECK(written.get("Marathon", "Profile") == "dpad/buttons");
        CHECK(written.get("ControlMapping", "An.Up") == "1-37");
        CHECK(r.info.steering && r.info.pedals && r.info.deadzone && !r.info.vibration);
        if (windows) {
            // A linked copy on Windows: Marathon's own controls.ini through --controlconfig=.
            install(b, "ppsspp", exe.parent_path(), exe, true);
            r = launch(b, "ridge-racer-2-psp", "ppsspp", def, onePad);
            CHECK_HAS(r.commandLine, "--controlconfig=" + pathToUtf8(paths().config / "ppsspp" / "controls.ini") + "\n");
        }
    }

    // MAME: the controller file and its arguments.
    {
        std::error_code ec;
        fs::create_directories(games / "raverace", ec);
        gameFiles(b, "rave-racer", games / "raverace", "romset-dir");
        const fs::path exe = installs / "mame" / "mame";
        writeFile(exe, "#!/bin/sh\nexit 0\n", true);
        install(b, "mame", exe.parent_path(), exe, false);
        fakeMameSet(b, "rave-racer", "mame", paths().cache / "mame-sets" / "rave-racer", {"namcoc71", "namcoc74"});
        Run r = launch(b, "rave-racer", "mame", def, onePad);
        const fs::path ctrlr = paths().config / "mame" / "ctrlr";
        CHECK_TEXT(readFile(ctrlr / "marathon.cfg"), ctl::mameControllerFile("raverace"));
        CHECK_HAS(r.commandLine, "-joystick\n-joystick_deadzone\n0.08\n-ctrlrpath\n" + pathToUtf8(ctrlr) + "\n-ctrlr\nmarathon\n");
        CHECK_HAS(r.log, defNote + pathToUtf8(ctrlr / "marathon.cfg"));
        CHECK(r.info.deadzone && !r.info.steering);
    }

    // Ridge Racer recomp (a build-macos layout): Player 1 routed to a controller.
    {
        writeFile(games / "rr1.cue", "FILE \"rr1.bin\" BINARY\n");
        gameFiles(b, "ridge-racer", games / "rr1.cue", "cue");
        const fs::path root = installs / "rr1";
        writeFile(root / "game.toml", "[game]\nid = \"ridge-racer\"\n");
        const fs::path exe = root / "build-macos" / "RidgeRacer_Recompiled";
        writeFile(exe, "#!/bin/sh\nexit 0\n", true);
        writeFile(root / "build-macos" / "settings.toml", "[video]\nfullscreen = 0\n");
        install(b, "rr1-recomp", root, exe, true);
        Run r = launch(b, "ridge-racer", "rr1-recomp", def, onePad);
        CHECK_HAS(r.log, defNote + "settings.toml controller.p1_device=\"gamepad\" (Ridge Racer keeps its own digital button map)");
        CHECK_LACKS(r.log, "p1_mode");
        CHECK(readFile(root / "build-macos" / "settings.toml") == "[video]\nfullscreen = 0\n");  // a dry run writes nothing
        CHECK(r.info.playerChoice && !r.info.steering && !r.info.pedals && !r.info.note.empty());
    }

    // R4 recomp (a flat folder): devices, modes and the R4 Controls mod.
    {
        writeFile(games / "r4.cue", "FILE \"r4.bin\" BINARY\n");
        gameFiles(b, "r4", games / "r4.cue", "cue");
        const fs::path root = installs / "r4";
        const fs::path exe = root / "r4-runtime";
        writeFile(exe, "#!/bin/sh\nexit 0\n", true);
        writeFile(root / "game.toml", "[game]\nid = \"r4\"\n");
        writeFile(root / "settings.toml", "[controller]\np1_device = \"keyboard\"\n");
        writeFile(root / "mods" / "bundled" / "r4.modern-controls" / "1.0.0" / "manifest.toml", "id = \"r4.modern-controls\"\n");
        install(b, "r4-recomp", root, exe, false);
        ControllerSettings c = def;
        c.players[1].controller = "none";
        Run r = launch(b, "r4", "r4-recomp", c, onePad);
        CHECK_HAS(r.log, defNote + "settings.toml controller.p1_device=\"gamepad\", controller.p2_device=\"none\", "
                                   "controller.p1_mode=\"analog\", controller.p2_mode=\"analog\"");
        CHECK_HAS(r.log, "mod r4.modern-controls.modern-controls = on scheme=modern");
        CHECK_HAS(r.log, "mod r4.modern-controls.rumble = on");
        CHECK_HAS(r.log, "mod r4.modern-controls.controls-tuning = on stick_deadzone=8");
        CHECK(r.info.players == 2 && r.info.steering && r.info.pedals && r.info.vibration && r.info.deadzone);
        ControllerSettings classic = settings("dpad", "triggers", false, 15);
        classic.players[0].controller = kGuidA;
        r = launch(b, "r4", "r4-recomp", classic, onePad);
        CHECK_HAS(r.log, "settings.toml controller.p1_device=\"" + kGuidA + "\", controller.p2_device=\"gamepad\", "
                                                                           "controller.p1_mode=\"digital\"");
        CHECK_HAS(r.log, "mod r4.modern-controls.modern-controls = on scheme=classic");
        CHECK_HAS(r.log, "mod r4.modern-controls.rumble = off");
        CHECK_HAS(r.log, "mod r4.modern-controls.controls-tuning = on stick_deadzone=15");
        CHECK(!r.info.vibration && !r.info.deadzone);
    }

    // Fukami: [input] in the preview of fukami.ini (a dry run never writes the shared file).
    {
        gameFiles(b, "ridge-racer-v", games / "rrv.iso", "iso");
        const fs::path folder = installs / "fukami" / "Fukami";
        const fs::path exe = folder / "Fukami";
        writeFile(exe, "#!/bin/sh\nexit 0\n", true);
        writeFile(folder / "share" / "fukami-default.ini",
                  "[display]\n# --- ratio ---\nratio = 4:3\n\n[rendering]\n# --- scale ---\nscale = 2\n\n"
                  "[input]\n# --- analog ---\nanalog = true\n\n# --- rumble ---\nrumble = true\n");
        install(b, "fukami", folder, exe, false);
        Run r = launch(b, "ridge-racer-v", "fukami", settings("stick", "triggers", false), onePad);
        cfg::Ini preview;
        preview.parse(readFile(paths().config / "fukami" / "fukami.ini.preview"));
        CHECK(preview.get("input", "analog") == "true" && preview.get("input", "rumble") == "false");
        CHECK_HAS(r.log, "controllers: steering=stick pedals=triggers vibration=off deadzone=8% -> fukami.ini input.analog = "
                         "true, input.rumble = false");
        CHECK(r.info.vibration && !r.info.steering && r.info.players == 2 && !r.info.note.empty());
    }

    // Ridge Racer 6 recomp: the platform's own package/development controller database.
    {
        gameFiles(b, "ridge-racer-6", games / "rr6.iso", "iso");
        const fs::path root = installs / "rr6";
        fs::path exe = root / "RR6 Launcher.exe";
        fs::path mappings = root / "gamecontrollerdb.txt";
        if (currentOs() == Os::MacOS) {
            exe = root / "out/build/rr6_recomp.app/Contents/MacOS/rr6_recomp";
            writeFile(root / "rr6_recomp_manifest.toml", "# fixture\n");
            writeFile(root.parent_path() / "game/default.xex", "XEX2");
        } else if (currentOs() == Os::Linux) {
            exe = root / "bin/rr6_recomp";
            mappings = root / "bin/gamecontrollerdb.txt";
        }
        writeFile(exe, "MZ", true);
        writeFile(root / "bin" / "rr6_recomp.exe", "MZ", true);
        writeFile(root / "game" / "default.xex", "XEX2");
        writeFile(root / "game" / ".rr6-ready", "");
        writeFile(mappings, "# SDL mappings\n");
        install(b, "rr6-recomp", root, exe, false);
        Run r = launch(b, "ridge-racer-6", "rr6-recomp", def, onePad, {{"extra", {{"logInput", true}}}});
        CHECK_HAS(r.commandLine, "--hid_mappings_file=" + pathToUtf8(mappings) + "\n");
        CHECK_HAS(r.commandLine, "--rr6_log_input=true\n");
        CHECK_LACKS(r.commandLine, "keybind_");
        CHECK_HAS(r.log, defNote + "left to Ridge Racer 6 (its own controls); controller database");
        CHECK(!r.info.steering && !r.info.pedals && !r.info.vibration && !r.info.deadzone && !r.info.layout.empty());
    }

    // Rage Racer PC: --set input.*.
    {
        writeFile(games / "rage.cue", "FILE \"rage.bin\" BINARY\n");
        gameFiles(b, "rage-racer", games / "rage.cue", "cue");
        const fs::path exe = installs / "rage" / "rage-racer";
        writeFile(exe, "#!/bin/sh\nexit 0\n", true);
        install(b, "rage-racer-pc", exe.parent_path(), exe, false);
        Run r = launch(b, "rage-racer", "rage-racer-pc", def, onePad);
        CHECK_HAS(r.commandLine, "--set\ninput.analog=true\n--set\ninput.steering_deadzone=0.08\n");
        CHECK_HAS(r.log, defNote + "--set input.analog=true, --set input.steering_deadzone=0.08");
        CHECK_HAS(r.log, "controllers: vibration is not applied");
        r = launch(b, "rage-racer", "rage-racer-pc", settings("dpad", "triggers", false, 0), onePad,
                   {{"fullscreen", true}});
        CHECK_HAS(r.commandLine, "--set\ninput.analog=false\n--set\ninput.steering_deadzone=0\n");
        CHECK_HAS(r.log, "the port opens in a window; F4 switches to full screen");
        CHECK(r.info.steering && r.info.deadzone && !r.info.pedals);
    }

    // Rave Racer native: pad_deadzone in the folder Marathon owns, nothing in a linked portable folder.
    {
        const fs::path exe = installs / "rave" / "rr";
        writeFile(exe, std::string("\x7f" "ELF", 4), true);
        install(b, "rave-racer-native", exe.parent_path(), exe, false);
        const fs::path data = currentOs() == Os::Windows ? exe.parent_path()
                                                         : paths().config / "rave-racer-native" / "namco-2x-systems" / "raverace";
        fakeMameSet(b, "rave-racer", "rave-racer-native", data / "roms", {"namcoc74"});
        Run r = launch(b, "rave-racer", "rave-racer-native", def, onePad);
        cfg::Ini cfgFile;
        cfgFile.parse(readFile(data / "rr_controls.cfg"));
        CHECK(cfgFile.get("", "pad_deadzone") == "2621");
        CHECK_HAS(r.log, defNote + pathToUtf8(data / "rr_controls.cfg") + " pad_deadzone = 2621");
        CHECK(r.info.deadzone && r.info.note.empty());

        const fs::path linked = installs / "rave-linked" / "RaveRacer-x86_64.AppImage";
        writeFile(linked, std::string("\x7f" "ELF", 4), true);
        // The game's own folder: beside RaveRacer.exe on Windows, the AppImage's portable data folder elsewhere.
        const fs::path own = currentOs() == Os::Windows ? linked.parent_path()
                                                        : linked.parent_path() / "namco-2x-systems-data" / "raverace";
        std::error_code ec;
        fs::create_directories(own, ec);
        install(b, "rave-racer-native", linked.parent_path(), linked, true);
        r = launch(b, "rave-racer", "rave-racer-native", def, onePad);
        CHECK(!fs::exists(own / "rr_controls.cfg", ec));
        CHECK_HAS(r.log, defNote + "left to Rave Racer");
        CHECK_HAS(r.log, ctl::linkedNote("Rave Racer"));
        CHECK(!r.info.deadzone && !r.info.note.empty());
    }

    // Exact 31:9 projection, followed by restoring the native presentation.
    for (const char* id : {"ridge-racer", "rage-racer", "r4"}) {
        const fs::path core = installs / "cores" / "swanstation";
        writeFile(core, "fixture");
        install(b, "lr-swanstation", core.parent_path(), core, false);
        gameFiles(b, id, games / "ps1.cue", "cue");
        Run r = launch(b, id, "lr-swanstation", def, onePad, {{"aspect", "31:9"}});
        CHECK(r.result.coreOptions["swanstation_GPU_WidescreenHack"] == "true");
        CHECK(r.result.coreOptions["swanstation_Display_CustomAspectRatioNumerator"] == "31");
        CHECK(r.result.coreOptions["swanstation_Display_CustomAspectRatioDenominator"] == "9");
        CHECK(r.result.presentAspect == "auto"); // presentation follows the widened core geometry
        r = launch(b, id, "lr-swanstation", def, onePad, {{"aspect", "4:3"}});
        CHECK(r.result.coreOptions["swanstation_GPU_WidescreenHack"] == "false");
        CHECK(r.result.coreOptions["swanstation_Display_AspectRatio"] == "4:3");
    }
    for (const auto& [id, disc] : std::vector<std::pair<std::string, std::string>>{
             {"ridge-racer-psp", "UCES00002"}, {"ridge-racer-2-psp", "UCES00422"}}) {
        const fs::path core = installs / "cores" / "ppsspp";
        writeFile(core, "fixture");
        install(b, "lr-ppsspp", core.parent_path(), core, false);
        gameFiles(b, id, games / "psp.iso", "iso");
        Run r = launch(b, id, "lr-ppsspp", def, onePad,
                       {{"aspect", "31:9"}, {"extra", {{"discId", disc}}}});
        CHECK(r.result.coreOptions["ppsspp_cheats"] == "enabled");
        CHECK(std::abs(std::stod(r.result.presentAspect) - 31.0 / 9.0) < 0.00001);
        r = launch(b, id, "lr-ppsspp", def, onePad,
                   {{"aspect", "31:9"}, {"extra", {{"discId", "ULUS10001"}}}});
        CHECK(r.result.presentAspect == "auto"); // unsupported region is never merely stretched
    }
    {
        const json ultra = {{"aspect", "31:9"}, {"fullscreen", true}, {"resolution", "native"}};
        Run r = launch(b, "ridge-racer-v", "pcsx2", def, onePad, ultra);
        CHECK_HAS(r.log, "Marathon Widescreen 31:9");
        r = launch(b, "ridge-racer-6", "rr6-recomp", def, onePad, ultra, {3100, 900, 60, 1.0f});
        CHECK_HAS(r.commandLine, "--rr6_aspect_ratio=3.444");
        CHECK_HAS(r.commandLine, "--rr6_hud_fix=true");
        r = launch(b, "ridge-racer-6", "rr6-recomp", def, onePad, ultra, {1920, 1080, 60, 1.0f});
        CHECK_HAS(r.commandLine, "--rr6_aspect_ratio=0\n");
        CHECK_HAS(r.commandLine, "--present_letterbox=true");
    }
}

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    // SDL and the test can use separate CRT environment tables on Windows.
    // The recomp adapters read std::getenv(), so update this CRT as well.
    CHECK(_putenv_s(name, value.c_str()) == 0);
#endif
    CHECK(SDL_setenv_unsafe(name, value.c_str(), 1) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string catalogDir, workDir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--catalog" && i + 1 < argc) catalogDir = argv[++i];
        else if (a == "--work" && i + 1 < argc) workDir = argv[++i];
        else {
            std::fprintf(stderr, "usage: controller_configs_test [--catalog DIR] [--work DIR]\n");
            return 2;
        }
    }
    std::error_code ec;
    if (catalogDir.empty())
        catalogDir = fs::exists("catalog/games.json", ec) ? "catalog"
                                                          : pathToUtf8(fs::path(__FILE__).parent_path().parent_path() / "catalog");
    if (workDir.empty()) {
        const fs::path base = currentOs() == Os::Windows ? fs::temp_directory_path(ec) : fs::path("/tmp");
        workDir = pathToUtf8(base / ("rrm-ctl-configs-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())));
    }
    const fs::path work = utf8ToPath(workDir);
    fs::create_directories(work, ec);
    // Programs that keep files in the user's home (Fukami) look inside the test folder instead.
    const std::string home = pathToUtf8(work / "home");
    for (const char* name : {"HOME", "CFFIXED_USER_HOME", "USERPROFILE"}) setEnv(name, home);
    setEnv("XDG_DATA_HOME", home + "/.local/share");
    setEnv("XDG_CONFIG_HOME", home + "/.config");
    setEnv("LOCALAPPDATA", home + "/AppData/Local");
    setEnv("FUKAMI_DATA_DIR", home + "/Fukami");
    setEnv("RRM_RECOMP_DRY_RUN", "1");
    const char* recompDryRun = std::getenv("RRM_RECOMP_DRY_RUN");
    if (!recompDryRun || std::string(recompDryRun) != "1") {
        std::fprintf(stderr, "Cannot enable recomp dry runs; refusing to launch test fixtures.\n");
        return 1;
    }
    emu::dryRun() = true;
    initPaths(pathToUtf8(work / "data").c_str());
    std::printf("work folder: %s\ncatalog: %s\n", workDir.c_str(), catalogDir.c_str());

    testIni();
    testRpcs3();
    testPcsx2();
    testPpsspp();
    testOthers();
    testAdapters(work, catalogDir);

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
