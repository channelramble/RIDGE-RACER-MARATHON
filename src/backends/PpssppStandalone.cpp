// PPSSPP (standalone) for Ridge Racer and Ridge Racer 2 (PSP).
//
// Install: the official hrydgard/ppsspp GitHub release (Windows zip with
// PPSSPPWindows64.exe, Linux "anylinux" AppImage, macOS PPSSPPSDL.app universal
// zip), or a linked existing installation.
//
// Marathon runs PPSSPP with its OWN memory stick (ppsspp.ini, saves, cheats,
// texture packs), so the user's PPSSPP is never touched. PPSSPP has no command
// line switch for the memory stick, so it is redirected per OS, exactly as
// PPSSPP's own start-up code locates it (UI/NativeApp.cpp, Windows/main.cpp):
//   Windows  installed.txt beside the exe holds the memory stick path. Only for
//            Marathon's own copy; a linked copy gets --config=<Marathon ini> and
//            keeps its own memory stick (no cheats or texture packs then).
//   macOS    $HOME/.config/ppsspp (SDL build), so HOME points into Marathon's
//            data. Marathon's copy also gets its own bundle id, because PPSSPP
//            reads a custom memory stick path from NSUserDefaults.
//   Linux    $XDG_CONFIG_HOME/ppsspp.
// (--appendconfig is no use: PPSSPP saves the merged values into the main ini.)
//
// Ultrawide: the CWCheat widescreen codes of the European discs (Saramagrean/
// CWCheat-Database-Plus-) write the game's projection aspect as a float, so any
// ratio works. Marathon writes PSP/Cheats/<DISC_ID>.ini with the exact ratio and
// sets the display layout's aspect multiplier to match, which is correct in a
// window of any shape (the database's "Stretch" layout only matches when the
// window already has the cheat's shape). The 2D HUD is stretched with the image.
// Other regions have no such code; they stay at the PSP's own 480x272 shape.
//
// The PSP games run at 60 fps; PPSSPP has no frame generation, so nothing above 60.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "backends/Adapters.h"
#include "backends/EmuCommon.h"
#include "backends/PspCommon.h"
#include "core/ConfigFiles.h"

namespace rrm {

namespace {

using namespace emu;
using namespace psp;

// ---- what to apply -------------------------------------------------------------------------------

// Marathon's own copy; a linked copy on Windows (own memory stick); a linked copy elsewhere (redirected).
enum class Layout { Marathon, LinkedWindows, Linked };

struct Plan {
    std::string discId;
    double ratio = 0;                  // projection = presentation aspect when the code applies
    const WideCheat* cheat = nullptr;  // set when the widescreen code is applied
    std::vector<std::string> warnings;
};

Plan makePlan(const LaunchRequest& req, Layout layout) {
    Plan p;
    p.discId = discIdFor(req);
    const double wanted = aspectRatio(req.settings.aspect, req.display.width, req.display.height);
    // Only wider than the PSP screen needs the code; 16:9 is the PSP's own shape (PPSSPP trims 2 lines).
    if (wanted <= kNativeAspect + 0.03) return p;
    const WideCheat* cheat = wideCheatFor(p.discId);
    if (!cheat) {
        if (p.discId.empty())
            p.warnings.push_back("The disc id of this image (.chd/.pbp) is unknown, so the widescreen code is off. Use an "
                                 ".iso/.cso, or set the advanced option discId (e.g. UCES00422).");
        else
            p.warnings.push_back("Ultrawide needs the European disc (UCES-00422 Ridge Racer 2, UCES-00002 Ridge Racer); "
                                 "this disc is " + p.discId + ", so the game stays at the PSP's own 16:9.");
        return p;
    }
    if (layout == Layout::LinkedWindows) {
        p.warnings.push_back("A linked PPSSPP on Windows keeps its own memory stick, so Marathon cannot add the widescreen "
                             "code. Install PPSSPP from Marathon for ultrawide.");
        return p;
    }
    p.cheat = cheat;
    p.ratio = std::min(wanted, 4.0);
    return p;
}


// ---- the backend -----------------------------------------------------------------------------

class PpssppStandalone : public Backend {
public:
    using Backend::Backend;

    bool install(Library& lib, TaskProgress& progress) override {
        if (!Backend::install(lib, progress)) return false;
#if defined(__APPLE__)
        // NSUserDefaults is keyed by bundle id: give Marathon's copy its own, so a custom memory
        // stick folder chosen in the user's PPSSPP never applies here (and the other way round).
        if (const BackendInstall* bi = lib.install(def_.id); bi && !bi->executable.empty()) {
            fs::path app;
            for (fs::path q = utf8ToPath(bi->executable).parent_path(); !q.empty() && q != q.root_path(); q = q.parent_path())
                if (endsWithI(pathToUtf8(q.filename()), ".app")) {
                    app = q;
                    break;
                }
            if (!app.empty()) {
                ProcessSpec plist, sign;
                plist.argv = {"/usr/bin/plutil", "-replace", "CFBundleIdentifier", "-string", "org.ppsspp.ppsspp.marathon",
                              pathToUtf8(app / "Contents" / "Info.plist")};
                sign.argv = {"/usr/bin/codesign", "--force", "--deep", "--sign", "-", pathToUtf8(app)};
                if (runProcess(plist) != 0 || runProcess(sign) != 0)
                    progress.addLog("warning: could not give Marathon's PPSSPP its own bundle id");
            }
        }
#endif
        return true;
    }

