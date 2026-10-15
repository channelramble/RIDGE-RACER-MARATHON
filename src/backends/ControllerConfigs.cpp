// Controller configuration for the external backends. The formats and codes come from each
// program's own source (see the adapters' header comments and docs/CONTROLLERS.md).
#include "backends/ControllerConfigs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace rrm::ctl {

namespace {

bool isGuid(const std::string& s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char ch) {
               return std::isdigit(static_cast<unsigned char>(ch)) || (ch >= 'a' && ch <= 'f');
           });
}

std::string lowerCopy(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// A YAML scalar, quoted the way RPCS3 writes its files: empty values, alternatives (",") and
// combinations ("&") in double quotes, and anything YAML would otherwise read differently.
std::string yamlValue(const std::string& v) {
    static const std::string indicators = "-?:,[]{}#&*!|>'\"%@`";
    const std::string low = lowerCopy(v);
    bool quote = v.empty() || v.find_first_of(",&") != std::string::npos || v.front() == ' ' || v.back() == ' ' ||
                 v.find(": ") != std::string::npos || v.find(" #") != std::string::npos || v.back() == ':' ||
                 low == "null" || low == "~" || low == "true" || low == "false" || low == "yes" || low == "no" ||
                 low == "on" || low == "off";
    if (!quote && indicators.find(v.front()) != std::string::npos)
        quote = !(v.front() == '-' && v.size() > 1 && std::isdigit(static_cast<unsigned char>(v[1])));
    if (!quote) return v;
    std::string out = "\"";
    for (char ch : v) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    return out + "\"";
}

std::string tomlString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
            out += buf;
        } else {
            out += static_cast<char>(ch);
        }
    }
    return out + "\"";
}

