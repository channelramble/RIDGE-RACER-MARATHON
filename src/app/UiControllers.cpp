// Gamepads in Marathon's own UI: button glyphs, the footer hints, the buttons the UI acts on
// itself (back, tabs, race, game settings), the Controllers settings page with its test and the
// game page's CONTROLS panel. docs/CONTROLLERS.md describes all of it for players.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "app/Ui.h"
#include "core/Controllers.h"
#include "core/Platform.h"
#include "imgui_internal.h"

namespace rrm::ui {

namespace {

const ImU32 kYellow = IM_COL32(245, 196, 0, 255);
const ImU32 kAmber = IM_COL32(255, 166, 0, 255);
const ImU32 kGlyphBg = IM_COL32(38, 45, 62, 255);
const ImU32 kGlyphText = IM_COL32(235, 238, 245, 255);
const char* const kDot = " \xC2\xB7 ";  // middle dot

bool g_focusRequest = false;
uint32_t g_testPick = 0;          // TEST panel choice (0 = the pad used last)
uint32_t g_testFor = 0;           // the pad the test state below belongs to
uint64_t g_backHeldSince = 0;     // test: when its Back button went down
bool g_guideSeen = false;         // test: a Guide press reached Marathon
int g_testScroll = 0;             // test: frames until the panel is scrolled into view

// ---- glyphs ----------------------------------------------------------------------------------

// A representative SDL type per family, for SDL's button labels.
SDL_GamepadType familyType(const std::string& family) {
    if (family == "playstation") return SDL_GAMEPAD_TYPE_PS5;
    if (family == "nintendo") return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
    return SDL_GAMEPAD_TYPE_XBOXONE;  // Xbox and generic pads: Xbox names
}

// The label of the face button that confirms, goes back or opens settings on this family:
// A / Cross, B / Circle, Y / Triangle (Nintendo pads have A on the right and Y on the left).
SDL_GamepadButtonLabel faceLabel(PadGlyph glyph, const std::string& family) {
    const SDL_GamepadType type = familyType(family);
    const bool nintendo = SDL_GetGamepadButtonLabelForType(type, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_B;
    SDL_GamepadButton button = nintendo ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH;
    if (glyph == PadGlyph::Confirm) button = nintendo ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
    else if (glyph == PadGlyph::Back) button = nintendo ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
    return SDL_GetGamepadButtonLabelForType(type, button);
}

const char* pillText(PadGlyph g, const std::string& family) {
    const bool ps = family == "playstation", nin = family == "nintendo";
    switch (g) {
        case PadGlyph::LB: return ps ? "L1" : nin ? "L" : "LB";
        case PadGlyph::RB: return ps ? "R1" : nin ? "R" : "RB";
        case PadGlyph::LT: return ps ? "L2" : nin ? "ZL" : "LT";
        case PadGlyph::RT: return ps ? "R2" : nin ? "ZR" : "RT";
        case PadGlyph::Start: return ps ? "Options" : nin ? "+" : "Menu";
        case PadGlyph::Guide: return ps ? "PS" : nin ? "Home" : "Xbox";
        case PadGlyph::BackButton: return ps ? "Create" : nin ? "-" : "View";
        default: return "";
    }
}

bool isFace(PadGlyph g) { return g == PadGlyph::Confirm || g == PadGlyph::Back || g == PadGlyph::North; }

// Face button with a given label (PlayStation symbols are drawn: the fonts have no such glyphs).
void drawFace(ImDrawList* dl, ImVec2 c, float size, SDL_GamepadButtonLabel label, bool nintendo, bool lit = false) {
    const float r = size * 0.5f, s = r * 0.48f, th = std::max(1.5f, size * 0.09f);
    dl->AddCircleFilled(c, r, lit ? kYellow : kGlyphBg, 24);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 40), 24, 1.0f);
    const ImU32 dark = IM_COL32(20, 22, 30, 255);
    switch (label) {
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: {
            const ImU32 col = lit ? dark : IM_COL32(122, 166, 232, 255);
            dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, th);
            dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), col, th);
            return;
        }
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: dl->AddCircle(c, s, lit ? dark : IM_COL32(232, 106, 106, 255), 20, th); return;
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
            dl->AddRect(ImVec2(c.x - s * 0.9f, c.y - s * 0.9f), ImVec2(c.x + s * 0.9f, c.y + s * 0.9f),
                        lit ? dark : IM_COL32(217, 140, 200, 255), 0, 0, th);
            return;
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
            dl->AddTriangle(ImVec2(c.x, c.y - s * 1.05f), ImVec2(c.x - s * 1.1f, c.y + s * 0.8f), ImVec2(c.x + s * 1.1f, c.y + s * 0.8f),
                            lit ? dark : IM_COL32(64, 192, 160, 255), th);
            return;
        default: break;
    }
    const char* text = "?";
    ImU32 col = kGlyphText;
    switch (label) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: text = "A"; col = IM_COL32(108, 194, 74, 255); break;
        case SDL_GAMEPAD_BUTTON_LABEL_B: text = "B"; col = IM_COL32(226, 70, 61, 255); break;
        case SDL_GAMEPAD_BUTTON_LABEL_X: text = "X"; col = IM_COL32(59, 142, 234, 255); break;
        case SDL_GAMEPAD_BUTTON_LABEL_Y: text = "Y"; col = IM_COL32(245, 196, 0, 255); break;
        default: break;
    }
    if (nintendo) col = kGlyphText;  // Nintendo's letters are all the same colour
    if (lit) col = dark;
    ImFont* f = app().fonts.bodyBold;
    const float fs = size * 0.62f;
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, text);
    dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, text);
}