    std::vector<std::string> missingRequirements(const LaunchRequest& req) const override {
        auto out = Backend::missingRequirements(req);
        if (!req.entry || !req.entry->hasFiles() || !req.library) return out;
        const BackendInstall* bi = req.library->install(def_.id);
        const Layout layout = layoutFor(bi);
        const Plan plan = makePlan(req, layout);
        for (auto& w : plan.warnings) out.push_back(kWarningPrefix + w);
        if (const std::string pack = jget<std::string>(req.settings.extra, "texturePack", ""); !pack.empty()) {
            std::error_code ec;
            if (layout == Layout::LinkedWindows)
                out.push_back(std::string(kWarningPrefix) + "Texture packs need the PPSSPP that Marathon installs.");
            else if (plan.discId.empty())
                out.push_back(std::string(kWarningPrefix) + "Texture pack skipped: the disc id of this image is unknown.");
            else if (!fs::exists(utf8ToPath(pack), ec))
                out.push_back(std::string(kWarningPrefix) + "Texture pack not found: " + pack);
            else if (fs::is_directory(utf8ToPath(pack), ec))  // zips are only looked into at launch (unpacking)
                if (const std::string e = packProblem(pack, plan.discId); !e.empty()) out.push_back(kWarningPrefix + e);
        }
#if defined(__APPLE__)
        // A linked macOS PPSSPP shares NSUserDefaults with its Marathon launch: a custom
        // memory stick folder chosen there overrides the redirected HOME. Asked once per session.
        if (bi && bi->userProvided && !dryRun()) {
            static const bool customMemstick = [] {
                ProcessSpec q;
                q.argv = {"/usr/bin/defaults", "read", "org.ppsspp.ppsspp", "UserPreferredMemoryStickDirectoryPath"};
                std::string value;
                return runProcess(q, &value) == 0 && value.find('/') != std::string::npos;
            }();
            if (customMemstick)
                out.push_back(std::string(kWarningPrefix) +
                              "Your PPSSPP uses a custom memory stick folder, which also applies when Marathon starts it, so "
                              "Marathon's settings and widescreen code are not seen. Install PPSSPP from Marathon instead.");
        }
#endif
        return out;
    }

    LaunchResult launch(const LaunchRequest& req) override {
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        if (exe.empty()) return fail(def_.name + " is not installed.");
        if (!req.entry || !req.entry->hasFiles()) return fail("Add your " + req.game->title + " disc image (.iso/.cso) first.");
        const Layout layout = layoutFor(req.library->install(def_.id));
        const GameSettings& s = req.settings;
        const fs::path data = dataDir(def_.id);
        const Plan plan = makePlan(req, layout);

        ProcessSpec spec;
        // --pause-menu-exit: "Exit to menu" quits, back to Marathon. Fullscreen flags are not saved.
        spec.argv = {exe, s.fullscreen ? "--fullscreen" : "--windowed", "--pause-menu-exit"};
        spec.workingDir = pathToUtf8(programFolder(exe));
        fs::path memstick, mainIni;
        if (layout == Layout::LinkedWindows) {
            mainIni = data / "ppsspp.ini";
            spec.argv.push_back("--config=" + pathToUtf8(mainIni));
        } else {
            memstick = memstickDir(data);
            mainIni = memstick / "PSP" / "SYSTEM" / "ppsspp.ini";
            std::string error;
            if (!redirectMemstick(exe, data, memstick, layout, spec, error)) return fail(error);
        }

        bool texturePack = false;
        std::vector<fs::path> inis{mainIni};
        if (!memstick.empty() && !plan.discId.empty()) {
            std::error_code ec;
            // A per-game ini made in Marathon's PPSSPP overrides the main one for this disc: keep it in step.
            const fs::path gameIni = memstick / "PSP" / "SYSTEM" / utf8ToPath(plan.discId + "_ppsspp.ini");
            if (fs::exists(gameIni, ec)) inis.push_back(gameIni);

            const fs::path dest = memstick / "PSP" / "TEXTURES" / utf8ToPath(plan.discId);
            if (const std::string setting = jget<std::string>(s.extra, "texturePack", ""); !setting.empty()) {
                const PackSource pack = findPack(setting, plan.discId);
                std::string note;
                texturePack = pack.error.empty() && installPack(pack, dest, note);
                if (!pack.error.empty()) note = pack.error;
                if (!note.empty()) logNote(note);
            } else if (ownedPackDir(dest)) {
                // The pack was switched off: drop Marathon's link or copy.
                if (fs::is_symlink(dest, ec)) fs::remove(dest, ec);
                else fs::remove_all(dest, ec);
            }
            if (!writeCheatFile(memstick / "PSP" / "Cheats" / utf8ToPath(plan.discId + ".ini"), plan.discId, plan.cheat,
                                plan.ratio))
                return fail("Could not write the PPSSPP cheat file.");
        }
        for (auto& ini : inis)
            if (!writeIni(ini, req, plan, texturePack)) return fail("Could not write " + pathToUtf8(ini));

        spec.argv.push_back(gamePath(req));
        return spawnLogged(def_.id, spec);
    }

private:
    static Layout layoutFor(const BackendInstall* bi) {
        if (!bi || !bi->userProvided) return Layout::Marathon;
        return currentOs() == Os::Windows ? Layout::LinkedWindows : Layout::Linked;
    }