std::vector<std::string> splitCodes(const std::string& value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) comma = value.size();
        if (comma > start) out.push_back(value.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

std::string joinCodes(const std::vector<std::string>& codes) {
    std::string out;
    for (auto& code : codes) out += (out.empty() ? "" : ",") + code;
    return out;
}

// ---- RPCS3 -----------------------------------------------------------------------------------

// One player's Config block (the SDL handler's button names), in RPCS3's own key order.
KeyValues rpcs3PadConfig(const ControllerSettings& c) {
    const bool dpad = c.steering == "dpad", triggers = c.pedals == "triggers";
    const std::string deadzone = std::to_string(deadzoneUnits(c.deadzone));
    const std::string motor = c.vibration ? "100" : "0";
    return {
        // Digital steering: the left stick works as the d-pad and the PS3 stick stays centred.
        {"Left Stick Left", dpad ? "" : "LS X-"},
        {"Left Stick Down", dpad ? "" : "LS Y-"},
        {"Left Stick Right", dpad ? "" : "LS X+"},
        {"Left Stick Up", dpad ? "" : "LS Y+"},
        {"Right Stick Left", "RS X-"},
        {"Right Stick Down", "RS Y-"},
        {"Right Stick Right", "RS X+"},
        {"Right Stick Up", "RS Y+"},
        {"Start", "Start"},
        {"Select", "Back"},
        {"PS Button", "Guide,Start&Back"},
        // A button bound to a trigger takes the trigger's travel as its pressure, so RT and LT
        // are analog accelerator and brake through Cross and Square.
        {"Square", triggers ? "West,LT" : "West"},
        {"Cross", triggers ? "South,RT" : "South"},
        {"Circle", "East"},
        {"Triangle", "North"},
        {"Left", dpad ? "Left,LS X-" : "Left"},
        {"Down", dpad ? "Down,LS Y-" : "Down"},
        {"Right", dpad ? "Right,LS X+" : "Right"},
        {"Up", dpad ? "Up,LS Y+" : "Up"},
        // Ridge Racer 7: R1/L1 shift up/down, R2 nitrous, L2 double nitrous. With the pedals on the
        // triggers, nitrous moves to the bumpers and shifting to the stick clicks.
        {"R1", triggers ? "RS" : "RB"},
        {"R2", triggers ? "RB" : "RT"},
        {"R3", triggers ? "" : "RS"},
        {"L1", triggers ? "LS" : "LB"},
        {"L2", triggers ? "LB" : "LT"},
        {"L3", triggers ? "" : "LS"},
        {"Left Stick Deadzone", deadzone},
        {"Right Stick Deadzone", deadzone},
        {"Large Vibration Motor Multiplier", motor},
        {"Small Vibration Motor Multiplier", motor},
    };
}

// ---- PCSX2 -----------------------------------------------------------------------------------

// Replaces the SDL values of one binding and keeps the others in front of them.
void bindPcsx2(cfg::Ini& ini, const std::string& section, const std::string& key, const std::vector<std::string>& sdl) {
    std::vector<std::string> values;
    for (auto& v : ini.getList(section, key))
        if (v.rfind("SDL-", 0) != 0) values.push_back(v);
    values.insert(values.end(), sdl.begin(), sdl.end());
    ini.setList(section, key, values);
}

void pcsx2Pad(cfg::Ini& ini, int index, const ControllerSettings& c) {
    const std::string section = "Pad" + std::to_string(index + 1);
    const std::string dev = "SDL-" + std::to_string(index) + "/";
    const bool triggers = c.pedals == "triggers";
    auto bind = [&](const std::string& key, std::initializer_list<const char*> inputs) {
        std::vector<std::string> sdl;
        for (const char* in : inputs) sdl.push_back(dev + in);
        bindPcsx2(ini, section, key, sdl);
    };
    ini.set(section, "Type", "DualShock2");
    bind("Up", {"DPadUp"});
    bind("Right", {"DPadRight"});
    bind("Down", {"DPadDown"});
    bind("Left", {"DPadLeft"});
    bind("Triangle", {"FaceNorth"});
    bind("Circle", {"FaceEast"});
    // A trigger bound to a face button gives it the trigger's travel as pressure (Ridge Racer V
    // reads Cross and Square pressure-sensitive): analog accelerator and brake.
    if (triggers) {
        bind("Cross", {"FaceSouth", "+RightTrigger"});
        bind("Square", {"FaceWest", "+LeftTrigger"});
    } else {
        bind("Cross", {"FaceSouth"});
        bind("Square", {"FaceWest"});
    }
    bind("Select", {"Back"});
    bind("Start", {"Start"});
    bind("L1", {"LeftShoulder"});
    bind("R1", {"RightShoulder"});
    if (triggers) {
        bind("L2", {});
        bind("R2", {"RightStick"});  // rear view
        bind("L3", {});
        bind("R3", {});
    } else {
        bind("L2", {"+LeftTrigger"});
        bind("R2", {"+RightTrigger"});
        bind("L3", {"LeftStick"});
        bind("R3", {"RightStick"});
    }
    // Never Analog: the game switches the pad to analog itself, and toggling it off would stop the stick.
    bind("Analog", {});
    bind("LUp", {"-LeftY"});
    bind("LRight", {"+LeftX"});
    bind("LDown", {"+LeftY"});
    bind("LLeft", {"-LeftX"});
    bind("RUp", {"-RightY"});
    bind("RRight", {"+RightX"});
    bind("RDown", {"+RightY"});
    bind("RLeft", {"-RightX"});
    if (c.vibration) {
        bind("LargeMotor", {"LargeMotor"});
        bind("SmallMotor", {"SmallMotor"});
    } else {
        bind("LargeMotor", {});
        bind("SmallMotor", {});
    }
    ini.set(section, "Deadzone", fraction(c.deadzone));
}

// ---- PPSSPP ----------------------------------------------------------------------------------

// PPSSPP 1.20's codes: <device>-<key>, device 1 the keyboard, 10 an SDL pad (macOS, Linux) or a
// DirectInput pad (Windows), 20 an XInput pad; axes are 4000 + 2 x axis (+1 for the minus half).
// SDL: South 189, East 190, West 191, North 188, RB 192, LB 193, Back 196, Start 197, Guide 4,
// d-pad 19-22, left stick 4000-4003, LT 4008, RT 4010. XInput: A 96, B 97, X 99, Y 100, LB 102,
// RB 103, Back 109, Start 108, Guide 3, R3 107, stick up 4002, LT 4034, RT 4036. The DirectInput
// lines fit PlayStation pads (DualShock 4 / DualSense without Steam Input).
const KeyValues kPpssppSdl = {
    {"Up", "1-19,10-19"},
    {"Down", "1-20,10-20"},
    {"Left", "1-21,10-21"},
    {"Right", "1-22,10-22"},
    {"Circle", "1-52,10-190"},
    {"Cross", "1-54,10-189,10-4010"},
    {"Square", "1-29,10-191,10-4008"},
    {"Triangle", "1-47,10-188"},
    {"Start", "1-62,10-197"},
    {"Select", "1-66,10-196"},
    {"L", "1-45,10-193"},
    {"R", "1-51,10-192"},
    {"An.Up", "1-37,10-4003"},
    {"An.Down", "1-39,10-4002"},
    {"An.Left", "1-38,10-4001"},
    {"An.Right", "1-40,10-4000"},
    {"RapidFire", "1-59"},
    {"Fast-forward", "1-61"},
    {"SpeedToggle", "1-68"},
    {"Pause", "1-111,10-4"},
    {"Rewind", "1-67"},
    {"Analog limiter", "1-60"},
    {"Toggle Debugger", "1-142"},
    {"Pause (no menu)", "1-138"},
};
const KeyValues kPpssppWindows = {
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

std::string* findValue(KeyValues& kv, const std::string& key) {
    for (auto& [k, v] : kv)
        if (k == key) return &v;
    return nullptr;
}

}  // namespace

// ---- shared ----------------------------------------------------------------------------------

std::string noteLine(const ControllerSettings& c, const std::string& what) {
    return "controllers: steering=" + c.steering + " pedals=" + c.pedals + " vibration=" + (c.vibration ? "on" : "off") +
           " deadzone=" + std::to_string(c.deadzone) + "% -> " + what;
}

std::string linkedNote(const std::string& program) {
    return "controllers: " + program + " keeps its own controller settings (linked install)";
}

std::string fraction(int percent, bool decimalPoint) {
    percent = std::max(0, percent);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d.%02d", percent / 100, percent % 100);
    std::string s = buf;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') {
        if (decimalPoint) s += '0';
        else s.pop_back();
    }
    return s;
}

