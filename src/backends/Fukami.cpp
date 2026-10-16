// Fukami (danisandoval/fukami, GPL-3.0): native port of Ridge Racer V (USA,
// SLUS-20002) by static recompilation, drawn with PCSX2's GS renderer.
//
// Data folder. Fukami unpacks the game once from the user's disc image (about
// 480 MB) into its own per-user folder and keeps fukami.ini and the save card
// there:
//   macOS    ~/Library/Application Support/Fukami  (NSApplicationSupportDirectory; CFFIXED_USER_HOME moves it)
//   Windows  %LOCALAPPDATA%\Fukami                 (FUKAMI_DATA_DIR replaces it)
//   Linux    ${XDG_DATA_HOME:-~/.local/share}/Fukami
// Marathon shares that folder with a standalone Fukami, so the game is unpacked
// once and saves are common to both. It works the folder out from the same
// environment Fukami reads (which also lets tests point both at /tmp).
//
// Settings. In app mode Fukami reads only fukami.ini: there are no settings
// flags, and the game process gets a fresh environment built from the ini. So
// Marathon edits the keys it owns in that file with cfg::Ini (comments, order
// and every other key stay as they are) and only writes keys the installed
// version knows: an unknown key or a bad value makes Fukami replace the whole
// file with its defaults. The bundled fukami-default.ini documents every key.
//
// Disc. Windows and Linux take the image (.chd, .cue or .bin) as the first
// argument on the first start; Marathon wraps a .iso in a one-track cue sheet.
// macOS has no argument or variable for it: Fukami asks with its own dialog.
//
// Motion. The game runs at 59.94 frames a second and Fukami renders no more.
// High-refresh displays are served by showing every frame for the same time:
// on Windows `[display] refresh_rate` switches the display to a multiple of
// 60 Hz in fullscreen and `vrr` uses FreeSync/G-Sync; macOS (Metal present
// pacing) and Linux (Vulkan FIFO) keep the desktop's rate.
//
// Controllers. Fukami presents the first two controllers as DualShock 2 pads with a fixed layout
// (no remapping yet): the left stick steers, A (Cross) accelerates, X (Square) brakes, LB / RB
// shift, and LT / RT are the game's L2 / R2 (rear view), so the triggers cannot be the pedals.
// Marathon sets [input] analog = true with stick steering (false would make the game ignore the
// stick) and [input] rumble from the vibration setting.
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "backends/Adapters.h"
#include "backends/ControllerConfigs.h"
#include "backends/EmuCommon.h"
#include "core/ConfigFiles.h"

