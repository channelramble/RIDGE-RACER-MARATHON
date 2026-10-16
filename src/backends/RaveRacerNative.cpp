// Rave Racer from spacestate1/namco22-decompile ("Namco 2x Systems", MIT): the
// System 22 program translated to C with an OpenGL renderer.
//
// Releases: Windows windows-release-<ver>.zip (RaveRacer.exe among the other
// games), Linux RaveRacer-x86_64.AppImage, and Marathon's self-contained macOS
// Apple Silicon build (bare rr with statically linked SDL2).
//
// Data folder. The game program reads everything from its working directory:
// roms/ (MAME 0.271+ raverace.zip and namcoc74.zip), extracted/ (the chips it
// unpacks from them on the first start), rr_controls.cfg and rr_eeprom.nv.
//   Windows   RaveRacer.exe changes to its own folder (portable by design), so
//             a Marathon install keeps them in its install folder.
//   AppImage  its launcher script picks ${XDG_DATA_HOME:-~/.local/share}/
//             namco-2x-systems/raverace (unless a portable folder sits beside
//             the AppImage); Marathon sets XDG_DATA_HOME to its own folder.
//   bare rr   (macOS bundle or local build) starts in that same folder.
// Marathon rebuilds raverace.zip / namcoc74.zip there from the user's ROMs and
// edits only the rr_controls.cfg keys it owns (the game itself rewrites single
// lines the same way). A linked install whose folder is the user's own (Windows,
// or a portable AppImage) gets environment overrides only and nothing written.
//
// Picture. `resolution` is the render height (the width follows the picture's
// shape); with `widescreen = 1` the picture takes the window's own shape, so
// ultrawide is real (more track at the sides) in fullscreen on an ultrawide
// screen. Windowed, widescreen opens a 16:9 window. The game always runs at
// 59.906 Hz: `frame_rate` above 60 shows frames again, it does not interpolate.
//
// Controllers: every SDL controller drives the car (left stick the analog steering, RT / LT the
// analog pedals, A / B full gas / brake, LB / RB shift, Y view, Back inserts a coin). The pad_*
// button keys belong to the player and the game's Controls page; Marathon sets only pad_deadzone
// (stick units) in the folders it owns.
#include <cmath>
#include <map>

#include "backends/Adapters.h"
#include "backends/ControllerConfigs.h"
#include "backends/EmuCommon.h"
#include "core/ConfigFiles.h"