    // Where PPSSPP looks for the memory stick once redirectMemstick() has set things up.
    static fs::path memstickDir(const fs::path& data) {
        switch (currentOs()) {
            case Os::Windows: return data / "memstick";
            case Os::MacOS: return data / "home" / ".config" / "ppsspp";
            default: return data / "xdg" / "ppsspp";
        }
    }

    static bool redirectMemstick(const std::string& exe, const fs::path& data, const fs::path& memstick, Layout layout,
                                 ProcessSpec& spec, std::string& error) {
        std::error_code ec;
        fs::create_directories(memstick / "PSP" / "SYSTEM", ec);
        switch (currentOs()) {
            case Os::Windows: {
                if (layout != Layout::Marathon) break;
                // Marathon's own copy: installed.txt beside the exe names the memory stick (first line, UTF-8).
                const fs::path marker = utf8ToPath(exe).parent_path() / "installed.txt";
                const std::string want = pathToUtf8(memstick);
                if (readText(marker) != want && !writeText(marker, want)) {
                    error = "Could not write " + pathToUtf8(marker);
                    return false;
                }
                break;
            }
            case Os::MacOS: spec.env["HOME"] = pathToUtf8(data / "home"); break;
            default: spec.env["XDG_CONFIG_HOME"] = pathToUtf8(data / "xdg"); break;
        }
        return true;
    }

    bool writeIni(const fs::path& path, const LaunchRequest& req, const Plan& plan, bool texturePack) const {
        const GameSettings& s = req.settings;
        cfg::Ini ini;
        ini.load(pathToUtf8(path));
        // Rendering resolution: 1..10 x 480x272 (0 would mean "auto", the window size).
        ini.set("Graphics", "InternalResolution", std::to_string(renderScale(req, 272, 10)));
        ini.set("Graphics", "VerticalSync", boolStr(s.vsync, "True", "False"));
        // Alternative speed 1 (stored in fps, 0 = unlimited) carries the fast-forward percentage
        // for its hotkey; Tab is PPSSPP's own unlimited fast-forward. There is no base speed.
        ini.set("Graphics", "FrameRate", std::to_string(s.turbo <= 0 ? 0 : std::max(1, s.turbo * 60 / 100)));
        ini.set("DisplayLayout.Landscape", "BufferFiltering", s.filter == "linear" ? "1" : "2");  // 1 linear, 2 nearest
        ini.set("DisplayLayout.Landscape", "DisplayStretch", "False");
        // The aspect multiplier is relative to 480:272; the cheat widens the 3D view to the same ratio.
        ini.set("DisplayLayout.Landscape", "DisplayAspectRatio", plan.cheat ? fmtNumber(plan.ratio / kNativeAspect) : "1");
        if (plan.cheat) ini.set("General", "EnableCheats", "True");
        if (texturePack) ini.set("Graphics", "ReplaceTextures", "True");
        ini.set("General", "CheckForNewVersion", "False");  // Marathon updates its own copy
        // Advanced: settings.extra.ppssppIni {"Section": {"Key": "Value"}} wins over the mapping.
        if (auto it = s.extra.find("ppssppIni"); it != s.extra.end() && it->is_object())
            for (auto& [section, keys] : it->items())
                if (keys.is_object())
                    for (auto& [k, v] : keys.items())
                        if (v.is_string()) ini.set(section, k, v.get<std::string>());
        return ini.save(pathToUtf8(path));
    }

    void logNote(const std::string& line) const {
        std::ofstream out(paths().logs / utf8ToPath(def_.id + ".log"), std::ios::binary | std::ios::app);
        if (out) out << "[marathon] " << line << "\n";
    }
};

}  // namespace

std::unique_ptr<Backend> makePpssppStandalone(const BackendDef& def) { return std::make_unique<PpssppStandalone>(def); }

}  // namespace rrm