namespace rrm {

namespace {

using namespace emu;

constexpr int kNativeLines = 448;  // full-mode frame height
constexpr int kMaxScale = 8;

// An environment variable as a path ("" when unset). Wide on Windows so non-ASCII folders survive.
fs::path envPath(const char* name) {
#if defined(_WIN32)
    std::wstring wide(name, name + std::strlen(name));
    const wchar_t* v = _wgetenv(wide.c_str());
    return v && *v ? fs::path(v) : fs::path();
#else
    const char* v = std::getenv(name);
    return v && *v ? fs::path(v) : fs::path();
#endif
}

// Fukami's data folder, decided the way its launcher does (src/app/fukami_app*.{mm,cpp}).
fs::path fukamiDataDir() {
#if defined(_WIN32)
    if (fs::path p = envPath("FUKAMI_DATA_DIR"); p.is_absolute()) return p;
    fs::path base = envPath("LOCALAPPDATA");
    if (base.empty()) base = envPath("USERPROFILE") / "AppData" / "Local";
    return base / "Fukami";
#elif defined(__APPLE__)
    fs::path home = envPath("CFFIXED_USER_HOME");
    if (home.empty()) home = envPath("HOME");
    return home / "Library" / "Application Support" / "Fukami";
#else
    if (fs::path xdg = envPath("XDG_DATA_HOME"); xdg.is_absolute()) return xdg / "Fukami";
    return envPath("HOME") / ".local" / "share" / "Fukami";
#endif
}

// True when disc/ holds the files its marker lists (Fukami's own check: nine files, sizes match).
bool discReady(const fs::path& dataDir) {
    const fs::path disc = dataDir / "disc";
    std::istringstream marker(readText(disc / ".fukami-disc-verified"));
    std::string name;
    uintmax_t size = 0;
    int files = 0;
    while (marker >> name >> size) {
        std::error_code ec;
        if (fs::file_size(disc / utf8ToPath(name), ec) != size || ec) return false;
        ++files;
    }
    return files == 9;
}

// The app's bundled defaults: Fukami.app/Contents/Resources (macOS) or share/ beside the
// launcher (Windows Fukami.exe, the Linux Fukami script; bin/Fukami when that was linked).
fs::path bundledDefaults(const std::string& exe) {
    const fs::path dir = utf8ToPath(exe).parent_path();
    std::error_code ec;
    for (const fs::path& p : {dir.parent_path() / "Resources" / "fukami-default.ini", dir / "share" / "fukami-default.ini",
                              dir.parent_path() / "share" / "fukami-default.ini"})
        if (fs::is_regular_file(p, ec)) return p;
    return {};
}

// Keys the installed version understands: its default ini has a "# --- key ---" header
// or a "key =" line for each. Without that file, the keys v1.0.0 already had.
std::set<std::string> knownKeys(const std::string& defaultsText) {
    std::set<std::string> keys;
    std::istringstream in(defaultsText);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos) continue;
        std::string word;
        if (line.compare(i, 5, "# ---") == 0) {
            std::istringstream w(line.substr(i + 5));
            w >> word;
        } else if (line[i] != '#' && line[i] != ';' && line[i] != '[') {
            const size_t eq = line.find('=', i);
            if (eq != std::string::npos && eq > i) word = line.substr(i, line.find_last_not_of(" \t", eq - 1) + 1 - i);
        }
        if (!word.empty() && word.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789") == std::string::npos)
            keys.insert(word);
    }
    if (keys.count("ratio") && keys.count("scale")) return keys;
    return {"ratio", "hud", "aspect", "window", "fullscreen", "integer_scaling", "present_pacing",
            "render_mode", "scale", "aa1", "fxaa", "cas", "aniso", "mipmap", "analog", "rumble"};
}

// [display] ratio for the unified aspect ("" = leave Fukami's choice). Fukami has 4:3,
// 16:10, 16:9 and 21:9 (the camera really widens); anything wider is shown at 21:9.
std::string ratioFor(const LaunchRequest& req, std::vector<std::string>& notes) {
    const std::string& a = req.settings.aspect;
    if (a == "4:3" || a == "16:9" || a == "21:9") return a;
    if (a != "31:9" && a != "32:9" && a != "fit") return {};
    const double want = aspectRatio(a, req.display.width, req.display.height);
    if (want > 21.0 / 9.0 + 0.07) {
        notes.push_back("Fukami's widest view is 21:9; the " + fmtNumber(std::round(want * 100) / 100) +
                        ":1 screen gets bars at the sides");
        return "21:9";
    }
    struct Ratio {
        const char* name;
        double value;
    };
    const Ratio ratios[] = {{"4:3", 4.0 / 3.0}, {"16:10", 1.6}, {"16:9", 16.0 / 9.0}, {"21:9", 21.0 / 9.0}};
    const Ratio* best = &ratios[0];
    for (const Ratio& r : ratios)
        if (std::fabs(std::log(r.value / want)) < std::fabs(std::log(best->value / want))) best = &r;
    return best->name;
}

// Windows `[display] refresh_rate`: a multiple of 60 Hz for the 59.94 fps game.
// original/display: auto (a display already at 60/120/180/240 stays, another goes to the
// highest multiple below its rate); a number: the highest multiple of 60 not above it.
std::string refreshRateFor(const GameSettings& s) {
    const int fps = resolveFps(s);
    if (fps <= 0) return "auto";
    return std::to_string(std::clamp(fps / 60, 1, 4) * 60);
}

#if defined(__APPLE__)
int macosMajorVersion() {
    char buf[32] = {};
    size_t len = sizeof(buf) - 1;
    if (sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0) != 0) return 0;
    return std::atoi(buf);
}
#endif

class Fukami : public Backend {
public:
    using Backend::Backend;

