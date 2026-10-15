#include "core/Settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

json GlobalSettings::toJson() const {
    return json{{"defaults", defaults.toJson()},
                {"uiScale", uiScale},
                {"showUnsupported", showUnsupported},
                {"confirmDownloads", confirmDownloads},
                {"lastScanFolder", lastScanFolder}};
}

GlobalSettings GlobalSettings::fromJson(const json& j) {
    GlobalSettings g;
    if (!j.is_object()) return g;
    if (auto it = j.find("defaults"); it != j.end()) g.defaults = GameSettings::fromJson(*it);
    g.uiScale = jget(j, "uiScale", g.uiScale);
    g.showUnsupported = jget(j, "showUnsupported", g.showUnsupported);
    g.confirmDownloads = jget(j, "confirmDownloads", g.confirmDownloads);
    g.lastScanFolder = jget(j, "lastScanFolder", g.lastScanFolder);
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
