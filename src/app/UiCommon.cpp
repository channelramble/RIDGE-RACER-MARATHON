#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "app/Ui.h"
#include "core/Platform.h"
#include "imgui_internal.h"

namespace rrm {

// ---- theme ---------------------------------------------------------------------------------

namespace {
ImFont* loadFont(const char* resource, float size) {
    std::string data;
    if (!readResource(resource, data)) {
        SDL_Log("font missing: %s", resource);
        return nullptr;
    }
    void* mem = IM_ALLOC(data.size());
    std::memcpy(mem, data.data(), data.size());
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;
    cfg.OversampleH = 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(mem, (int)data.size(), size, &cfg);
}
}  // namespace

void App::loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    fonts.body = loadFont("assets/fonts/TitilliumWeb-Regular.ttf", 18.0f);
    fonts.bodyBold = loadFont("assets/fonts/TitilliumWeb-SemiBold.ttf", 18.0f);
    fonts.display = loadFont("assets/fonts/Rajdhani-Bold.ttf", 32.0f);
    if (!fonts.body) fonts.body = io.Fonts->AddFontDefault();
    if (!fonts.bodyBold) fonts.bodyBold = fonts.body;
    if (!fonts.display) fonts.display = fonts.bodyBold;
    io.FontDefault = fonts.body;
}

void App::applyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0;
    st.ChildRounding = 14;
    st.FrameRounding = 8;
    st.PopupRounding = 12;
    st.GrabRounding = 8;
    st.TabRounding = 8;
    st.ScrollbarRounding = 8;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.FrameBorderSize = 0;
    st.WindowPadding = ImVec2(18, 16);
    st.FramePadding = ImVec2(12, 7);
    st.ItemSpacing = ImVec2(12, 10);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.ScrollbarSize = 14;
    st.GrabMinSize = 14;
    st.FontSizeBase = 18.0f;
    ImVec4* c = st.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
    c[ImGuiCol_Text] = rgb(232, 236, 244);
    c[ImGuiCol_TextDisabled] = rgb(110, 118, 136);
    c[ImGuiCol_WindowBg] = rgb(9, 11, 16);
    c[ImGuiCol_ChildBg] = rgb(17, 21, 31);
    c[ImGuiCol_PopupBg] = rgb(19, 24, 35, 0.98f);
    c[ImGuiCol_Border] = rgb(36, 43, 58);
    c[ImGuiCol_FrameBg] = rgb(26, 32, 46);
    c[ImGuiCol_FrameBgHovered] = rgb(34, 42, 60);
    c[ImGuiCol_FrameBgActive] = rgb(40, 50, 72);
    c[ImGuiCol_Button] = rgb(30, 37, 53);
    c[ImGuiCol_ButtonHovered] = rgb(42, 52, 74);
    c[ImGuiCol_ButtonActive] = rgb(52, 64, 92);
    c[ImGuiCol_Header] = rgb(245, 196, 0, 0.18f);
    c[ImGuiCol_HeaderHovered] = rgb(245, 196, 0, 0.28f);
    c[ImGuiCol_HeaderActive] = rgb(245, 196, 0, 0.38f);
    c[ImGuiCol_CheckMark] = rgb(245, 196, 0);
    c[ImGuiCol_SliderGrab] = rgb(245, 196, 0);
    c[ImGuiCol_SliderGrabActive] = rgb(255, 214, 64);
    c[ImGuiCol_Separator] = rgb(36, 43, 58);
    c[ImGuiCol_PlotHistogram] = rgb(245, 196, 0);
    c[ImGuiCol_NavCursor] = rgb(245, 196, 0);
    c[ImGuiCol_ScrollbarBg] = rgb(9, 11, 16, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = rgb(44, 52, 70);
    c[ImGuiCol_TableHeaderBg] = rgb(22, 27, 39);
    c[ImGuiCol_TableBorderStrong] = rgb(36, 43, 58);
    c[ImGuiCol_TableBorderLight] = rgb(28, 34, 47);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.02f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.6f);
    st.ScaleAllSizes(uiScale);
    st.FontScaleMain = uiScale;
}