    ControlsInfo controls(const LaunchRequest&) const override {
        ControlsInfo info;
        info.vibration = true;
        info.players = 2;
        info.layout = {"Left stick: steer (analog)", "A (Cross): accelerate, X (Square): brake", "LB / RB: shift down / up",
                       "LT / RT are the PS2's L2 / R2 (rear view) in this game"};
        info.note = "Fukami has no button remapping yet: the triggers cannot accelerate or brake.";
        info.menu = "Guide, Esc or F1: Fukami's menu > Quit";
        return info;
    }

    // The macOS app is built for macOS 27 and later (LSMinimumSystemVersion 27.0).
    bool availableHere() const override {
#if defined(__APPLE__)
        const int minimum = jget(def_.params, "minMacos", 27);
        if (minimum > 0 && macosMajorVersion() < minimum) return false;
#endif
        return Backend::availableHere();
    }

    std::vector<std::string> missingRequirements(const LaunchRequest& req) const override {
        std::vector<std::string> out = Backend::missingRequirements(req);
        if (!req.entry || !req.entry->hasFiles()) return out;
        const fs::path data = fukamiDataDir();
        if (currentOs() == Os::MacOS && !discReady(data))
            out.push_back(std::string(kWarningPrefix) + "On its first start Fukami asks for your disc image: choose " +
                          gamePath(req) + ". It unpacks the game (about 480 MB) once into " + pathToUtf8(data) + ".");
#if defined(_WIN32)
        const fs::path system = envPath("SystemRoot") / "System32";
        std::error_code ec;
        if (!fs::exists(system / "vcruntime140_1.dll", ec) || !fs::exists(system / "msvcp140.dll", ec))
            out.push_back(std::string(kWarningPrefix) +
                          "Fukami needs the Microsoft Visual C++ x64 runtime (vc_redist.x64.exe from microsoft.com).");
#endif
        return out;
    }