// A rounded label button (shoulders, triggers, Start, Guide, Back, any named button).
float pillWidth(const char* text, float size) {
    const ImVec2 ts = app().fonts.bodyBold->CalcTextSizeA(size * 0.52f, FLT_MAX, 0, text);
    return std::max(size * 0.82f, ts.x + size * 0.5f);
}

void drawPill(ImDrawList* dl, ImVec2 c, float size, const char* text, bool lit = false) {
    ImFont* f = app().fonts.bodyBold;
    const float h = size * 0.82f, fs = size * 0.52f, w = pillWidth(text, size);
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, text);
    dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), lit ? kYellow : kGlyphBg, h * 0.3f);
    dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), lit ? IM_COL32(20, 22, 30, 255) : kGlyphText, text);
}

// ---- footer hints ------------------------------------------------------------------------------

std::vector<std::pair<PadGlyph, const char*>> hintsFor(App& app) {
    using G = PadGlyph;
    if (app.padTestActive) return {{G::Back, "Hold to stop testing"}};
    switch (app.screen) {
        case Screen::Library:
            return {{G::Confirm, "Open"}, {G::Start, "Race!"}, {G::North, "Game settings"}, {G::LB, nullptr}, {G::RB, "Tabs"}};
        case Screen::Game:
            return {{G::Confirm, "Select"}, {G::Back, "Back"},    {G::Start, "Race!"},
                    {G::North, "Settings"}, {G::LB, nullptr},     {G::RB, "Tabs"}};
        case Screen::Settings:
            return {{G::Confirm, "Select"}, {G::Back, "Back"},  {G::LT, nullptr},
                    {G::RT, "General / Controllers"},           {G::LB, nullptr}, {G::RB, "Tabs"}};
        default: return {{G::Confirm, "Select"}, {G::Back, "Back"}, {G::LB, nullptr}, {G::RB, "Tabs"}};
    }
}

// ---- Controllers page ---------------------------------------------------------------------------

struct Choice {
    const char* value;
    const char* label;
};

const Choice kSteering[] = {{"stick", "Left stick (analog)"}, {"dpad", "D-pad (digital)"}};
const Choice kPedals[] = {{"triggers", "Triggers (RT gas, LT brake)"}, {"buttons", "Buttons (the game's own layout)"}};
const Choice kChords[] = {{"guide-start", "Hold Guide + Start"}, {"back-start", "Hold Back + Start"}, {"off", "Off"}};