namespace ui {

// ---- helpers ----------------------------------------------------------------------------------

float S(float v) { return v * app().uiScale; }

ImVec4 hexColorV(const std::string& hex, float alpha) {
    unsigned r = 245, g = 196, b = 0;
    if (hex.size() == 7 && hex[0] == '#') sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
    return ImVec4(r / 255.f, g / 255.f, b / 255.f, alpha);
}

ImU32 hexColor(const std::string& hex, float alpha) { return ImGui::ColorConvertFloat4ToU32(hexColorV(hex, alpha)); }

void titleText(App& app, const char* text, float size, ImU32 color) {
    ImGui::PushFont(app.fonts.display, size);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

bool primaryButton(const char* label, const ImVec2& size, ImU32 color, bool enabled) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    ImGui::PushStyleColor(ImGuiCol_Button, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(1.f, c.x * 1.12f + 0.05f), std::min(1.f, c.y * 1.12f + 0.05f),
                                                         std::min(1.f, c.z * 1.12f + 0.05f), 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.85f, c.y * 0.85f, c.z * 0.85f, 1));
    float lum = 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
    ImGui::PushStyleColor(ImGuiCol_Text, lum > 0.55f ? ImVec4(0.04f, 0.05f, 0.08f, 1) : ImVec4(1, 1, 1, 1));
    ImGui::PushFont(app().fonts.display, 0.0f);
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::Button(label, size);
    ImGui::EndDisabled();
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return pressed && enabled;
}

bool secondaryButton(const char* label, const ImVec2& size, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::Button(label, size);
    ImGui::EndDisabled();
    return pressed && enabled;
}

void chip(const char* text, ImU32 bg, ImU32 fg) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(S(9), S(3));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 size(ts.x + pad.x * 2, ts.y + pad.y * 2);
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, size.y * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + pad.x, p.y + pad.y), fg, text);
    ImGui::Dummy(size);
}

void sectionHeader(App& app, const char* text) {
    ImGui::PushFont(app.fonts.display, ImGui::GetStyle().FontSizeBase * 1.2f);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(245, 196, 0, 255));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

std::string ellipsizePath(const std::string& path, float maxWidth) {
    if (ImGui::CalcTextSize(path.c_str()).x <= maxWidth) return path;
    std::string s = path;
    while (s.size() > 8 && ImGui::CalcTextSize(("..." + s).c_str()).x > maxWidth) s.erase(0, 1);
    return "..." + s;
}

void drawCardArt(ImDrawList* dl, ImVec2 a, ImVec2 b, const GameDef& g, float rounding, bool hovered) {
    ImU32 accent = hexColor(g.accent, 1.0f);
    ImU32 dark = IM_COL32(12, 15, 22, 255);
    dl->AddRectFilledMultiColor(a, b, hexColor(g.accent, hovered ? 0.55f : 0.42f), dark, dark,
                                hexColor(g.accent, hovered ? 0.30f : 0.22f));
    dl->PushClipRect(a, b, true);
    // Racing stripes.
    float w = b.x - a.x, h = b.y - a.y;
    for (int i = 0; i < 3; ++i) {
        float x = a.x + w * (0.58f + i * 0.09f);
        float sw = w * 0.035f;
        dl->AddQuadFilled(ImVec2(x, b.y), ImVec2(x + sw, b.y), ImVec2(x + sw + h * 0.6f, a.y), ImVec2(x + h * 0.6f, a.y),
                          hexColor(g.accent, 0.16f - i * 0.04f));
    }
    // Big faded year.
    if (g.year > 0) {
        char year[8];
        snprintf(year, sizeof(year), "%d", g.year);
        ImFont* f = app().fonts.display;
        float fs = h * 0.62f;
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, year);
        dl->AddText(f, fs, ImVec2(b.x - ts.x - w * 0.03f, b.y - ts.y * 0.92f), IM_COL32(255, 255, 255, 14), year);
    }
    // Checkered corner.
    float cs = std::max(4.0f, h * 0.045f);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 6; ++x)
            if ((x + y) % 2 == 0)
                dl->AddRectFilled(ImVec2(b.x - (6 - x) * cs - cs, a.y + y * cs + cs), ImVec2(b.x - (5 - x) * cs - cs, a.y + (y + 1) * cs + cs),
                                  IM_COL32(255, 255, 255, 22));
    dl->PopClipRect();
    dl->AddRect(a, b, hovered ? accent : IM_COL32(255, 255, 255, 18), rounding, 0, hovered ? 2.0f : 1.0f);
}

