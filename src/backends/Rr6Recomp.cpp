// Ridge Racer 6 Recompiled (Sirhalo23/RidgeRacer6-Recompilation, BSD-3-Clause): the
// Xbox 360 game's PowerPC code translated to C++ and run on the ReXGlue SDK (the
// author's fork). Only the USA disc works (default.xex SHA-256 39D3C000...6C00).
//
// Layouts Marathon runs (worked out from the recorded executable):
//   Windows zip    <pkg>/RR6 Launcher.exe, <pkg>/bin/rr6_recomp.exe (+ rexruntime.dll,
//                  rexgpu-xenos.dll, rr6_recomp.toml), <pkg>/tools/prepare-game.ps1
//   Linux tarball  <pkg>/ridge-racer-6.sh, <pkg>/bin/rr6_recomp (+ librexruntime.so,
//                  librexgpu-xenos.so, rr6-extract); rr6_recomp.toml only after a first start
//   macOS, built by the user (there is no release asset):
//     packaged     "Ridge Racer 6.app"/Contents/MacOS/rr6-launcher (macos/make-package.sh).
//                  "rr6-launcher [iso] --play -- <cvars>" copies the game out of the ISO into
//                  ~/Library/Application Support/Ridge Racer 6/game when needed, then execs
//                  Contents/MacOS/rr6_recomp with RR6_MACOS_RELEASE_USER_ROOT set, so the
//                  settings file is ~/Library/Application Support/Ridge Racer 6/rr6_recomp.toml.
//     development  <repo>/out/build/mac-arm64-release/rr6_recomp.app/Contents/MacOS/rr6_recomp
//                  (build-macos.sh): rr6_recomp.toml beside it, the game in <repo>/../game;
//                  started the way run-macos.sh does.
//
// Game files: the program runs from the files on the disc (about 6 GB), copied once out of
// the user's ISO by the release's own tool, tools/prepare-game.ps1 (Windows) or
// bin/rr6-extract (Linux), into Marathon's data folder so the copy survives updates. A copy
// the user already made with the official launcher (<pkg>/game) or an extracted game
// folder is used as it is. Saves stay where the game keeps them (Documents\rr6_recomp).
//
// Settings: the SDK ranks --name=value on the command line above rr6_recomp.toml
// (rex::cvar::Source::kCommandLine > kConfig), so Marathon passes every cvar it owns as a
// flag and never edits the file: key bindings and language stay the user's. The game runs
// at 60 fps: `vsync` is the guest's 60 Hz vertical-blank timer its speed hangs on and always
// stays on; Marathon's vsync choice goes to the presenter instead.
//
// Controllers: the game reads SDL controllers as Xbox 360 pads (left stick steers, RT / LT analog
// gas and brake, rumble as the game's own option), with no remapping and one player, so Marathon's
// controller settings do not apply. Marathon passes --hid_mappings_file with the package's own
// gamecontrollerdb.txt (Windows: the package root, Linux: bin/, the macOS development build: the
// repository root; the macOS app passes it itself), so the database no longer depends on the folder
// the game starts in. Key bindings (keybind_*, mnk_*) stay the player's. settings.extra.logInput =
// true adds --rr6_log_input=true (player 1's pad state in rr6-recomp-game.log).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "backends/Adapters.h"
#include "backends/ControllerConfigs.h"
#include "backends/EmuCommon.h"