template <size_t N>
bool choiceCombo(const char* label, std::string& value, const Choice (&choices)[N]) {
    const char* current = value.c_str();
    for (auto& c : choices)
        if (value == c.value) current = c.label;
    bool changed = false;
    if (ImGui::BeginCombo(label, current)) {
        for (auto& c : choices) {
            const bool sel = value == c.value;
            if (ImGui::Selectable(c.label, sel)) {
                value = c.value;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::string displayName(const ConnectedPad& p) {
    return p.nameIndex > 1 ? p.name + " " + std::to_string(p.nameIndex) : p.name;
}

std::string chordText(const ControllerSettings& c) {
    if (c.quitChord == "off") return "";
    const char* keys = c.quitChord == "back-start" ? "Back + Start" : "Guide + Start";
    char buf[96];
    if (c.quitHoldMs % 1000 == 0)
        snprintf(buf, sizeof(buf), "hold %s for %d second%s", keys, c.quitHoldMs / 1000, c.quitHoldMs == 1000 ? "" : "s");
    else snprintf(buf, sizeof(buf), "hold %s for %.1f seconds", keys, c.quitHoldMs / 1000.0);
    return buf;
}

bool playerCombo(int player, PlayerChoice& pc, const std::vector<ConnectedPad>& pads) {
    const char* autoLabel = player == 0 ? "Automatic (first controller)" : "Automatic (second controller)";
    auto connectedModel = [&](const std::string& guid) {
        return std::any_of(pads.begin(), pads.end(), [&](const ConnectedPad& p) { return p.guid == guid; });
    };
    const bool chosen = !pc.controller.empty() && pc.controller != "none";
    std::string preview = pc.controller.empty() ? autoLabel : pc.controller == "none" ? "Nobody" : pc.name.empty() ? "Controller" : pc.name;
    if (chosen && !connectedModel(pc.controller)) preview += " (not connected)";
    bool changed = false;
    if (ImGui::BeginCombo(player == 0 ? "Player 1" : "Player 2", preview.c_str())) {
        if (ImGui::Selectable(autoLabel, pc.controller.empty())) {
            pc = {};
            changed = true;
        }
        if (player > 0 && ImGui::Selectable("Nobody", pc.controller == "none")) {
            pc = {"none", ""};
            changed = true;
        }
        // One entry per model: a choice is recognised by its model (SDL GUID), not by the unit.
        std::vector<std::string> listed;
        for (auto& p : pads) {
            if (p.guid.empty() || std::find(listed.begin(), listed.end(), p.guid) != listed.end()) continue;
            listed.push_back(p.guid);
            const auto same = std::count_if(pads.begin(), pads.end(), [&](const ConnectedPad& q) { return q.guid == p.guid; });
            std::string label = p.name + (same > 1 ? " (" + std::to_string(same) + " connected)" : std::string()) + "##" + p.guid;
            if (ImGui::Selectable(label.c_str(), pc.controller == p.guid)) {
                pc = {p.guid, p.name};
                changed = true;
            }
        }
        if (chosen && !connectedModel(pc.controller)) ImGui::Selectable((preview + "##remembered").c_str(), true);
        ImGui::EndCombo();
    }
    return changed;
}

void startTest(App& app, uint32_t id) {
    app.padTestActive = true;
    app.padTestId = id;
    g_testFor = id;
    g_backHeldSince = 0;
    g_guideSeen = false;
    g_testScroll = 2;  // (on the second frame, once the page knows how tall the test made it)
}

void stopTest(App& app) {
    app.padTestActive = false;
    g_backHeldSince = 0;
}

std::string buttonName(SDL_Gamepad* gp, SDL_GamepadButton b, const std::string& family) {
    switch (SDL_GetGamepadButtonLabel(gp, b)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default: break;
    }
    const bool ps = family == "playstation";
    switch (b) {
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return pillText(PadGlyph::LB, family);
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return pillText(PadGlyph::RB, family);
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ps ? "L3" : "LS";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ps ? "R3" : "RS";
        case SDL_GAMEPAD_BUTTON_BACK: return pillText(PadGlyph::BackButton, family);
        case SDL_GAMEPAD_BUTTON_START: return pillText(PadGlyph::Start, family);
        case SDL_GAMEPAD_BUTTON_GUIDE: return pillText(PadGlyph::Guide, family);
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "Up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "Down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "Left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "Right";
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
        case SDL_GAMEPAD_BUTTON_MISC1: return "Misc";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "Left paddle 1";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "Left paddle 2";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "Right paddle 1";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "Right paddle 2";
        default: break;
    }
    if (b >= SDL_GAMEPAD_BUTTON_MISC2 && b <= SDL_GAMEPAD_BUTTON_MISC6)
        return "Misc " + std::to_string(2 + (b - SDL_GAMEPAD_BUTTON_MISC2));
    const char* s = SDL_GetGamepadStringForButton(b);
    return s ? s : "?";
}

// The button labelled B (Circle): the bottom one on Nintendo pads.
SDL_GamepadButton backButtonOf(SDL_Gamepad* gp) {
    const SDL_GamepadButtonLabel south = SDL_GetGamepadButtonLabel(gp, SDL_GAMEPAD_BUTTON_SOUTH);
    return south == SDL_GAMEPAD_BUTTON_LABEL_B || south == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE ? SDL_GAMEPAD_BUTTON_SOUTH
                                                                                            : SDL_GAMEPAD_BUTTON_EAST;
}

void drawStick(ImDrawList* dl, ImVec2 c, float r, int16_t x, int16_t y, int dz) {
    int16_t ax = x, ay = y;
    applyStickDeadzone(ax, ay, dz);
    dl->AddCircleFilled(c, r, IM_COL32(22, 27, 39, 255), 40);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 60), 40, 1.5f);
    if (dz > 0) dl->AddCircleFilled(c, r * dz / 32767.0f, IM_COL32(255, 166, 0, 40), 32);
    if (dz > 0) dl->AddCircle(c, r * dz / 32767.0f, IM_COL32(255, 166, 0, 140), 32, 1.0f);
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(255, 255, 255, 18));
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), IM_COL32(255, 255, 255, 18));
    auto at = [&](int16_t vx, int16_t vy) { return ImVec2(c.x + r * vx / 32767.0f, c.y + r * vy / 32767.0f); };
    dl->AddCircleFilled(at(x, y), S(6), IM_COL32(150, 158, 176, 255), 16);
    dl->AddCircleFilled(at(ax, ay), S(5), kYellow, 16);
}