// ---- RPCS3 -----------------------------------------------------------------------------------

std::string rpcs3DeviceName(const ConnectedPad& pad) { return pad.name + " " + std::to_string(std::max(1, pad.nameIndex)); }

std::string rpcs3InputConfig(const ControllerSettings& c, const std::vector<ConnectedPad>& pads, bool keyboardCopilot) {
    const ConnectedPad* player[2] = {nullptr, nullptr};
    for (auto& p : pads)
        if (p.player >= 0 && p.player < 2 && !player[p.player]) player[p.player] = &p;
    std::string out =
        "# Written by Ridge Racer Marathon before each launch, from its controller settings.\n"
        "# Keys that are not listed take RPCS3's defaults for the handler.\n";
    auto line = [&](int indent, const std::string& key, const std::string& value) {
        out += std::string(static_cast<size_t>(indent), ' ') + key + ": " + yamlValue(value) + "\n";
    };
    const KeyValues config = rpcs3PadConfig(c);
    for (int i = 0; i < 7; ++i) {
        out += "Player " + std::to_string(i + 1) + " Input:\n";
        if (i < 2 && player[i]) {
            line(2, "Handler", "SDL");
            line(2, "Device", rpcs3DeviceName(*player[i]));
            out += "  Config:\n";
            for (auto& [k, v] : config) line(4, k, v);
        } else if (i == 0) {
            line(2, "Handler", "Keyboard");  // RPCS3's own keyboard layout
            line(2, "Device", "Keyboard");
        } else if (i == 6 && keyboardCopilot && player[0]) {
            line(2, "Handler", "Keyboard");
            line(2, "Device", "Keyboard");
            out += "  Config:\n";
            line(4, "Device Class Type", "40964");  // 0xa004: copilot for Player 1
        } else {
            line(2, "Handler", "Null");
        }
    }
    return out;
}

// ---- PCSX2 -----------------------------------------------------------------------------------