namespace rrm {

namespace {

constexpr const char* kUsaXexSha256 = "39d3c0004ec62aeb0fe3e7e1889cf25d98fbd27987b6bc6b5ff30a56ffba6c00";
constexpr double kOriginalAspect = 16.0 / 9.0;
constexpr int kNativeHeight = 720;  // the game draws 1280x720
constexpr int kMaxScaleY = 4;       // upstream's launcher offers 1x-4x the 720 lines
constexpr int kMaxScaleAxis = 7;    // the SDK's TextureCache::kMaxDrawResolutionScaleAlongAxis
constexpr const char* kReadyMarker = ".rr6-ready";      // written by the official copy tools
constexpr const char* kCopyingMarker = ".rr6-copying";  // present while a copy is unfinished

enum class Layout { WindowsZip, LinuxTarball, MacApp, MacDevBuild };

struct Install {
    Layout layout = Layout::WindowsZip;
    fs::path root;     // package folder, or the .app bundle
    fs::path program;  // what Marathon starts: bin/rr6_recomp(.exe), rr6-launcher or rr6_recomp
};

bool nativeLayout(Layout layout) {
    if (layout == Layout::WindowsZip) return currentOs() == Os::Windows;
    if (layout == Layout::LinuxTarball) return currentOs() == Os::Linux;
    return currentOs() == Os::MacOS;
}

std::string platformProblem() {
    if (currentOs() == Os::MacOS)
        return "Link the native macOS Ridge Racer 6.app build. The Windows .exe and Linux packages cannot run on macOS.";
    return "Link the Ridge Racer 6 Recomp package for this operating system (" +
           std::string(osId(currentOs())) + ").";
}

bool pathExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool locate(const std::string& exe, Install& out) {
    const fs::path p = utf8ToPath(exe);
    const std::string name = emu::lower(pathToUtf8(p.filename()));
    const fs::path dir = p.parent_path();
    const bool inBin = emu::lower(pathToUtf8(dir.filename())) == "bin";
    if (name == "rr6-launcher" || (name == "rr6_recomp" && pathToUtf8(dir.filename()) == "MacOS")) {
        out.layout = name == "rr6-launcher" ? Layout::MacApp : Layout::MacDevBuild;
        out.root = dir.parent_path().parent_path();  // .../X.app
        out.program = p;
        return true;
    }
    if (name == "rr6 launcher.exe" || name == "rr6_recomp.exe") {
        out.layout = Layout::WindowsZip;
        out.root = name == "rr6_recomp.exe" && inBin ? dir.parent_path() : dir;
        out.program = out.root / "bin" / "rr6_recomp.exe";
        if (!pathExists(out.program) && name == "rr6_recomp.exe") out.program = p;
        return true;
    }
    if (name == "ridge-racer-6.sh" || name == "rr6_recomp") {
        out.layout = Layout::LinuxTarball;
        out.root = name == "rr6_recomp" && inBin ? dir.parent_path() : dir;
        out.program = out.root / "bin" / "rr6_recomp";
        return true;
    }
    return false;
}

// Complete game files: default.xex, no unfinished copy and, for copies made by the
// official tools, their "ready" marker.
bool gameReady(const fs::path& dir, bool needMarker) {
    return !dir.empty() && pathExists(dir / "default.xex") && !pathExists(dir / kCopyingMarker) &&
           (!needMarker || pathExists(dir / kReadyMarker));
}

// An extracted game folder the user added (scanner format "xex-folder"), or empty.
fs::path extractedFolder(const LaunchRequest& req) {
    if (!req.entry || !req.entry->hasFiles()) return {};
    const fs::path p = utf8ToPath(req.entry->files.path);
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && emu::endsWithI(req.entry->files.path, ".xex")) return p.parent_path();
    if (fs::is_directory(p, ec)) return p;
    return {};
}

std::string joinArgs(const std::vector<std::string>& argv) {
    std::string out;
    for (auto& a : argv) {
        if (!out.empty()) out += ' ';
        out += !a.empty() && a.find_first_of(" \t\"'") == std::string::npos ? a : "\"" + a + "\"";
    }
    return out;
}

// emu::spawnLogged plus Marathon's notes in the log. `visible`: the program's output is
// not redirected (on Windows its console window shows it).
LaunchResult spawn(const std::string& backendId, ProcessSpec spec, const std::vector<std::string>& notes,
                   bool visible = false) {
    LaunchResult r;
    r.mode = LaunchResult::Mode::ExternalProcess;
    r.commandLine = spec.argv;
    const fs::path log = paths().logs / utf8ToPath(backendId + ".log");
    {
        std::ofstream out(log, std::ios::binary | std::ios::app);
        if (out) {
            out << "\n==== " << emu::timestamp() << " Marathon launch" << (emu::dryRun() ? " (dry run)" : "") << "\n";
            out << "  " << joinArgs(spec.argv) << "\n";
            if (!spec.workingDir.empty()) out << "  cwd " << spec.workingDir << "\n";
            for (auto& [k, v] : spec.env) out << "  env " << k << "=" << v << "\n";
            for (auto& n : notes) out << "  [marathon] " << n << "\n";
        }
    }
    if (!visible) spec.logFile = pathToUtf8(log);
    if (emu::dryRun()) {
        r.ok = true;
        return r;
    }
    std::string error;
    r.pid = spawnProcess(spec, error);
    if (r.pid <= 0) {
        r.error = error.empty() ? "Could not start " + spec.argv.front() : error;
        return r;
    }
    r.ok = true;
    return r;
}

struct Picture {
    int scaleX = 1, scaleY = 1;  // internal render scale of the 1280x720 frame
    double ratio = 0;            // > 0: the 3D view is widened to this width/height
};

Picture picture(const LaunchRequest& req) {
    Picture p;
    p.scaleY = emu::renderScale(req, kNativeHeight, kMaxScaleY);
    p.scaleX = p.scaleY;
    const double r = aspectRatio(req.settings.aspect, req.display.width, req.display.height);
    // Like upstream's launcher: only a full screen wider than 16:9 is filled; anything else
    // keeps the original 16:9 picture with bars (4:3 would only squeeze the 3D view). A
    // window stays 16:9: this SDK also takes a non-default window_width/height as the
    // guest's video mode (BUILDING.md: "Startup window size is deliberately not exposed").
    if (r > kOriginalAspect + 0.01 && req.settings.fullscreen) {
        p.ratio = r;
        // The frame stays 16:9 and is stretched sideways, so its width gets proportionally
        // more pixels (the launcher's own rule).
        p.scaleX = std::clamp(static_cast<int>(std::ceil(p.scaleY * r / kOriginalAspect - 0.01)), p.scaleY, kMaxScaleAxis);
    }
    return p;
}

// Every cvar Marathon owns, as --name=value flags.
std::vector<std::string> settingFlags(const LaunchRequest& req, Layout layout, std::vector<std::string>& notes) {
    const GameSettings& s = req.settings;
    const Picture pic = picture(req);
    const bool widen = pic.ratio > 0;
    std::vector<std::string> f;
    auto set = [&](const std::string& key, const std::string& value) { f.push_back("--" + key + "=" + value); };

    set("fullscreen", emu::boolStr(s.fullscreen));
    // resolution_scale as well: one in the file would otherwise win over x = y = 1.
    set("resolution_scale", std::to_string(pic.scaleY));
    set("draw_resolution_scale_x", std::to_string(pic.scaleX));
    set("draw_resolution_scale_y", std::to_string(pic.scaleY));

    set("rr6_aspect_ratio", widen ? emu::fmtNumber(pic.ratio) : "0");
    set("present_letterbox", emu::boolStr(!widen));  // widened: the 16:9 frame is stretched over the screen
    set("rr6_hud_fix", "true");                       // menus and HUD keep their shape when widened
    set("rr6_hud_edges", emu::boolStr(jget(s.extra, "hudEdges", true)));  // race HUD at the screen edges
    set("swap_post_effect", s.antiAlias == "fxaa" ? "fxaa" : "none");

    // The game's speed follows the guest's 60 Hz vertical blank, which `vsync` drives.
    set("vsync", "true");
    // No tearing: vsync_to_display in SDK fork builds from v0.10.0.101 on (paces the guest
    // by a 60/120/240 Hz display; an older runtime logs it as unknown), and the presenter's
    // own switch for the runtime of v0.1.5 / Linux test build 06 (v0.10.0.100).
    set("vsync_to_display", emu::boolStr(s.vsync));
    if (layout == Layout::WindowsZip)
        set("d3d12_allow_variable_refresh_rate_and_tearing", emu::boolStr(!s.vsync));
    else
        set("vulkan_allow_present_mode_immediate", emu::boolStr(!s.vsync));
    if ((layout == Layout::MacApp || layout == Layout::MacDevBuild) && pic.scaleY >= 2)
        set("window_high_pixel_density", "true");  // a Retina back buffer, so the extra detail shows

    set("log_file", pathToUtf8(paths().logs / "rr6-recomp-game.log"));
    for (auto& flag : ctl::rr6ControllerFlags("", jget(s.extra, "logInput", false))) f.push_back(flag);

    notes.push_back("render " + std::to_string(1280 * pic.scaleX) + "x" + std::to_string(720 * pic.scaleY) +
                    (widen ? ", 3D view widened to " + emu::fmtNumber(pic.ratio) + ":1" : ", 16:9"));
    if (s.aspect == "4:3") notes.push_back("4:3 is not offered by the recomp: original 16:9");
    if (!widen && !s.fullscreen && aspectRatio(s.aspect, req.display.width, req.display.height) > kOriginalAspect + 0.01)
        notes.push_back("a wider picture needs full screen: the window stays 16:9");
    if (resolveFps(s) != 0 && layout != Layout::MacApp && layout != Layout::MacDevBuild)
        notes.push_back("the game runs at 60 fps (its speed is tied to 60 Hz)");
    if (s.antiAlias == "msaa" || s.antiAlias == "ssaa") notes.push_back(s.antiAlias + " is not available: none used");
    return f;
}

// ridge-racer-6.sh writes bin/rr6_recomp.toml on its first start. Marathon starts the
// program itself, so a copy it installed gets the same defaults (keyboard on with the
// documented keys, foliage fix, 16x texture filtering); a linked one is left alone.
void seedLinuxConfig(const Install& in, const BackendInstall* bi) {
    const fs::path cfg = in.root / "bin" / "rr6_recomp.toml";
    if (pathExists(cfg) || !bi || bi->userProvided) return;
    std::string text =
        "# Ridge Racer 6 settings, first written by Ridge Racer Marathon with the defaults of\n"
        "# ridge-racer-6.sh. Marathon passes the picture settings on the command line (they win\n"
        "# over this file); change the rest here or with F4 in the game.\n"
        "use_fuzzy_alpha_epsilon = true      # stops trees and foliage flickering\n"
        "anisotropic_override = 5            # texture detail on the road: 3 = 4x, 4 = 8x, 5 = 16x\n"
        "user_language = 1                   # 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian\n"
        "mnk_mode = true                     # false = ignore the keyboard\n"
        "mnk_mouse = false\n";
    const char* keys[][2] = {{"lstick_left", "Left,A"},   {"lstick_right", "Right,D"}, {"lstick_up", "Up,W"},
                             {"lstick_down", "Down,S"},   {"right_trigger", "Up,W"},   {"left_trigger", "Down,S"},
                             {"a", "Space"},              {"b", "Backspace,B"},        {"x", "X"},
                             {"y", "Y"},                  {"right_shoulder", "E"},     {"left_shoulder", "Q"},
                             {"start", "Return,P"},       {"back", "Tab"},             {"dpad_up", "Numpad8"},
                             {"dpad_down", "Numpad2"},    {"dpad_left", "Numpad4"},    {"dpad_right", "Numpad6"},
                             {"rstick_up", "I"},          {"rstick_down", "K"},        {"rstick_left", "J"},
                             {"rstick_right", "L"},       {"lstick_press", "N"},       {"rstick_press", "M"}};
    for (auto& k : keys) text += std::string("keybind_") + k[0] + " = \"" + k[1] + "\"\n";
    emu::writeText(cfg, text);
}

#if defined(__linux__)
// The game keeps guest memory in /dev/shm/xenia_memory_*, left behind when it is stopped
// hard; ridge-racer-6.sh clears them while no copy runs, and so does Marathon.
void clearShmLeftovers() {
    if (emu::dryRun()) return;
    const std::string pgrep = findOnPath("pgrep");
    if (pgrep.empty()) return;
    ProcessSpec p;
    p.argv = {pgrep, "-x", "rr6_recomp"};
    if (runProcess(p) != 1) return;  // running (0), or unknown
    std::error_code ec;
    for (auto& e : fs::directory_iterator("/dev/shm", ec))
        if (pathToUtf8(e.path().filename()).rfind("xenia_memory_", 0) == 0) fs::remove(e.path(), ec);
}
#endif

#if defined(_WIN32)
// prepare-game.ps1 takes the single .iso inside a folder: the ISO's own folder when it
// holds no other .iso, otherwise a link to it in Marathon's data folder (a hard link on
// the same volume, else a symbolic link). Empty if neither works.
fs::path isoFolderFor(const fs::path& iso, const fs::path& dataDir) {
    std::error_code ec;
    int count = 0;
    for (auto& e : fs::directory_iterator(iso.parent_path(), ec))
        if (e.is_regular_file(ec) && emu::endsWithI(pathToUtf8(e.path().filename()), ".iso")) ++count;
    if (count == 1) return iso.parent_path();
    const fs::path dir = dataDir / "disc";
    const fs::path link = dir / iso.filename();
    fs::create_directories(dir, ec);
    if (fs::exists(link, ec) && fs::equivalent(link, iso, ec)) return dir;
    for (auto& e : fs::directory_iterator(dir, ec)) fs::remove(e.path(), ec);  // links to other images
    ec.clear();
    fs::create_hard_link(iso, link, ec);
    if (!ec) return dir;
    ec.clear();
    fs::create_symlink(iso, link, ec);
    return ec ? fs::path() : dir;
}

std::string psQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        out += c;
        if (c == '\'') out += '\'';
    }
    return out + "'";
}