void axisLine(const char* name, int16_t raw, int16_t after) {
    ImGui::Text("%s %+.2f (after dead zone %+.2f)", name, raw / 32767.0, after / 32767.0);
}

void drawPadTest(App& app) {
    SDL_Gamepad* gp = app.gamepads().gamepad(app.padTestId);
    if (!gp) {
        stopTest(app);
        app.toast("The controller was disconnected, so the test stopped.");
        return;
    }
    if (g_testFor != app.padTestId) startTest(app, app.padTestId);
    ConnectedPad info;
    for (auto& p : app.padSnapshot())
        if (p.id == app.padTestId) info = p;
    const std::string family = padFamily(info.type);
    const ControllerSettings& c = app.global.controllers;
    const uint64_t now = SDL_GetTicks();
    const ImVec2 start = ImGui::GetCursorScreenPos();

    // Stop: its Back button (by label) held for 1 s, Esc, or the Stop button.
    const SDL_GamepadButton back = backButtonOf(gp);
    float backHeld = 0;
    if (SDL_GetGamepadButton(gp, back)) {
        if (!g_backHeldSince) g_backHeldSince = now;
        backHeld = std::min(1.0f, (now - g_backHeldSince) / 1000.0f);
        if (now - g_backHeldSince >= 1000) {
            stopTest(app);
            return;
        }
    } else {
        g_backHeldSince = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        stopTest(app);
        return;
    }
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_GUIDE)) g_guideSeen = true;

    ImGui::PushFont(app.fonts.bodyBold, 0.0f);
    ImGui::TextUnformatted(displayName(info).c_str());
    ImGui::PopFont();
    {
        // "Hold B (Circle) for 1 second" with this pad's own Back button.
        const float gs = ImGui::GetFrameHeight() * 0.9f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        drawFace(ImGui::GetWindowDrawList(), ImVec2(p.x + gs * 0.5f, p.y + gs * 0.5f), gs, SDL_GetGamepadButtonLabel(gp, back),
                 family == "nintendo");
        ImGui::Dummy(ImVec2(gs, gs));
        ImGui::SameLine();
        const char* backName = family == "playstation" ? "Circle" : "B";
        ImGui::PushTextWrapPos(0);
        ImGui::Text("Testing: the controller does not move around Marathon. Hold %s for 1 second, or press Esc, to stop.", backName);
        ImGui::PopTextWrapPos();
    }
    if (backHeld > 0) ImGui::ProgressBar(backHeld, ImVec2(S(220), S(6)), "");
    if (secondaryButton("Stop test")) {
        stopTest(app);
        return;
    }

    // Sticks with the dead zone, triggers as bars.
    const int dz = deadzoneUnits(c.deadzone);
    const int16_t lx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
    const int16_t rx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX), ry = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY);
    const int16_t lt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), rt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x, r = S(52), h = r * 2 + S(36);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 left(o.x + r + S(4), o.y + r + S(4)), right(o.x + w - r - S(4), o.y + r + S(4));
    drawStick(dl, left, r, lx, ly, dz);
    drawStick(dl, right, r, rx, ry, dz);
    auto bar = [&](float x, int16_t v, PadGlyph g) {
        const float bh = r * 2 - S(24), bw = S(18), top = o.y + S(4);
        const float f = std::clamp(v / 32767.0f, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(x - bw * 0.5f, top), ImVec2(x + bw * 0.5f, top + bh), IM_COL32(22, 27, 39, 255), S(4));
        if (f > 0) dl->AddRectFilled(ImVec2(x - bw * 0.5f, top + bh * (1 - f)), ImVec2(x + bw * 0.5f, top + bh), kYellow, S(4));
        drawPill(dl, ImVec2(x, top + bh + S(14)), S(24), pillText(g, family));
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(std::lround(f * 100)));
        const ImVec2 ts = ImGui::CalcTextSize(pct);
        dl->AddText(ImVec2(x - ts.x * 0.5f, top + bh + S(28)), IM_COL32(230, 234, 244, 255), pct);
    };
    bar(o.x + w * 0.5f - S(40), lt, PadGlyph::LT);
    bar(o.x + w * 0.5f + S(40), rt, PadGlyph::RT);
    ImGui::Dummy(ImVec2(w, h));
    int16_t alx = lx, aly = ly, arx = rx, ary = ry;
    applyStickDeadzone(alx, aly, dz);
    applyStickDeadzone(arx, ary, dz);
    axisLine("LX", lx, alx);
    ImGui::SameLine(w * 0.5f);
    axisLine("RX", rx, arx);
    axisLine("LY", ly, aly);
    ImGui::SameLine(w * 0.5f);
    axisLine("RY", ry, ary);

    // Every button the pad has; lit while pressed.
    const float gs = S(30);
    float x = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        const auto button = static_cast<SDL_GamepadButton>(b);
        if (!SDL_GamepadHasButton(gp, button)) continue;
        const bool down = SDL_GetGamepadButton(gp, button);
        const SDL_GamepadButtonLabel label = SDL_GetGamepadButtonLabel(gp, button);
        const bool face = label != SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
        const std::string name = buttonName(gp, button, family);
        const float bw = face ? gs : pillWidth(name.c_str(), gs);
        if (x > 0 && x + bw > w) x = 0;
        if (x > 0) ImGui::SameLine(0, 0);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (face) drawFace(dl, ImVec2(p.x + bw * 0.5f, p.y + gs * 0.5f), gs, label, family == "nintendo", down);
        else drawPill(dl, ImVec2(p.x + bw * 0.5f, p.y + gs * 0.5f), gs, name.c_str(), down);
        ImGui::Dummy(ImVec2(bw + S(6), gs));
        x += bw + S(6);
    }
    if (!info.hasGuide) ImGui::TextDisabled("This controller has no Guide button: Back + Start closes games.");
    else if (g_guideSeen) ImGui::TextDisabled("Guide: received");
    else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "Guide: not received yet (macOS may be using it; see Closing a game)");
    // The pad cannot scroll during the test, so the page scrolls to it once.
    if (g_testScroll > 0 && --g_testScroll == 0) {
        ImGui::ScrollToRect(ImGui::GetCurrentWindow(), ImRect(start, ImVec2(start.x + w, ImGui::GetCursorScreenPos().y)),
                            ImGuiScrollFlags_KeepVisibleEdgeY);
    }
}

}  // namespace