namespace rrm {

namespace {

using namespace emu;

constexpr const char* kSlug = "raverace";          // the launcher's folder name for Rave Racer
constexpr const char* kCfg = "rr_controls.cfg";

// Where the game keeps its files, and whether Marathon may write there.
struct Layout {
    fs::path data;            // roms/, extracted/, rr_controls.cfg, rr_eeprom.nv
    bool owned = true;        // Marathon writes the romsets and its cfg keys
    std::map<std::string, std::string> env;
    std::vector<std::string> args;
    std::string workingDir;
};

bool isShellScript(const std::string& exe) {
    std::ifstream in(utf8ToPath(exe), std::ios::binary);
    char head[2] = {};
    return in.read(head, 2) && head[0] == '#' && head[1] == '!';
}

Layout layoutFor(const std::string& exe, bool userProvided, const fs::path& marathonRoot) {
    Layout l;
    const fs::path program = utf8ToPath(exe);
    const fs::path marathonData = marathonRoot / "namco-2x-systems" / kSlug;
    if (currentOs() == Os::Windows) {
        l.data = program.parent_path();  // RaveRacer.exe works from its own folder
        l.owned = !userProvided;
        return l;
    }
    const bool appImage = endsWithI(exe, ".AppImage");
    if (appImage || isShellScript(exe)) {
        // namco-2x-launch (inside the AppImage, or the .deb's /usr/bin/raveracer) chooses the folder.
        if (pathToUtf8(program.filename()) == "namco-2x-launch") l.args = {"rave"};
        if (appImage) {
            // Runs without FUSE (Ubuntu 22.04+ lacks libfuse2); $APPIMAGE is still set.
            l.env["APPIMAGE_EXTRACT_AND_RUN"] = "1";
            const fs::path dir = program.parent_path();
            std::error_code ec;
            for (const char* p : {"namco-2x-systems-data", "namco22-data"})
                if (fs::is_directory(dir / p, ec)) {
                    l.data = dir / p / kSlug;
                    l.owned = !userProvided;
                    return l;
                }
            fs::path home = program;
            home += ".home";
            fs::path stemHome = program.parent_path() / (pathToUtf8(program.stem()) + ".home");
            for (const fs::path& h : {home, stemHome})
                if (fs::is_directory(h, ec)) {
                    l.data = h / "namco-2x-systems" / kSlug;
                    l.owned = !userProvided;
                    return l;
                }
        }
        l.env["XDG_DATA_HOME"] = pathToUtf8(marathonRoot);
        l.data = marathonData;
        return l;
    }
    // A bare program: everything relative to the working directory.
    l.data = marathonData;
    l.workingDir = pathToUtf8(marathonData);
    return l;
}

// A cfg value without its trailing "# comment" (the game's own parser drops those too).
std::string cfgValue(const cfg::Ini& ini, const std::string& key) {
    std::string v = ini.get("", key);
    if (auto h = v.find('#'); h != std::string::npos) v.erase(h);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;
}

// Render lines for the unified resolution; 0 = "native" (the window's own pixels).
int renderLines(const std::string& res) {
    if (res == "display") return 0;
    if (res.size() >= 2 && res.back() == 'x') return 480 * std::clamp(std::atoi(res.c_str()), 1, 9);
    if (res == "720p") return 720;
    if (res == "1080p") return 1080;
    if (res == "1440p") return 1440;
    if (res == "4k") return 2160;
    if (res == "5k") return 2880;
    if (res == "8k") return 4320;
    return 480;  // native: the board's 640x480 (wider with widescreen)
}

// `frame_rate`: pictures a second. 60 = one per game frame (original), 0 = Auto (the
// display's rate, each frame shown again as needed), else the nearest offered rate.
int frameRateFor(const GameSettings& s) {
    const int fps = resolveFps(s);
    if (fps < 0) return 0;
    if (fps == 0) return 60;
    static const int rates[] = {24, 30, 50, 60, 75, 90, 120, 144, 165, 240};
    int best = 60;
    for (int r : rates)
        if (std::abs(r - fps) < std::abs(best - fps) || (std::abs(r - fps) == std::abs(best - fps) && r > best)) best = r;
    return best;
}

class RaveRacerNative : public Backend {
public:
    using Backend::Backend;

    ControlsInfo controls(const LaunchRequest& req) const override {
        ControlsInfo info;
        const BackendInstall* bi = req.library ? req.library->install(def_.id) : nullptr;
        if (bi && !bi->executable.empty() && !layoutFor(bi->executable, bi->userProvided, dataDir(def_.id)).owned)
            info.note = "This linked Rave Racer keeps its own controller settings (the game's Controls page).";
        else
            info.deadzone = true;
        info.layout = {"Left stick: steer (analog); d-pad full lock", "RT / LT: gas / brake (analog); A / B: full",
                       "LB / RB: shift", "Y: view", "Back: insert coin"};
        info.menu = "Start, R3 or Esc: the game's menu (Esc twice exits)";
        return info;
    }