fs::path systemFolder() {
    const char* root = std::getenv("SystemRoot");
    return utf8ToPath(root && *root ? root : "C:\\Windows") / "System32";
}
#endif

// The source folder of a macOS development build (holds rr6_recomp_manifest.toml), or empty.
fs::path devRepo(const Install& in) {
    for (fs::path p = in.root.parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path())
        if (pathExists(p / "rr6_recomp_manifest.toml")) return p;
    return {};
}

// The package's controller database, or empty when it has none (or passes it itself).
fs::path mappingsFile(const Install& in) {
    fs::path p;
    switch (in.layout) {
        case Layout::WindowsZip: p = in.root / "gamecontrollerdb.txt"; break;
        case Layout::LinuxTarball: p = in.root / "bin" / "gamecontrollerdb.txt"; break;
        case Layout::MacDevBuild:
            if (const fs::path repo = devRepo(in); !repo.empty()) p = repo / "gamecontrollerdb.txt";
            break;
        case Layout::MacApp: return {};  // rr6-launcher passes Contents/Resources/gamecontrollerdb.txt
    }
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec) ? p : fs::path();
}

class Rr6Recomp : public Backend {
public:
    using Backend::Backend;

    ControlsInfo controls(const LaunchRequest&) const override {
        ControlsInfo info;  // the game's own controls: none of Marathon's settings apply
        info.layout = {"Left stick: steer (analog)", "RT / LT: accelerate / brake (analog)", "B / Y: nitrous", "Start: pause"};
        info.menu = "Back + Start held 1 s, or Esc: 'Quit Ridge Racer 6?' (A confirms)";
        return info;
    }