// ---- public -------------------------------------------------------------------------------------

void drawPadGlyph(ImDrawList* dl, ImVec2 center, float size, PadGlyph glyph, const std::string& family) {
    if (isFace(glyph)) drawFace(dl, center, size, faceLabel(glyph, family), family == "nintendo");
    else drawPill(dl, center, size, pillText(glyph, family));
}

float padGlyphWidth(PadGlyph glyph, float size, const std::string& family) {
    return isFace(glyph) ? size : pillWidth(pillText(glyph, family), size);
}

bool padHintsVisible(App& app) { return !app.touchUi && app.gamepads().lastUsed() != 0; }

std::string padFamilyNow(App& app) {
    const uint32_t id = app.gamepads().lastUsed();
    if (!id) return "generic";
    const char* type = SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(id));
    return padFamily(type ? type : "");
}

std::vector<std::pair<PadGlyph, const char*>> padHintsFor(App& app) { return hintsFor(app); }

void drawPadHints(App& app, const std::vector<std::pair<PadGlyph, const char*>>& hints) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = S(44), gs = S(26), fs = S(17);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(13, 16, 24, 255));
    dl->AddLine(p, ImVec2(p.x + w, p.y), IM_COL32(36, 43, 58, 255), 1.0f);
    const std::string family = padFamilyNow(app);
    ImFont* f = app.fonts.body;
    // Right-aligned, as on a console.
    float total = 0;
    for (auto& [g, text] : hints) {
        total += padGlyphWidth(g, gs, family) + S(6);
        if (text) total += f->CalcTextSizeA(fs, FLT_MAX, 0, text).x + S(22);
    }
    float x = std::max(p.x + S(20), p.x + w - S(20) - total);
    const float cy = p.y + h * 0.5f;
    for (auto& [g, text] : hints) {
        const float gw = padGlyphWidth(g, gs, family);
        drawPadGlyph(dl, ImVec2(x + gw * 0.5f, cy), gs, g, family);
        x += gw + S(6);
        if (!text) continue;
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, text);
        dl->AddText(f, fs, ImVec2(x, cy - ts.y * 0.5f), IM_COL32(200, 206, 220, 255), text);
        x += ts.x + S(22);
    }
    ImGui::Dummy(ImVec2(w, h));
}

