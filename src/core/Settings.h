// Unified enhancement settings. Every backend translates these into its own
// configuration (INI/TOML/YAML/CLI flags/core options). See docs/SETTINGS.md.
#pragma once

#include <string>

#include "core/Json.h"

namespace rrm {

struct GameSettings {
    std::string backend = "auto";      // backend id, or "auto" = best for this OS

    // Rendering
    std::string resolution = "1080p";  // native|2x|3x|4x|5x|6x|8x|720p|1080p|1440p|4k|5k|8k|display
    std::string aspect = "auto";       // auto (game default)|4:3|16:9|21:9|32:9|fit (the window/display shape)
    std::string filter = "nearest";    // nearest|linear|xbr
    std::string antiAlias = "off";     // off|fxaa|msaa|ssaa
    bool precision = true;             // PGXP / sub-pixel geometry where available
    bool redraw = false;               // experimental native re-render (RR1 scene, Rage modern, MAME hi-res)

    // Motion
    std::string fps = "original";      // original|display|60|90|120|144|165|240|300|custom
    int customFps = 144;
    std::string fpsMethod = "interpolate";  // interpolate|blend
    bool vsync = true;

    // Speed
    int speed = 100;                   // base run speed in percent (100 = authentic)
    int turbo = 300;                   // fast-forward speed while the hotkey is held/toggled; 0 = unlimited
    bool fastBoot = true;              // skip BIOS intro / fast disc loads where safe

    // Window
    bool fullscreen = false;

    json extra = json::object();       // backend-specific advanced options

    json toJson() const;
    static GameSettings fromJson(const json& j);
};

struct GlobalSettings {
    GameSettings defaults;             // applied to games without their own settings
    float uiScale = 0.0f;              // 0 = automatic from display DPI
    bool showUnsupported = true;       // show games that cannot run on this device
    bool confirmDownloads = true;
    std::string lastScanFolder;
    json toJson() const;
    static GlobalSettings fromJson(const json& j);
};

// Resolves "1080p", "4k", "display" ... into an integer scale of the native
// height (e.g. 240 lines for PS1, 272 for PSP, 480 for PS2/arcade, 720 for HD).
int resolveScale(const std::string& resolution, int nativeHeight, int displayHeight, int maxScale);
// Target presentation FPS: 0 = original/game-driven, -1 = follow display.
int resolveFps(const GameSettings& s);
// Width/height of an aspect setting ("21:9" -> 2.333); "fit" = the display's shape;
// 0 for "auto" (the game's own default) or anything unrecognised.
double aspectRatio(const std::string& aspect, int displayWidth, int displayHeight);

}  // namespace rrm