void applyPcsx2Controllers(cfg::Ini& ini, const ControllerSettings& c) {
    ini.set("InputSources", "SDL", "true");
    pcsx2Pad(ini, 0, c);
    if (c.players[1].controller == "none") ini.set("Pad2", "Type", "None");
    else pcsx2Pad(ini, 1, c);
    // PCSX2's pause menu (Close Game) from the pad, next to Esc.
    bindPcsx2(ini, "Hotkeys", "OpenPauseMenu", {"SDL-0/Back & SDL-0/Start"});
}

// ---- PPSSPP ----------------------------------------------------------------------------------

KeyValues ppssppControlMapping(const ControllerSettings& c, bool windows) {
    KeyValues kv = windows ? kPpssppWindows : kPpssppSdl;
    if (c.pedals != "triggers") {
        // The game's own layout: the triggers are no accelerator and brake.
        const std::vector<std::string> drop =
            windows ? std::vector<std::string>{"20-4036", "10-195", "20-4034", "10-194"}
                    : std::vector<std::string>{"10-4010", "10-4008"};
        for (const char* key : {"Cross", "Square"}) {
            std::string* v = findValue(kv, key);
            std::vector<std::string> codes = splitCodes(*v);
            codes.erase(std::remove_if(codes.begin(), codes.end(),
                                       [&](const std::string& code) {
                                           return std::find(drop.begin(), drop.end(), code) != drop.end();
                                       }),
                        codes.end());
            *v = joinCodes(codes);
        }
    }
    if (c.steering == "dpad") {
        // Digital steering: the stick presses the d-pad and the PSP's analog nub is left to the keyboard.
        const char* pairs[][2] = {{"An.Up", "Up"}, {"An.Down", "Down"}, {"An.Left", "Left"}, {"An.Right", "Right"}};
        for (auto& p : pairs) {
            std::string* analog = findValue(kv, p[0]);
            std::string* dpad = findValue(kv, p[1]);
            std::vector<std::string> keep, moved;
            for (auto& code : splitCodes(*analog)) (code.rfind("1-", 0) == 0 ? keep : moved).push_back(code);
            *analog = joinCodes(keep);
            std::vector<std::string> codes = splitCodes(*dpad);
            codes.insert(codes.end(), moved.begin(), moved.end());
            *dpad = joinCodes(codes);
        }
    }
    return kv;
}

std::string ppssppProfile(const ControllerSettings& c) { return c.steering + "/" + c.pedals; }

bool ppssppMappingDue(const cfg::Ini& ini, const ControllerSettings& c) {
    if (!ini.hasSection("Marathon") || !ini.hasSection("ControlMapping")) return true;
    if (std::atoi(ini.get("Marathon", "ControlsVersion", "0").c_str()) < kPpssppControlsVersion) return true;
    return ini.get("Marathon", "Profile") != ppssppProfile(c);
}

void applyPpssppMapping(cfg::Ini& ini, const ControllerSettings& c, bool windows) {
    ini.replaceSection("ControlMapping", ppssppControlMapping(c, windows));
    ini.set("Marathon", "ControlsVersion", std::to_string(kPpssppControlsVersion));
    ini.set("Marathon", "Profile", ppssppProfile(c));
}

// ---- MAME ------------------------------------------------------------------------------------

std::string mameControllerFile(const std::string& setName) {
    return "<?xml version=\"1.0\"?>\n"
           "<mameconfig version=\"10\">\n"
           "  <system name=\"default\">\n"
           "    <input>\n"
           "      <port type=\"UI_MENU\"><newseq type=\"standard\">KEYCODE_TAB OR JOYCODE_1_SELECT JOYCODE_1_START</newseq></port>\n"
           "      <port type=\"UI_CANCEL\"><newseq type=\"standard\">KEYCODE_ESC OR JOYCODE_1_SELECT JOYCODE_1_BUTTON2</newseq></port>\n"
           "    </input>\n"
           "  </system>\n"
           "  <system name=\"" + setName + "\">\n"
           "    <input>\n"
           "      <port type=\"P1_JOYSTICK_UP\"><newseq type=\"standard\">KEYCODE_UP OR JOYCODE_1_HAT1UP OR JOYCODE_1_BUTTON5</newseq></port>\n"
           "      <port type=\"P1_JOYSTICK_DOWN\"><newseq type=\"standard\">KEYCODE_DOWN OR JOYCODE_1_HAT1DOWN OR JOYCODE_1_BUTTON6</newseq></port>\n"
           "    </input>\n"
           "  </system>\n"
           "</mameconfig>\n";
}