    LaunchResult launch(const LaunchRequest& req) override {
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        if (exe.empty()) return fail(def_.name + " is not installed.");
        if (!req.entry || !req.entry->hasFiles())
            return fail("Add your Ridge Racer V (USA) disc image (.cue with its .bin, .chd or .iso) first.");
        const GameSettings& s = req.settings;
        std::vector<std::string> notes;

        const fs::path data = fukamiDataDir();
        const fs::path iniPath = data / "fukami.ini";
        const fs::path defaults = bundledDefaults(exe);
        const std::string defaultsText = defaults.empty() ? std::string() : readText(defaults);
        const std::set<std::string> known = knownKeys(defaultsText);

        // The user's file; before Fukami's first start, the copy of its defaults it would make.
        cfg::Ini ini;
        std::error_code ec;
        const bool existed = fs::exists(iniPath, ec);
        if (existed) {
            if (!ini.load(pathToUtf8(iniPath))) return fail("Could not read " + pathToUtf8(iniPath));
        } else {
            ini.parse(defaultsText);
        }
        std::vector<std::string> changes;
        auto put = [&](const std::string& section, const std::string& key, const std::string& value) {
            if (!known.count(key) || ini.get(section, key, "\x01") == value) return;
            ini.set(section, key, value);
            changes.push_back(section + "." + key + " = " + value);
        };

        // Resolution: whole multiples of the 448-line frame (Fukami lowers odd ones on a Steam Deck).
        put("rendering", "scale", std::to_string(renderScale(req, kNativeLines, kMaxScale)));

        // Aspect: real widescreen; the picture fit follows it ([display] aspect empty).
        if (const std::string ratio = ratioFor(req, notes); !ratio.empty()) {
            put("display", "ratio", ratio);
            put("display", "aspect", "");
        }

        put("display", "fullscreen", boolStr(s.fullscreen));
        put("rendering", "fxaa", boolStr(s.antiAlias == "fxaa"));
        // Linear: every 3D texture smoothed, the cars included (the game asks for point sampling there).
        if (s.filter != "nearest" || !ini.get("rendering", "texture_filter").empty())
            put("rendering", "texture_filter", s.filter == "nearest" ? "ps2" : "bilinear");

        // Motion and display sync.
        if (currentOs() == Os::Windows) {
            put("display", "refresh_rate", refreshRateFor(s));
            // vrr = on presents each frame at its own time without waiting for the display:
            // even on FreeSync/G-Sync, tearing elsewhere, which is what "vsync off" asks for.
            put("display", "vrr", s.vsync ? "auto" : "on");
        } else {
            // Fukami presents on the display's refresh whenever it is paced; the [env] allowlist
            // lets this one variable turn that off. The line is only added when it is needed.
            const std::string vsync = ini.get("env", "RRV_PCSX2_GS_VSYNC");
            if ((!s.vsync || !vsync.empty()) && vsync != (s.vsync ? "1" : "0")) {
                ini.set("env", "RRV_PCSX2_GS_VSYNC", s.vsync ? "1" : "0");
                changes.push_back(std::string("env.RRV_PCSX2_GS_VSYNC = ") + (s.vsync ? "1" : "0"));
            }
        }
        if (resolveFps(s) > 60 || resolveFps(s) < 0)
            notes.push_back(std::string("Ridge Racer V runs at 59.94 frames a second in Fukami; ") +
                            (currentOs() == Os::Windows
                                 ? "in fullscreen the display is switched to a multiple of 60 Hz (or uses VRR)"
                                 : "every frame is shown for the same time at the desktop's refresh rate"));

        // Controllers ([input]); the advanced keys below still win.
        std::string input;
        for (auto& k : ctl::fukamiInputKeys(req.controllers)) {
            if (!known.count(k.key)) continue;  // this version has no such key
            put(k.section, k.key, k.value);
            input += (input.empty() ? "" : ", ") + k.section + "." + k.key + " = " + k.value;
        }
        notes.push_back(ctl::noteLine(req.controllers, (input.empty() ? std::string("left to Fukami") : "fukami.ini " + input) +
                                                           " (Fukami's own fixed layout)"));

        // Advanced: settings.extra.ini {"section.key": "value"} for keys Marathon does not map
        // (hud, draw_distance, car_lod, logging ...). Only keys this version knows are written.
        if (auto it = s.extra.find("ini"); it != s.extra.end() && it->is_object())
            for (auto& [dotted, v] : it->items()) {
                const size_t dot = dotted.find('.');
                if (dot == std::string::npos || !v.is_string()) continue;
                const std::string section = dotted.substr(0, dot), key = dotted.substr(dot + 1);
                if (section == "env" || section == "developer" || section == "paths") continue;
                put(section, key, v.get<std::string>());
            }

        if (!changes.empty() || !existed) {
            // Dry run: the file Fukami reads is outside Marathon's folders; write a preview instead.
            const fs::path target = dryRun() ? dataDir(def_.id) / "fukami.ini.preview" : iniPath;
            if (!ini.save(pathToUtf8(target))) return fail("Could not write " + pathToUtf8(target));
            notes.push_back(dryRun() ? "would update " + pathToUtf8(iniPath) + " (preview: " + pathToUtf8(target) + ")"
                                     : "updated " + pathToUtf8(iniPath));
        }
        for (auto& c : changes) notes.push_back("  " + c);

        ProcessSpec spec;
        spec.argv.push_back(exe);
        // First start: the disc image to unpack (Windows/Linux; Fukami ignores it once unpacked).
        if (!discReady(data)) {
            if (currentOs() == Os::MacOS) {
                notes.push_back("first start: choose " + gamePath(req) + " in Fukami's dialog");
            } else {
                std::string image = gamePath(req);
                if (endsWithI(image, ".iso")) {
                    // Fukami's launcher takes .chd/.cue/.bin; a cue sheet makes the ISO one cooked data track.
                    const fs::path cue = dataDir(def_.id) / "disc-image.cue";
                    writeText(cue, "FILE \"" + image + "\" BINARY\r\n  TRACK 01 MODE1/2048\r\n    INDEX 01 00:00:00\r\n");
                    image = pathToUtf8(cue);
                }
                spec.argv.push_back(image);
            }
        }

        {
            std::ofstream log(paths().logs / utf8ToPath(def_.id + ".log"), std::ios::binary | std::ios::app);
            log << "\n[marathon] " << timestamp() << (dryRun() ? " (dry run)" : "") << "\n";
            for (auto& n : notes) log << "[marathon] " << n << "\n";
        }
        return spawnLogged(def_.id, spec);
    }
};

}  // namespace

std::unique_ptr<Backend> makeFukami(const BackendDef& def) { return std::make_unique<Fukami>(def); }

}  // namespace rrm