// ---- focus --------------------------------------------------------------------------------------

void requestFocus() { g_focusRequest = true; }

bool takeFocusRequest() {
    if (!g_focusRequest) return false;
    g_focusRequest = false;
    return true;
}

bool navCursorVisible() { return GImGui && GImGui->NavCursorVisible; }

void focusOutline(float rounding) {
    if (!ImGui::IsItemFocused() || !navCursorVisible()) return;
    const float t = S(3);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x - t * 0.5f, a.y - t * 0.5f), ImVec2(b.x + t * 0.5f, b.y + t * 0.5f), kYellow,
                                        rounding, 0, t);
}

void unsupportedMarker(const char* text) {
    // Disabled widgets cannot take the pad's focus, so pad users get a "(?)" they can reach.
    if (!navCursorVisible()) return;
    ImGui::SameLine();
    ImGui::PushID(text);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::SmallButton("(?)");
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(text, ImGui::FindRenderedTextEnd(text));  // (without the "##id" part)
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

// ---- pad actions --------------------------------------------------------------------------------

void handlePadActions(App& app, bool popupWasOpen, bool itemWasActive) {
    std::vector<PadAction> actions;
    actions.swap(app.padActions);
    if (app.padTestActive) return;  // (a press that raced the switch to the test)
    static const Screen kTabs[] = {Screen::Library, Screen::AddGames, Screen::Backends, Screen::Settings, Screen::About};
    const int tabCount = static_cast<int>(sizeof(kTabs) / sizeof(kTabs[0]));
    for (PadAction a : actions) {
        if (app.screen == Screen::Playing) {
            // The pause menu: B resumes.
            if (a == PadAction::Back && playerMenuOpen() && !popupWasOpen && !itemWasActive) closePlayerMenu(app);
            continue;
        }
        switch (a) {
            case PadAction::Back:
                if (popupWasOpen || itemWasActive) break;  // ImGui closed the popup or ended the edit
                if (app.screen == Screen::Settings && app.settingsPage == App::SettingsPage::Controllers)
                    app.settingsPage = App::SettingsPage::General;
                else if (app.screen != Screen::Library) app.screen = Screen::Library;
                break;
            case PadAction::TabPrev:
            case PadAction::TabNext: {
                // Holding X (Square) with LB / RB is ImGui's own window switching.
                if (popupWasOpen || itemWasActive || ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft)) break;
                const Screen cur = app.screen == Screen::Game ? Screen::Library : app.screen;
                int i = 0;
                for (int t = 0; t < tabCount; ++t)
                    if (kTabs[t] == cur) i = t;
                i = (i + (a == PadAction::TabNext ? 1 : tabCount - 1)) % tabCount;
                app.screen = kTabs[i];
                break;
            }
            case PadAction::PagePrev:
            case PadAction::PageNext:
                if (popupWasOpen || itemWasActive || app.screen != Screen::Settings) break;
                app.settingsPage = app.settingsPage == App::SettingsPage::General ? App::SettingsPage::Controllers
                                                                                   : App::SettingsPage::General;
                break;
            case PadAction::Play: {
                if (popupWasOpen) break;
                if (app.screen == Screen::Library && !app.focusedGame.empty()) {
                    const GameDef* g = app.catalog.game(app.focusedGame);
                    if (!g) break;
                    if (app.statusOf(*g) == App::GameStatus::Ready) {
                        app.play(g->id);
                    } else {
                        app.selectedGame = g->id;
                        app.screen = Screen::Game;
                    }
                } else if (app.screen == Screen::Game) {
                    const GameDef* g = app.catalog.game(app.selectedGame);
                    if (g && app.statusOf(*g) == App::GameStatus::Ready) app.play(g->id);
                }
                break;
            }
            case PadAction::GameSettings:
                if (popupWasOpen || itemWasActive) break;
                if (app.screen == Screen::Library && !app.focusedGame.empty() && app.catalog.game(app.focusedGame)) {
                    app.selectedGame = app.focusedGame;
                    app.screen = Screen::Game;
                    app.focusSettingsRequest = true;
                } else if (app.screen == Screen::Game) {
                    app.focusSettingsRequest = true;
                }
                break;
            case PadAction::Confirm: break;  // ImGui's own activation
        }
    }
}

// ---- Controllers page -----------------------------------------------------------------------------