std::vector<std::string> mameControllerArgs(const ControllerSettings& c, const std::string& ctrlrDir) {
    return {"-joystick", "-joystick_deadzone", fraction(c.deadzone), "-ctrlrpath", ctrlrDir, "-ctrlr", "marathon"};
}

// ---- Xenia -----------------------------------------------------------------------------------

KeyValues xeniaHidKeys(const ControllerSettings& c) {
    const std::string deadzone = fraction(c.deadzone, true);  // a TOML float: 0.0 .. 1.0
    return {
        {"HID.hid", "\"any\""},  // SDL pads and the keyboard (never XInput only)
        {"HID.vibration", c.vibration ? "true" : "false"},
        {"HID.left_stick_deadzone_percentage", deadzone},
        {"HID.right_stick_deadzone_percentage", deadzone},
    };
}

// ---- PSXRecomp -------------------------------------------------------------------------------

std::string psxDevice(const PlayerChoice& choice, const std::optional<std::string>& current, const std::string& lastWritten,
                      bool nobodyAllowed, bool keyboardToGamepad) {
    if (isGuid(choice.controller)) return choice.controller;
    if (choice.controller == "none" && nobodyAllowed) return "none";
    if (!current) return "gamepad";
    if (keyboardToGamepad && *current == "keyboard") return "gamepad";
    if (!lastWritten.empty() && *current == lastWritten) return "gamepad";
    return *current;
}

KeyValues psxControllerKeys(const ControllerSettings& c, bool r4, const std::optional<std::string>& currentP1,
                            const std::optional<std::string>& currentP2, const std::string& lastP1,
                            const std::string& lastP2) {
    KeyValues kv;
    kv.emplace_back("controller.p1_device", tomlString(psxDevice(c.players[0], currentP1, lastP1, false, true)));
    if (!r4) return kv;  // Ridge Racer locks its pad to digital and has one player
    kv.emplace_back("controller.p2_device", tomlString(psxDevice(c.players[1], currentP2, lastP2, true, false)));
    const std::string mode = tomlString(c.steering == "dpad" ? "digital" : "analog");
    kv.emplace_back("controller.p1_mode", mode);
    kv.emplace_back("controller.p2_mode", mode);
    return kv;
}

std::vector<ModSetting> r4ControllerMods(const ControllerSettings& c) {
    const bool modern = c.steering == "stick" && c.pedals == "triggers";
    return {
        {"r4.modern-controls", "modern-controls", true, {{"scheme", modern ? "modern" : "classic"}}},
        {"r4.modern-controls", "rumble", c.vibration, {}},
        {"r4.modern-controls", "controls-tuning", true, {{"stick_deadzone", std::to_string(c.deadzone)}}},
    };
}

// ---- Fukami ----------------------------------------------------------------------------------

std::vector<IniKey> fukamiInputKeys(const ControllerSettings& c) {
    std::vector<IniKey> keys;
    // Off would make the game see a digital pad and ignore the stick, so it is only ever switched on.
    if (c.steering == "stick") keys.push_back({"input", "analog", "true"});
    keys.push_back({"input", "rumble", c.vibration ? "true" : "false"});
    return keys;
}

// ---- RR6 recomp ------------------------------------------------------------------------------

std::vector<std::string> rr6ControllerFlags(const std::string& mappingsFile, bool logInput) {
    std::vector<std::string> flags;
    if (!mappingsFile.empty()) flags.push_back("--hid_mappings_file=" + mappingsFile);
    if (logInput) flags.push_back("--rr6_log_input=true");
    return flags;
}

// ---- Rage Racer PC ---------------------------------------------------------------------------

std::vector<std::string> rageInputSets(const ControllerSettings& c) {
    return {std::string("input.analog=") + (c.steering == "stick" ? "true" : "false"),
            "input.steering_deadzone=" + fraction(c.deadzone)};
}

// ---- Rave Racer native -----------------------------------------------------------------------

KeyValues raveCfgKeys(const ControllerSettings& c) { return {{"pad_deadzone", std::to_string(deadzoneUnits(c.deadzone))}}; }

}  // namespace rrm::ctl