void progressLine(TaskProgress& p, float width) {
    std::string stage;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        stage = p.stage;
    }
    uint64_t done = p.done, total = p.total;
    float frac = total > 0 ? (float)((double)done / (double)total) : -1.0f;
    char overlay[96] = "";
    if (total > 0) snprintf(overlay, sizeof(overlay), "%s / %s", formatBytes(done).c_str(), formatBytes(total).c_str());
    else if (done > 0) snprintf(overlay, sizeof(overlay), "%s", formatBytes(done).c_str());
    if (frac < 0) frac = (float)std::fmod(ImGui::GetTime() * 0.5, 1.0);
    ImGui::ProgressBar(frac, ImVec2(width, S(6)), "");
    ImGui::TextDisabled("%s %s", stage.c_str(), overlay);
}

std::string formatBytes(uint64_t bytes) {
    char buf[32];
    if (bytes >= (1ull << 30)) snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
    else if (bytes >= (1ull << 20)) snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    else snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    return buf;
}

std::string formatDuration(int64_t s) {
    char buf[32];
    if (s >= 3600) snprintf(buf, sizeof(buf), "%lldh %02lldm", (long long)(s / 3600), (long long)(s / 60 % 60));
    else snprintf(buf, sizeof(buf), "%lldm", (long long)(s / 60));
    return buf;
}

void beginPanel(const char* id, const ImVec2& size) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}

void endPanel() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ---- settings editor ----------------------------------------------------------------------------