    bool linkExisting(Library& lib, const std::string& path, std::string& error) override {
        std::error_code ec;
        auto acceptable = [](const std::string& candidate) {
            Install probe;
            return locate(candidate, probe) && nativeLayout(probe.layout);
        };
        std::string exe;
        if (fs::is_regular_file(utf8ToPath(path), ec)) exe = path;
        else if (fs::is_directory(utf8ToPath(path), ec)) {
            // Filter every match, including mixed folders, before changing the saved install.
            for (const char* glob : {"*Contents/MacOS/rr6-launcher", "*Contents/MacOS/rr6_recomp", "*RR6 Launcher.exe",
                                     "*rr6_recomp.exe", "*ridge-racer-6.sh", "*rr6_recomp"}) {
                exe = findInTree(path, glob, 6, acceptable);
                if (!exe.empty()) break;
            }
        }
        Install probe;
        if (exe.empty() || !locate(exe, probe) || !nativeLayout(probe.layout)) {
            error = platformProblem();
            return false;
        }
        BackendInstall bi;
        bi.backendId = def_.id;
        bi.version = "user";
        bi.root = pathToUtf8(probe.root);
        bi.executable = exe;
        bi.userProvided = true;
        lib.setInstall(bi);
        lib.save();
        error.clear();
        return true;
    }