void drawControllersPage(App& app) {
    ControllerSettings& c = app.global.controllers;
    const std::vector<ConnectedPad> pads = app.padSnapshot();
    const float w = ImGui::GetContentRegionAvail().x;
    const float colW = w < S(900) ? w : (w - S(20)) * 0.5f;
    const float labelW = S(190);
    bool changed = false;  // (sliders save when they are let go)

    // ---- left column ----
    ImGui::BeginGroup();
    beginPanel("connected", ImVec2(colW, 0));
    sectionHeader(app, "CONNECTED CONTROLLERS");
    if (pads.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("No controller connected. Plug one in or pair it over Bluetooth; it shows up here right away.");
        ImGui::PopTextWrapPos();
    } else if (ImGui::BeginTable("pads", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, S(80));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        bool first = true;
        for (auto& p : pads) {
            ImGui::PushID(static_cast<int>(p.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(app.fonts.bodyBold, 0.0f);
            ImGui::TextUnformatted(displayName(p).c_str());
            ImGui::PopFont();
            std::string sub = padTypeLabel(p.type);
            if (SDL_Gamepad* gp = app.gamepads().gamepad(p.id)) {
                const SDL_JoystickConnectionState conn = SDL_GetGamepadConnectionState(gp);
                if (conn == SDL_JOYSTICK_CONNECTION_WIRED) sub += std::string(kDot) + "USB";
                else if (conn == SDL_JOYSTICK_CONNECTION_WIRELESS) sub += std::string(kDot) + "Wireless";
                int percent = -1;
                const SDL_PowerState power = SDL_GetGamepadPowerInfo(gp, &percent);
                if (percent >= 0 && (power == SDL_POWERSTATE_ON_BATTERY || power == SDL_POWERSTATE_CHARGING || power == SDL_POWERSTATE_CHARGED))
                    sub += std::string(kDot) + "battery " + std::to_string(percent) + "%";
            }
            ImGui::TextDisabled("%s", sub.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (p.player >= 0) ImGui::Text("Player %d", p.player + 1);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (first && takeFocusRequest()) ImGui::SetKeyboardFocusHere();
            first = false;
            if (secondaryButton("Test")) startTest(app, p.id);
            ImGui::TableNextColumn();
            if (secondaryButton("Vibrate", ImVec2(0, 0), p.hasRumble)) app.gamepads().rumble(p.id, 0x9000, 0x9000, 400);
            if (!p.hasRumble && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("SDL cannot drive vibration on this controller.");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    endPanel();

    ImGui::Dummy(ImVec2(0, S(4)));
    beginPanel("players", ImVec2(colW, 0));
    sectionHeader(app, "PLAYERS");
    ImGui::PushItemWidth(-labelW);
    if (takeFocusRequest()) ImGui::SetKeyboardFocusHere();  // (no controller listed above)
    ImGui::PushID("p1");
    changed |= playerCombo(0, c.players[0], pads);
    ImGui::PopID();
    ImGui::PushID("p2");
    changed |= playerCombo(1, c.players[1], pads);
    ImGui::PopID();
    helpMarker("Automatic gives Player 1 the first controller connected and Player 2 the second. A chosen controller is "
               "recognised by its model, so two identical controllers follow the order they were connected in.");
    ImGui::PopItemWidth();
    endPanel();

    ImGui::Dummy(ImVec2(0, S(4)));
    beginPanel("closing", ImVec2(colW, 0));
    sectionHeader(app, "CLOSING A GAME");
    ImGui::PushItemWidth(-labelW);
    changed |= choiceCombo("Close the game with", c.quitChord, kChords);
    helpMarker("Works in every game Marathon starts, including other programs: Marathon asks the program to close, as its "
               "window's close button would, and stops it if it has not closed after 6 seconds. Controllers without a "
               "Guide/Home button use Back + Start. On macOS the Guide/Home button may open Launchpad or the Games app; "
               "choose Back + Start then.");
    ImGui::BeginDisabled(c.quitChord == "off");
    float seconds = c.quitHoldMs / 1000.0f;
    if (ImGui::SliderFloat("Hold for", &seconds, 0.5f, 3.0f, "%.1f s"))
        c.quitHoldMs = std::clamp(static_cast<int>(std::lround(seconds * 10)) * 100, 500, 3000);
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    ImGui::EndDisabled();
    ImGui::PopItemWidth();
    endPanel();
    ImGui::EndGroup();

    // ---- right column ----
    if (colW < w) ImGui::SameLine(0, S(20));
    ImGui::BeginGroup();
    beginPanel("driving", ImVec2(colW, 0));
    sectionHeader(app, "DRIVING");
    ImGui::PushItemWidth(-labelW);
    changed |= choiceCombo("Steering", c.steering, kSteering);
    helpMarker("Analog steers in proportion to the stick; the d-pad still steers where the game allows. Digital steers at "
               "full lock like the original pad; where Marathon can, the stick then works as a d-pad.");
    changed |= choiceCombo("Accelerate and brake", c.pedals, kPedals);
    helpMarker("Triggers are analog where the game reads them; A and X (Cross and Square) keep working. Buttons keeps the "
               "original layout: the triggers do what the game uses them for (shifting, nitrous, views).");
    changed |= ImGui::Checkbox("Vibration", &c.vibration);
    ImGui::SliderInt("Stick dead zone", &c.deadzone, 0, 40, "%d%%");
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    helpMarker("How far the stick moves before the car steers. Raise it if the car drifts with the stick at rest.");
    ImGui::PopItemWidth();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Each game uses what its backend supports; the game page lists what applies.");
    ImGui::PopTextWrapPos();
    endPanel();

    ImGui::Dummy(ImVec2(0, S(4)));
    beginPanel("test", ImVec2(colW, 0));
    sectionHeader(app, "TEST");
    if (app.padTestActive) {
        drawPadTest(app);
    } else if (pads.empty()) {
        ImGui::TextDisabled("Connect a controller to test it.");
    } else {
        uint32_t pick = app.gamepads().connected(g_testPick) ? g_testPick : app.gamepads().lastUsed();
        std::string preview;
        for (auto& p : pads)
            if (p.id == pick) preview = displayName(p);
        ImGui::SetNextItemWidth(-labelW);
        if (ImGui::BeginCombo("Controller", preview.c_str())) {
            for (auto& p : pads) {
                ImGui::PushID(static_cast<int>(p.id));
                if (ImGui::Selectable(displayName(p).c_str(), p.id == pick)) g_testPick = pick = p.id;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (secondaryButton("Start test")) startTest(app, pick);
        ImGui::SameLine();
        ImGui::TextDisabled("Shows the sticks, triggers and buttons as Marathon reads them.");
    }
    endPanel();

    ImGui::Dummy(ImVec2(0, S(4)));
    beginPanel("keys", ImVec2(colW, 0));
    sectionHeader(app, "KEYBOARD AND HOTKEYS (GAMES INSIDE MARATHON)");
    ImGui::BulletText("Esc / Guide: pause menu");
    if (c.quitChord != "off")
        ImGui::BulletText("Hold %s: quit to Marathon", c.quitChord == "back-start" ? "Back + Start" : "Guide + Start");
    ImGui::BulletText("Tab (hold) / right-stick click (hold): fast-forward");
    ImGui::BulletText("F11: fullscreen   F5 / F8: save / load state");
    ImGui::BulletText("Keyboard: arrows, Z/X/A/S, Q/W, Enter (Start)");
    endPanel();
    ImGui::EndGroup();

    if (changed) app.saveAll();
}

// ---- game page --------------------------------------------------------------------------------

void drawGameControls(App& app, const GameDef& g, Backend* backend, const GameBackendOption* option) {
    if (!backend) {
        ImGui::TextDisabled("No backend can run this game here.");
        return;
    }
    LaunchRequest req;
    req.game = &g;
    req.option = option;
    req.backend = &backend->def();
    req.entry = app.library.find(g.id);
    req.library = &app.library;
    req.settings = app.settingsFor(g.id);
    req.display = app.display;
    req.controllers = app.controllersFor(g.id);
    req.pads = app.padSnapshot();
    const ControlsInfo ci = backend->controls(req);
    const std::string& name = backend->def().name;
    ImGui::PushTextWrapPos(0);
    if (ci.layout.empty()) {
        ImGui::TextDisabled("Uses %s's own controller settings.", name.c_str());
    } else {
        for (auto& line : ci.layout) {
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", line.c_str());
        }
    }
    if (!ci.menu.empty()) ImGui::TextWrapped("Menu: %s", ci.menu.c_str());
    if (!ci.note.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "%s", ci.note.c_str());
    if (std::string chord = chordText(req.controllers); !chord.empty()) ImGui::TextWrapped("Close the game: %s", chord.c_str());
    if (ci.players > 1) ImGui::TextDisabled("Up to %d players with their own controllers.", ci.players);
    std::string unused;
    auto add = [&](bool used, const char* label) {
        if (!used) unused += (unused.empty() ? "" : ", ") + std::string(label);
    };
    add(ci.steering, "Steering");
    add(ci.pedals, "Accelerate and brake");
    add(ci.vibration, "Vibration");
    add(ci.deadzone, "Stick dead zone");
    add(ci.playerChoice, "Player 1 / 2 choice");
    if (!unused.empty()) ImGui::TextDisabled("Not used here: %s", unused.c_str());
    ImGui::PopTextWrapPos();
}

}  // namespace rrm::ui