    std::vector<std::string> missingRequirements(const LaunchRequest& req) const override {
        std::vector<std::string> out = Backend::missingRequirements(req);
        const BackendInstall* bi = req.library ? req.library->install(def_.id) : nullptr;
        if (!bi || bi->executable.empty()) return out;
        const Layout l = layoutFor(bi->executable, bi->userProvided, dataDir(def_.id));
        if (!l.owned) {
            std::error_code ec;
            if (!fs::exists(l.data / "extracted" / "rv1data.6r", ec) && !fs::exists(l.data / "roms" / "raverace.zip", ec))
                out.push_back(std::string(kWarningPrefix) + "This linked Rave Racer keeps its ROMs in " +
                              pathToUtf8(l.data / "roms") +
                              ": Marathon does not write into it. Put raverace.zip and namcoc74.zip there.");
            out.push_back(std::string(kWarningPrefix) + "Marathon passes only fullscreen, frame rate and vsync to this "
                          "linked install; resolution and widescreen stay as set in the game's own menu (Esc, or Start on a controller).");
        }
        return out;
    }

    LaunchResult launch(const LaunchRequest& req) override {
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        if (exe.empty()) return fail(def_.name + " is not installed.");
        if (!req.entry || !req.entry->hasFiles()) return fail("Add your Rave Racer ROMs (the MAME raverace set) first.");
        const BackendInstall* bi = req.library->install(def_.id);
        const GameSettings& s = req.settings;
        const Layout l = layoutFor(exe, bi && bi->userProvided, dataDir(def_.id));
        std::vector<std::string> notes;

        ProcessSpec spec;
        spec.argv.push_back(exe);
        for (auto& a : l.args) spec.argv.push_back(a);
        spec.env = l.env;
        spec.workingDir = l.workingDir;

        const int fps = frameRateFor(s);
        if (!s.vsync) spec.env["RR_VSYNC"] = "0";  // the 60 fps pacing: a timer instead of the display's refresh
        if (fps == 0 || fps > 60)
            notes.push_back("the game runs at 59.906 Hz; above 60 pictures a second frames are shown again "
                            "(not interpolated): a 60 or 120 Hz display mode is smoothest");

        if (!l.owned) {
            // The user's own folder: environment only (RR_FULLSCREEN overrides the cfg's window mode,
            // ENG_FPS the frame rate, neither is saved).
            spec.env["RR_FULLSCREEN"] = s.fullscreen ? "1" : "0";
            spec.env["ENG_FPS"] = std::to_string(fps);
            notes.push_back("linked install with its own folder " + pathToUtf8(l.data) +
                            ": resolution and widescreen are left to the game's menu");
            notes.push_back(ctl::noteLine(req.controllers, "left to Rave Racer"));
            notes.push_back(ctl::linkedNote("Rave Racer"));
        } else {
            // ROMs: MAME 0.271+ names, rebuilt from the user's files by CRC (the game matches by name).
            std::error_code ec;
            fs::create_directories(l.data / "roms", ec);
            std::string error;
            if (prepareMameSet(req, l.data / "roms", error).empty()) return fail(error);
            GameBackendOption option = req.option ? *req.option : GameBackendOption{};
            if (!option.params.contains("devices"))
                option.params["devices"] = json::array(
                    {{{"set", "namcoc74"}, {"file", "c74.bin"}, {"crc32", jget<std::string>(def_.params, "c74Crc", "a3dce360")}}});
            LaunchRequest withDevices = req;
            withDevices.option = &option;
            if (!prepareMameDevices(withDevices, l.data / "roms", error)) return fail(error);

            const fs::path cfgPath = l.data / kCfg;
            cfg::Ini ini;
            if (!ini.load(pathToUtf8(cfgPath))) return fail("Could not read " + pathToUtf8(cfgPath));
            std::vector<std::string> changes;
            auto put = [&](const std::string& key, const std::string& value) {
                if (cfgValue(ini, key) == value) return;
                ini.set("", key, value);
                changes.push_back(key + " = " + value);
            };

            // Window: desktop fullscreen (an exclusive mode chosen in the game's menu stays).
            const std::string mode = cfgValue(ini, "window_mode");
            put("window_mode", s.fullscreen ? (mode == "2" ? "2" : "1") : "0");
            put("fullscreen", s.fullscreen ? "1" : "0");

            // Aspect: widescreen makes the picture the window's shape (more track at the sides).
            bool wide = cfgValue(ini, "widescreen") == "1";
            const std::string& a = s.aspect;
            if (a == "4:3") {
                wide = false;
                put("aspect", "4:3");
            } else if (a == "16:9" || a == "21:9" || a == "31:9" || a == "32:9" || a == "fit") {
                wide = true;
            }
            if (a != "auto") put("widescreen", wide ? "1" : "0");
            const double screen = req.display.width > 0 && req.display.height > 0
                                      ? static_cast<double>(req.display.width) / req.display.height
                                      : 16.0 / 9.0;
            const double shape = !wide ? 4.0 / 3.0 : s.fullscreen ? std::max(screen, 4.0 / 3.0) : 16.0 / 9.0;
            if (wide && a != "auto") {
                const double want = aspectRatio(a, req.display.width, req.display.height);
                if (!s.fullscreen && want > 16.0 / 9.0 + 0.01)
                    notes.push_back("windowed widescreen opens a 16:9 window; the picture follows the window's shape, "
                                    "so " + a + " needs fullscreen on a screen that shape (or a wider window)");
                else if (s.fullscreen && a != "fit" && std::fabs(want - screen) / want > 0.03)
                    notes.push_back("widescreen follows the screen's " + fmtNumber(std::round(screen * 100) / 100) +
                                    ":1 shape, not " + a);
            }

            // Resolution: the render height; the width only names the shape (and the exclusive mode).
            const int lines = renderLines(s.resolution);
            const int width = (static_cast<int>(std::lround(lines * shape)) + 1) & ~1;
            put("resolution", lines == 0 ? "native" : std::to_string(width) + "x" + std::to_string(lines));
            // Nearest-neighbour presentation, except when supersampling (a render taller than the screen).
            const bool sharp = s.filter == "nearest" && (lines == 0 || lines <= req.display.height);
            put("scaling", sharp ? "sharp" : "smooth");
            put("frame_rate", std::to_string(fps));

            // Controllers: only the stick dead zone (the pad_* buttons are the player's).
            std::string pad;
            for (auto& [k, v] : ctl::raveCfgKeys(req.controllers)) {
                put(k, v);
                pad += (pad.empty() ? "" : ", ") + k + " = " + v;
            }
            notes.push_back(ctl::noteLine(req.controllers, pathToUtf8(cfgPath) + " " + pad));

            // Advanced: settings.extra.cfg {"key": "value"} (draw_distance, wide_hud, free_play ...).
            if (auto it = s.extra.find("cfg"); it != s.extra.end() && it->is_object())
                for (auto& [k, v] : it->items())
                    if (v.is_string() && !k.empty() &&
                        k.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789") == std::string::npos)
                        put(k, v.get<std::string>());

            if (!changes.empty() && !ini.save(pathToUtf8(cfgPath))) return fail("Could not write " + pathToUtf8(cfgPath));
            notes.push_back("data folder " + pathToUtf8(l.data));
            for (auto& c : changes) notes.push_back("  " + c);
        }

        {
            std::ofstream log(paths().logs / utf8ToPath(def_.id + ".log"), std::ios::binary | std::ios::app);
            log << "\n[marathon] " << timestamp() << (dryRun() ? " (dry run)" : "") << "\n";
            for (auto& n : notes) log << "[marathon] " << n << "\n";
            if (currentOs() == Os::Windows) log << "[marathon] the game's own log: " << pathToUtf8(l.data / "raveracer.log") << "\n";
        }
        return spawnLogged(def_.id, spec);
    }
};

}  // namespace

std::unique_ptr<Backend> makeRaveRacerNative(const BackendDef& def) { return std::make_unique<RaveRacerNative>(def); }

}  // namespace rrm