    std::vector<std::string> missingRequirements(const LaunchRequest& req) const override {
        std::vector<std::string> out = Backend::missingRequirements(req);
        if (req.library) {
            Install in;
            const std::string exe = installedExecutable(*req.library);
            if (!exe.empty() && locate(exe, in) && !nativeLayout(in.layout)) out.push_back(platformProblem());
        }
#if defined(_WIN32)
        // The release's launcher and .bat files check the same two files.
        if (!pathExists(systemFolder() / "vcruntime140_1.dll") || !pathExists(systemFolder() / "msvcp140_atomic_wait.dll"))
            out.push_back("Ridge Racer 6 Recompiled needs the Microsoft Visual C++ 2015-2022 runtime (x64): "
                          "https://aka.ms/vs/17/release/vc_redist.x64.exe");
#endif
        return out;
    }

    LaunchResult launch(const LaunchRequest& req) override {
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        if (exe.empty()) return emu::fail(def_.name + " is not installed.");
        Install in;
        if (!locate(exe, in)) return emu::fail("Not a Ridge Racer 6 Recompiled program: " + exe);
        if (!nativeLayout(in.layout)) return emu::fail(platformProblem());
        if (!req.entry || !req.entry->hasFiles()) return emu::fail("Add your Ridge Racer 6 (USA) disc image first.");

        const fs::path folder = extractedFolder(req);
        if (!folder.empty() && sha256File(pathToUtf8(folder / "default.xex")) != kUsaXexSha256)
            return emu::fail("This game folder is not the USA version of Ridge Racer 6, the only one the recomp was "
                             "made from (its default.xex does not match).");
        const fs::path iso = folder.empty() ? utf8ToPath(req.entry->files.path) : fs::path();
        std::vector<std::string> notes;
        const std::vector<std::string> flags = settingFlags(req, in.layout, notes);
        const fs::path mappings = mappingsFile(in);
        notes.push_back(ctl::noteLine(req.controllers, "left to Ridge Racer 6 (its own controls)" +
                                                           (mappings.empty() ? std::string()
                                                                             : "; controller database " + pathToUtf8(mappings))));

        req_ = &req;  // for frame generation in the macOS launches
        switch (in.layout) {
            case Layout::MacApp: return launchMacApp(in, folder, iso, flags, notes);
            case Layout::MacDevBuild: return launchMacDev(in, folder, flags, notes);
            default: return launchPackage(req, in, folder, iso, flags, notes);
        }
    }

private:
    // The Windows zip and the Linux tarball.
    LaunchResult launchPackage(const LaunchRequest& req, const Install& in, const fs::path& folder, const fs::path& iso,
                               const std::vector<std::string>& flags, std::vector<std::string>& notes) {
        const bool windows = in.layout == Layout::WindowsZip;
        if (!pathExists(in.program)) return emu::fail("The game program is missing: " + pathToUtf8(in.program));
        const fs::path ownCopy = emu::dataDir(def_.id) / "game";
        fs::path game = folder;
        if (game.empty() && gameReady(in.root / "game", true)) game = in.root / "game";  // from the official launcher
        if (game.empty() && gameReady(ownCopy, true)) game = ownCopy;
        const bool prepare = game.empty();
        if (prepare) game = ownCopy;

        std::vector<std::string> gameArgs = {pathToUtf8(in.program), "--game_data_root=" + pathToUtf8(game),
                                             "--gpu_plugin=xenos"};
        for (auto& flag : ctl::rr6ControllerFlags(pathToUtf8(mappingsFile(in)), false)) gameArgs.push_back(flag);
        gameArgs.insert(gameArgs.end(), flags.begin(), flags.end());
        ProcessSpec spec;
        // gamecontrollerdb.txt is found in the folder the game starts in: the package root on
        // Windows, bin/ on Linux, as the release's own launcher and script start it.
        spec.workingDir = pathToUtf8(windows ? in.root : in.root / "bin");
        if (!windows) {
            const char* ld = std::getenv("LD_LIBRARY_PATH");
            spec.env["LD_LIBRARY_PATH"] = pathToUtf8(in.root / "bin") + (ld && *ld ? ":" + std::string(ld) : "");
            // X11 (XWayland on Wayland desktops) is what the test builds have been run on.
            if (!std::getenv("SDL_VIDEO_DRIVER")) spec.env["SDL_VIDEO_DRIVER"] = "x11";
            if (!std::getenv("SDL_VIDEODRIVER")) spec.env["SDL_VIDEODRIVER"] = "x11";
            seedLinuxConfig(in, req.library ? req.library->install(def_.id) : nullptr);
#if defined(__linux__)
            clearShmLeftovers();
#endif
        }
        if (!prepare) {
            spec.argv = gameArgs;
            return spawn(def_.id, spec, notes);
        }

        // First start: the release's own tool copies the game files out of the ISO (about
        // 6 GB, a few minutes), then the game starts, in one process.
        notes.push_back("first start: copying the game files out of " + pathToUtf8(iso) + " into " + pathToUtf8(game) +
                        " (about 6 GB, once)");
        if (!windows) {
            const fs::path extract = in.root / "bin" / "rr6-extract";
            if (!pathExists(extract)) return emu::fail("The package's copy tool is missing: " + pathToUtf8(extract));
            // rr6-extract <iso> <folder> exits 0 once the files are complete and verified.
            spec.argv = {"/bin/sh", "-c", "\"$0\" \"$1\" \"$2\" || exit $?; shift 2; exec \"$@\"", pathToUtf8(extract),
                         pathToUtf8(iso), pathToUtf8(game)};
            spec.argv.insert(spec.argv.end(), gameArgs.begin(), gameArgs.end());
            return spawn(def_.id, spec, notes);
        }
#if defined(_WIN32)
        const fs::path script = in.root / "tools" / "prepare-game.ps1";
        const fs::path isoDir = pathExists(script) ? isoFolderFor(iso, emu::dataDir(def_.id)) : fs::path();
        if (isoDir.empty()) {
            // The ISO cannot be handed over: the official launcher asks for it ("Choose disc
            // image...") and copies into <pkg>/game, which Marathon uses from then on.
            const fs::path gui = in.root / "RR6 Launcher.exe";
            if (!pathExists(gui))
                return emu::fail("The game files have not been copied out of your disc image yet, and the "
                                 "package's copy tool is missing.");
            notes.push_back("the ISO could not be linked for prepare-game.ps1: opening the official launcher");
            spec.argv = {pathToUtf8(gui)};
            return spawn(def_.id, spec, notes);
        }
        // A small script in Marathon's data folder runs prepare-game.ps1, then the game; its
        // console window shows the copy's progress.
        std::string ps = "\xEF\xBB\xBF# Written by Ridge Racer Marathon: copies the game files out of your disc image\n"
                         "# once with the release's tools\\prepare-game.ps1, then starts Ridge Racer 6.\n"
                         "$Host.UI.RawUI.WindowTitle = 'Ridge Racer 6: copying the game files (first start only)'\n"
                         "try {\n  & " + psQuote(pathToUtf8(script)) + " -IsoDir " + psQuote(pathToUtf8(isoDir)) +
                         " -OutDir " + psQuote(pathToUtf8(game)) + "\n  $code = $LASTEXITCODE\n"
                         "} catch { Write-Host $_ -ForegroundColor Red; $code = 1 }\n"
                         "if ($code -ne 0) {\n  Write-Host ''\n"
                         "  Read-Host 'The game files are not ready (see above). Press Enter to close'\n  exit $code\n}\n"
                         "Set-Location -LiteralPath " + psQuote(pathToUtf8(in.root)) + "\n&";
        for (auto& a : gameArgs) ps += " " + psQuote(a);
        ps += " | Out-Null\nexit $LASTEXITCODE\n";  // Out-Null waits for the game window to close
        const fs::path wrapper = emu::dataDir(def_.id) / "first-start.ps1";
        if (!emu::writeText(wrapper, ps)) return emu::fail("Could not write " + pathToUtf8(wrapper));
        spec.argv = {pathToUtf8(systemFolder() / "WindowsPowerShell" / "v1.0" / "powershell.exe"), "-NoProfile",
                     "-ExecutionPolicy", "Bypass", "-File", pathToUtf8(wrapper)};
        return spawn(def_.id, spec, notes, /*visible=*/true);
#else
        return emu::fail("The Windows package can only copy its game files on Windows.");
#endif
    }

