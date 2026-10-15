#include "core/Settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>

namespace rrm {

json GameSettings::toJson() const {
    return json{{"backend", backend},     {"resolution", resolution}, {"aspect", aspect},
                {"filter", filter},       {"antiAlias", antiAlias},   {"precision", precision},
                {"redraw", redraw},       {"fps", fps},               {"customFps", customFps},
                {"fpsMethod", fpsMethod}, {"vsync", vsync},           {"speed", speed},
                {"turbo", turbo},         {"fastBoot", fastBoot},     {"fullscreen", fullscreen},
                {"extra", extra}};
}

GameSettings GameSettings::fromJson(const json& j) {
    GameSettings s;
    if (!j.is_object()) return s;
    s.backend = jget(j, "backend", s.backend);
    s.resolution = jget(j, "resolution", s.resolution);
    s.aspect = jget(j, "aspect", s.aspect);
    s.filter = jget(j, "filter", s.filter);
    s.antiAlias = jget(j, "antiAlias", s.antiAlias);
    s.precision = jget(j, "precision", s.precision);
    s.redraw = jget(j, "redraw", s.redraw);
    s.fps = jget(j, "fps", s.fps);
    s.customFps = std::clamp(jget(j, "customFps", s.customFps), 30, 1000);
    s.fpsMethod = jget(j, "fpsMethod", s.fpsMethod);
    s.vsync = jget(j, "vsync", s.vsync);
    s.speed = std::clamp(jget(j, "speed", s.speed), 25, 400);
    s.turbo = std::clamp(jget(j, "turbo", s.turbo), 0, 1000);
    s.fastBoot = jget(j, "fastBoot", s.fastBoot);
    s.fullscreen = jget(j, "fullscreen", s.fullscreen);
    if (auto it = j.find("extra"); it != j.end() && it->is_object()) s.extra = *it;
    return s;
}

// ---- controllers ---------------------------------------------------------------------------

namespace {

bool oneOf(const std::string& v, std::initializer_list<const char*> allowed) {
    for (const char* a : allowed)
        if (v == a) return true;
    return false;
}

// "" (automatic), "none" or an SDL GUID (lower-cased); anything else becomes automatic.
std::string validController(std::string v) {
    if (v.empty() || v == "none") return v;
    if (v.size() != 32) return "";
    for (char& c : v) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return "";
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v;
}

// Applies the keys present in j onto c. Invalid values become the default when `reset`
// (reading settings.json) or keep c's current value otherwise (a per-game override).
void readDriving(ControllerSettings& c, const json& j, bool reset) {
    const ControllerSettings def;
    if (auto v = jget<std::string>(j, "steering", ""); oneOf(v, {"stick", "dpad"})) c.steering = v;
    else if (reset) c.steering = def.steering;
    if (auto v = jget<std::string>(j, "pedals", ""); oneOf(v, {"triggers", "buttons"})) c.pedals = v;
    else if (reset) c.pedals = def.pedals;
    c.vibration = jget(j, "vibration", c.vibration);
    c.deadzone = std::clamp(jget(j, "deadzone", c.deadzone), 0, 40);
}

}  // namespace

json ControllerSettings::toJson() const {
    json p = json::array();
    for (auto& pc : players) p.push_back({{"controller", pc.controller}, {"name", pc.name}});
    return json{{"steering", steering},     {"pedals", pedals},       {"vibration", vibration},
                {"deadzone", deadzone},     {"players", p},           {"quitChord", quitChord},
                {"quitHoldMs", quitHoldMs}};
}

ControllerSettings ControllerSettings::fromJson(const json& j) {
    ControllerSettings c;
    if (!j.is_object()) return c;
    readDriving(c, j, true);
    if (auto it = j.find("players"); it != j.end() && it->is_array()) {
        for (size_t i = 0; i < it->size() && i < c.players.size(); ++i) {
            const json& p = (*it)[i];
            if (!p.is_object()) continue;
            c.players[i].controller = validController(jget<std::string>(p, "controller", ""));
            c.players[i].name = jget<std::string>(p, "name", "");
        }
    }
    if (auto v = jget<std::string>(j, "quitChord", ""); oneOf(v, {"guide-start", "back-start", "off"})) c.quitChord = v;
    c.quitHoldMs = std::clamp(jget(j, "quitHoldMs", c.quitHoldMs), 500, 3000);
    return c;
}

ControllerSettings ControllerSettings::withOverrides(const json& extra) const {
    ControllerSettings c = *this;
    if (!extra.is_object()) return c;
    auto it = extra.find("controllers");
    if (it != extra.end() && it->is_object()) readDriving(c, *it, false);
    return c;
}

// ---- global ---------------------------------------------------------------------------------

json GlobalSettings::toJson() const {
    return json{{"version", kVersion},
                {"defaults", defaults.toJson()},
                {"uiScale", uiScale},
                {"showUnsupported", showUnsupported},
                {"confirmDownloads", confirmDownloads},
                {"lastScanFolder", lastScanFolder},
                {"controllers", controllers.toJson()}};
}

GlobalSettings GlobalSettings::fromJson(const json& j) {
    GlobalSettings g;
    if (!j.is_object()) return g;
    // Version 1 files have no "version" and no "controllers": the controller defaults apply.
    // Older builds read version 2 files too, skipping the keys they do not know.
    g.version = jget(j, "version", 1);
    if (auto it = j.find("defaults"); it != j.end()) g.defaults = GameSettings::fromJson(*it);
    g.uiScale = jget(j, "uiScale", g.uiScale);
    g.showUnsupported = jget(j, "showUnsupported", g.showUnsupported);
    g.confirmDownloads = jget(j, "confirmDownloads", g.confirmDownloads);
    g.lastScanFolder = jget(j, "lastScanFolder", g.lastScanFolder);
    if (auto it = j.find("controllers"); it != j.end()) g.controllers = ControllerSettings::fromJson(*it);
    return g;
}

int resolveScale(const std::string& resolution, int nativeHeight, int displayHeight, int maxScale) {
    if (nativeHeight <= 0) nativeHeight = 240;
    if (maxScale <= 0) maxScale = 16;
    auto lines = [&](int target) {
        // Round up so "1080p" never renders below 1080 lines.
        return std::max(1, static_cast<int>(std::ceil(static_cast<double>(target) / nativeHeight)));
    };
    int scale = 1;
    if (resolution == "native") scale = 1;
    else if (resolution.size() >= 2 && resolution.back() == 'x') scale = std::atoi(resolution.c_str());
    else if (resolution == "720p") scale = lines(720);
    else if (resolution == "1080p") scale = lines(1080);
    else if (resolution == "1440p") scale = lines(1440);
    else if (resolution == "4k") scale = lines(2160);
    else if (resolution == "5k") scale = lines(2880);
    else if (resolution == "8k") scale = lines(4320);
    else if (resolution == "display") scale = lines(displayHeight > 0 ? displayHeight : 1080);
    return std::clamp(scale, 1, maxScale);
}

int resolveFps(const GameSettings& s) {
    if (s.fps == "original" || s.fps.empty()) return 0;
    if (s.fps == "display") return -1;
    if (s.fps == "custom") return std::clamp(s.customFps, 30, 1000);
    int v = std::atoi(s.fps.c_str());
    return v > 0 ? v : 0;
}

double aspectRatio(const std::string& aspect, int displayWidth, int displayHeight) {
    if (aspect == "fit") return displayWidth > 0 && displayHeight > 0 ? static_cast<double>(displayWidth) / displayHeight : 16.0 / 9.0;
    size_t colon = aspect.find(':');
    if (colon == std::string::npos) return 0;
    double w = std::atof(aspect.substr(0, colon).c_str()), h = std::atof(aspect.substr(colon + 1).c_str());
    return w > 0 && h > 0 ? w / h : 0;
}

}  // namespace rrm