namespace {

struct Choice {
    const char* value;
    const char* label;
};

const Choice kResolutions[] = {{"native", "Native (original)"}, {"2x", "2x"},         {"3x", "3x"},
                               {"4x", "4x"},                    {"5x", "5x"},         {"6x", "6x"},
                               {"8x", "8x"},                    {"10x", "10x"},       {"720p", "720p"},
                               {"1080p", "1080p"},              {"1440p", "1440p"},   {"4k", "4K (2160p)"},
                               {"5k", "5K (2880p)"},            {"8k", "8K (4320p)"}, {"display", "Match my display"}};
const Choice kFps[] = {{"original", "Original"}, {"display", "Match display refresh"}, {"60", "60 FPS"},
                       {"90", "90 FPS"},         {"100", "100 FPS"},                  {"120", "120 FPS"},
                       {"144", "144 FPS"},       {"165", "165 FPS"},                  {"200", "200 FPS"},
                       {"240", "240 FPS"},       {"300", "300 FPS"},                  {"360", "360 FPS"},
                       {"custom", "Custom..."}};
const Choice kMethods[] = {{"interpolate", "Interpolated (motion-compensated)"}, {"blend", "Frame blend (cheaper)"}};
const Choice kAspects[] = {{"auto", "Game default"},   {"4:3", "4:3"},
                           {"16:9", "16:9 widescreen"}, {"21:9", "21:9 ultrawide"},
                           {"32:9", "32:9 super ultrawide"}, {"fit", "Match my screen"}};
const Choice kFilters[] = {{"nearest", "Sharp (nearest)"}, {"linear", "Smooth (bilinear)"}, {"xbr", "xBR upscale"}};
const Choice kAA[] = {{"off", "Off"}, {"fxaa", "FXAA"}, {"msaa", "MSAA"}, {"ssaa", "Supersampling"}};
const int kTurbo[] = {150, 200, 300, 400, 0};

// Values allowed by the backend for a key; empty optional = everything allowed (global defaults).
bool allowedList(const json* caps, const char* key, std::vector<std::string>& out) {
    out.clear();
    if (!caps) return true;
    auto it = caps->find(key);
    if (it == caps->end() || !it->is_array()) return false;
    for (auto& v : *it)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return !out.empty();
}

bool capFlag(const json* caps, const char* key) {
    if (!caps) return true;
    auto it = caps->find(key);
    if (it == caps->end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) return !it->get<std::string>().empty() && it->get<std::string>() != "false";
    return false;
}

std::string capString(const json* caps, const char* key) {
    if (!caps) return "base-and-turbo";
    auto it = caps->find(key);
    if (it == caps->end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_boolean() && it->get<bool>()) return "base-and-turbo";
    return "";
}

template <size_t N>
bool combo(const char* label, std::string& value, const Choice (&choices)[N], const json* caps, const char* key,
           const std::string& backendName) {
    std::vector<std::string> allowed;
    bool any = allowedList(caps, key, allowed);
    // "auto" (the game's own default) is always acceptable for aspect ratio.
    if (any && std::string(key) == "aspect" && std::find(allowed.begin(), allowed.end(), "auto") == allowed.end())
        allowed.insert(allowed.begin(), "auto");
    auto ok = [&](const char* v) { return !caps || std::find(allowed.begin(), allowed.end(), v) != allowed.end(); };
    const char* current = value.c_str();
    for (auto& c : choices)
        if (value == c.value) current = c.label;
    bool changed = false;
    ImGui::BeginDisabled(!any);
    if (ImGui::BeginCombo(label, any ? current : "Not supported")) {
        for (auto& c : choices) {
            if (!ok(c.value)) continue;
            bool sel = value == c.value;
            if (ImGui::Selectable(c.label, sel)) {
                value = c.value;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (!any && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s does not support this setting.", backendName.c_str());
    if (any && caps && !ok(value.c_str()))
        ImGui::TextDisabled("  %s can't use this value; it picks the closest option it supports.", backendName.c_str());
    return changed;
}

bool checkbox(const char* label, bool& v, bool supported, const std::string& backendName, const char* help = nullptr) {
    ImGui::BeginDisabled(!supported);
    bool changed = ImGui::Checkbox(label, &v);
    ImGui::EndDisabled();
    if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s does not support this setting.", backendName.c_str());
    if (help) helpMarker(help);
    return changed;
}

}  // namespace

bool settingsEditor(App& app, GameSettings& s, const json* caps, const std::string& backendName) {
    bool changed = false;
    float labelW = S(170);
    ImGui::PushItemWidth(-labelW);

    ImGui::SeparatorText("Picture");
    changed |= combo("Resolution", s.resolution, kResolutions, caps, "resolution", backendName);
    helpMarker("Internal rendering resolution. Higher values give crisper 3D at a GPU cost.");
    changed |= combo("Aspect ratio", s.aspect, kAspects, caps, "aspect", backendName);
    changed |= combo("Texture filter", s.filter, kFilters, caps, "filter", backendName);
    changed |= combo("Anti-aliasing", s.antiAlias, kAA, caps, "antiAlias", backendName);
    changed |= checkbox("Geometry precision (PGXP)", s.precision, capFlag(caps, "precision"), backendName,
                        "Stops PS1 polygons wobbling and textures warping. Visual only.");
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 196, 64, 255));
    changed |= checkbox("Experimental redraw", s.redraw, capFlag(caps, "redraw"), backendName,
                        "Re-renders the 3D scene natively instead of upscaling the original image "
                        "(Ridge Racer native scene, Rage Racer modern renderer, Rave Racer native renderer). "
                        "Crisper and smoother, but experimental.");
    ImGui::PopStyleColor();

    ImGui::SeparatorText("Motion");
    changed |= combo("Frame rate", s.fps, kFps, caps, "fps", backendName);
    helpMarker("Above the original rate, extra frames are drawn between game frames; the game itself keeps its original "
               "speed and physics. Generated frames add up to one frame of input latency.");
    if (s.fps == "custom") {
        changed |= ImGui::SliderInt("Custom FPS", &s.customFps, 30, 1000);
    }
    {
        std::vector<std::string> allowed;
        if (allowedList(caps, "fpsMethod", allowed) && s.fps != "original")
            changed |= combo("Smoothing method", s.fpsMethod, kMethods, caps, "fpsMethod", backendName);
    }
    changed |= checkbox("V-Sync", s.vsync, capFlag(caps, "vsync"), backendName);

    ImGui::SeparatorText("Speed");
    std::string speedCap = capString(caps, "speed");
    bool baseSpeed = speedCap == "base-and-turbo" || speedCap == "frame-limit";
    bool turbo = !speedCap.empty() && speedCap != "false";
    ImGui::BeginDisabled(!baseSpeed);
    changed |= ImGui::SliderInt("Game speed", &s.speed, 50, 300, "%d%%");
    ImGui::EndDisabled();
    if (!baseSpeed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s runs at the original speed only.", backendName.c_str());
    helpMarker("100% is the authentic speed. Raise it to speed the whole game up.");
    ImGui::BeginDisabled(!turbo);
    char turboLabel[32];
    snprintf(turboLabel, sizeof(turboLabel), s.turbo == 0 ? "Unlimited" : "%d%%", s.turbo);
    if (ImGui::BeginCombo("Fast-forward", turboLabel)) {
        for (int t : kTurbo) {
            char l[32];
            snprintf(l, sizeof(l), t == 0 ? "Unlimited" : "%d%%", t);
            if (ImGui::Selectable(l, s.turbo == t)) {
                s.turbo = t;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    helpMarker(app.touchUi ? "Hold the >> button while playing to fast-forward."
                           : "Hold Tab (or the right stick click) while playing to fast-forward.");
    changed |= checkbox("Fast boot", s.fastBoot, true, backendName, "Skips BIOS intros and speeds up loading where it is safe.");

    ImGui::SeparatorText("Window");
    changed |= checkbox("Start in fullscreen", s.fullscreen, capFlag(caps, "fullscreen") || !caps, backendName);
    ImGui::PopItemWidth();
    return changed;
}

// ---- dialogs ---------------------------------------------------------------------------------------

namespace {
std::mutex g_dialogMutex;
std::vector<DialogResult> g_dialogs;
}  // namespace

void deliverDialog(App&, DialogResult&& r) {
    std::lock_guard<std::mutex> lock(g_dialogMutex);
    g_dialogs.push_back(std::move(r));
}

std::vector<DialogResult> takeDialogs(App&) {
    std::lock_guard<std::mutex> lock(g_dialogMutex);
    std::vector<DialogResult> out;
    out.swap(g_dialogs);
    return out;
}

void onDialogResult(App& app, const DialogResult& r) {
    if (r.paths.empty()) return;
    const std::string& path = r.paths.front();
    if (r.purpose == "scan-folder") {
        app.scanFolder(path);
    } else if (r.purpose.rfind("system:", 0) == 0) {
        std::string key = r.purpose.substr(7);
        std::string kind = identifySystemFile(path);
        if (!kind.empty() && kind != key) app.toast("That file looks like a " + kind + ", registered as such.");
        app.library.setSystemFile(kind.empty() ? key : kind, path);
        app.library.save();
        app.toast("System file registered");
    } else if (r.purpose.rfind("link:", 0) == 0) {
        std::string id = r.purpose.substr(5);
        if (Backend* b = app.backend(id)) {
            std::string error;
            if (b->linkExisting(app.library, path, error)) app.toast(b->def().name + " linked");
            else app.toast(error, true);
        }
    } else if (r.purpose.rfind("file:", 0) == 0) {
        std::string gameId = r.purpose.substr(5);
        ScanHit hit;
        if (identifyPath(app.catalog, path, hit) && hit.gameId == gameId) {
            app.acceptHit(hit);
            app.toast("Files added");
        } else if (identifyPath(app.catalog, path, hit) && !hit.gameId.empty()) {
            app.acceptHit(hit);
            const GameDef* g = app.catalog.game(hit.gameId);
            app.toast("That file is " + (g ? g->title : hit.gameId) + " - added it there.");
        } else {
            app.toast("That doesn't look like a supported dump of this game.", true);
        }
    }
}

// ---- frame -------------------------------------------------------------------------------------------

void draw(App& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (app.screen == Screen::Playing) {
        drawPlayer(app);
        drawOverlays(app);
        return;
    }
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##marathon", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    drawTopBar(app);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(20)));
    ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    switch (app.screen) {
        case Screen::Library: drawLibrary(app); break;
        case Screen::Game: drawGame(app); break;
        case Screen::AddGames: drawAddGames(app); break;
        case Screen::Backends: drawBackends(app); break;
        case Screen::Settings: drawSettings(app); break;
        case Screen::About: drawAbout(app); break;
        case Screen::Playing: break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
    drawOverlays(app);
}

}  // namespace ui
}  // namespace rrm