    // "Ridge Racer 6.app" from macos/make-package.sh.
    LaunchResult launchMacApp(const Install& in, const fs::path& folder, const fs::path& iso,
                              const std::vector<std::string>& flags, std::vector<std::string>& notes) {
        // The app's own data folder (game files, settings, saves), shared with starting it from Finder.
        const char* home = std::getenv("HOME");
        const fs::path data = utf8ToPath(home ? home : "") / "Library" / "Application Support" / "Ridge Racer 6";
        const bool ready = gameReady(data / "game", true);
        ProcessSpec spec;
        if (!folder.empty() && !ready) {
            // An extracted folder: the app only imports disc images, so its game program is
            // started the way the app starts it, with the folder as the game data.
            const fs::path contents = in.root / "Contents";
            spec.argv = {pathToUtf8(contents / "MacOS" / "rr6_recomp"), "--game_data_root=" + pathToUtf8(folder),
                         "--user_data_root=" + pathToUtf8(data / "userdata"), "--gpu_plugin=xenos",
                         "--hid_mappings_file=" + pathToUtf8(contents / "Resources" / "gamecontrollerdb.txt"),
                         "--rr6_dlc_folder=" + pathToUtf8(data / "DLC")};
            spec.argv.insert(spec.argv.end(), flags.begin(), flags.end());
            spec.env["RR6_MACOS_RELEASE_USER_ROOT"] = pathToUtf8(data);
            frameGeneration(spec, notes);
            return spawn(def_.id, spec, notes);
        }
        spec.argv = {pathToUtf8(in.program)};
        if (!ready) {
            spec.argv.push_back(pathToUtf8(iso));  // copied once, with the app's own progress window
            notes.push_back("first start: the app copies the game files out of " + pathToUtf8(iso) + " (about 6 GB)");
        }
        spec.argv.push_back("--play");  // skip the app's settings window
        spec.argv.push_back("--");      // the rest goes to the game, replacing same-named defaults
        spec.argv.insert(spec.argv.end(), flags.begin(), flags.end());
        frameGeneration(spec, notes);  // inherited by the game program the launcher starts
        return spawn(def_.id, spec, notes);
    }

    // The development build (build-macos.sh), started the way run-macos.sh does.
    LaunchResult launchMacDev(const Install& in, const fs::path& folder, const std::vector<std::string>& flags,
                              std::vector<std::string>& notes) {
        const fs::path repo = devRepo(in);
        const fs::path ownCopy = emu::dataDir(def_.id) / "game";
        fs::path game = folder;
        if (game.empty() && !repo.empty() && gameReady(repo.parent_path() / "game", false)) game = repo.parent_path() / "game";
        if (game.empty() && gameReady(ownCopy, true)) game = ownCopy;
        if (game.empty())
            return emu::fail("This development build runs from the game files beside its source folder (../game, "
                             "extracted by build-macos.sh), and they were not found. Link the packaged "
                             "\"Ridge Racer 6.app\" (macos/make-package.sh) instead.");
        ProcessSpec spec;
        spec.argv = {pathToUtf8(in.program), "--game_data_root=" + pathToUtf8(game), "--gpu_plugin=xenos"};
        if (!repo.empty()) {
            spec.argv.push_back("--user_data_root=" + pathToUtf8(repo / "out" / "userdata"));
            spec.argv.push_back("--rr6_dlc_folder=" + pathToUtf8(repo / "out" / "DLC"));
            spec.workingDir = pathToUtf8(repo);
        }
        for (auto& flag : ctl::rr6ControllerFlags(pathToUtf8(mappingsFile(in)), false)) spec.argv.push_back(flag);
        spec.argv.insert(spec.argv.end(), flags.begin(), flags.end());
        frameGeneration(spec, notes);
        return spawn(def_.id, spec, notes);
    }

    // Above 60 fps the game itself would speed up (its logic follows the 60 Hz vblank), so higher
    // rates come from frame generation: MoltenVK presents, the present library inserts images.
    void frameGeneration(ProcessSpec& spec, std::vector<std::string>& notes) const {
        if (req_) emu::applyFrameGeneration(spec, spec.argv.front(), *req_, 60.0, notes);
    }

    const LaunchRequest* req_ = nullptr;
};

}  // namespace

std::unique_ptr<Backend> makeRr6Recomp(const BackendDef& def) { return std::make_unique<Rr6Recomp>(def); }

}  // namespace rrm
