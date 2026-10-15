// Xenia Canary (xenia-canary/xenia-canary): Xbox 360 emulator for Windows (Direct3D 12 /
// Vulkan; a .7z release) and Linux (Vulkan; an AppImage). No macOS build exists. Builds were
// published in xenia-canary/xenia-canary-releases until March 2026.
//
// Storage: Xenia keeps its config (xenia-canary.config.toml), patches/, content/ (saves)
// and cache_host/ under one storage root: --storage_root if given, else the executable's
// folder in portable mode (portable.txt beside it, or --portable, which defaults to true on
// Windows), else Documents/Xenia (app/xenia_main.cc, EmulatorApp::OnInitialize). Marathon
// passes --storage_root=<its data folder>/xenia-canary, a portable setup of its own that
// works for downloaded and linked copies alike, survives updates and never touches the
// user's own Xenia folder; saves go to Marathon's save folder (--content_root).
//
// Config: Xenia reads [Category] key tables from xenia-canary.config.toml in the storage
// root and rewrites the file with every option on each start; Marathon sets only its keys.
//
// Ultrawide: the "Aspect Ratio" patch of xenia-canary/game-patches
// (patches/4E4D07D3 - Ridge Racer 6.patch.toml, by Margen67) replaces the projection's 16:9
// float at 0x82001C54 with width / height. Its module hash 98901469675D7641 is listed with
// the Europe disc's media id, but the USA disc's code is byte-identical: Xenia's module hash
// (XXH3 of the code pages after the import thunks are written, UserModule::CalculateHash)
// of the USA default.xex is the same value, and the USA recomp finds the 16:9 float at the
// same address. Xenia applies a patch only to a module with a listed hash, so other
// versions are left alone. The patch widens the 3D view only: the HUD, menus and videos are
// stretched with the frame (Xenia has no counterpart of the recomp's HUD fix).
//
// Frame rate: Xenia's GPU `vsync` and `framerate_limit` pace the guest's vertical blank, and
// Ridge Racer 6's speed is tied to it, so they stay at 60. Marathon's vsync choice maps to
// the presenter (tearing / variable refresh allowed or not).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#include "backends/Adapters.h"
#include "backends/EmuCommon.h"
#include "core/ConfigFiles.h"

namespace rrm {

namespace {

constexpr double kOriginalAspect = 16.0 / 9.0;
constexpr int kMaxScaleAxis = 7;  // texture_cache.h kMaxDrawResolutionScaleAlongAxis; clamped further per GPU
constexpr const char* kRr6TitleId = "4E4D07D3";

bool hasTitle(const LaunchRequest& req, const std::string& titleId) {
    if (!req.game) return false;
    const auto& ids = req.game->identify.xboxTitleIds;
    return std::find(ids.begin(), ids.end(), titleId) != ids.end();
}

// The bits of a 32-bit float, as the be32 value of a Xenia patch.
std::string floatBits(double value) {
    const float f = static_cast<float>(value);
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", u);
    return buf;
}

// game-patches' Ridge Racer 6 file, with only the "Aspect Ratio" patch (the other one,
// "1280x720 Resolution", lowers the game's own 1440x800 render size).
std::string rr6PatchFile(double ratio) {
    const bool widen = ratio > 0;
    return std::string("title_name = \"Ridge Racer 6\"\n"
                       "title_id = \"4E4D07D3\" # NM-2003\n"
                       "hash = \"98901469675D7641\" # default.xex (the USA and Europe discs have the same code)\n"
                       "#media_id = \"03F77EEF\" # Disc (Europe): https://redump.info/disc/57116\n"
                       "\n"
                       "# From xenia-canary/game-patches, patches/4E4D07D3 - Ridge Racer 6.patch.toml.\n"
                       "# Written by Ridge Racer Marathon with the aspect ratio of its settings.\n"
                       "\n"
                       "[[patch]]\n"
                       "    name = \"Aspect Ratio\"\n"
                       "    desc = \"See note about aspect ratio patches in the README.\"\n"
                       "    author = \"Margen67\"\n"
                       "    is_enabled = ") +
           (widen ? "true" : "false") +
           "\n"
           "\n"
           "    [[patch.be32]]\n"
           "        address = 0x82001c54\n"
           "        value = " +
           floatBits(widen ? ratio : kOriginalAspect) + "\n";
}

std::string tomlString(const std::string& s) { return "\"" + s + "\""; }

class Xenia : public Backend {
public:
    using Backend::Backend;

    // Canary publishes a rolling build per commit; GitHub lists them in an order that does
    // not follow publication, so the newest one is taken from /releases/latest. Asset names
    // vary between builds (xenia_canary_windows.zip / xenia_canary_windows_.zip).
#if defined(_WIN32)
    std::vector<std::string> missingRequirements(const LaunchRequest& req) const override {
        std::vector<std::string> out = Backend::missingRequirements(req);
        const char* root = std::getenv("SystemRoot");
        const fs::path system = utf8ToPath(root && *root ? root : "C:\\Windows") / "System32";
        std::error_code ec;
        if (!fs::exists(system / "vcruntime140_1.dll", ec) || !fs::exists(system / "msvcp140_atomic_wait.dll", ec))
            out.push_back("Xenia Canary needs the Microsoft Visual C++ 2015-2022 runtime (x64): "
                          "https://aka.ms/vs/17/release/vc_redist.x64.exe");
        return out;
    }
#endif

    bool install(Library& lib, TaskProgress& progress) override {
        const BackendSource* src = def_.sourceFor(currentPlatformKey());
        if (!src || src->kind != "github") return Backend::install(lib, progress);
        emu::RunningGuard running(progress);
        progress.setStage("Looking up the latest " + def_.name + " build");
        emu::Download dl;
        std::string error;
        fs::path dest;
        if (!emu::githubStableAsset(src->repo, src->assetPattern, dl, error) ||
            !emu::downloadAndUnpack(def_, progress, dl, src->archive, dest, error)) {
            progress.fail(error);
            return false;
        }
        const std::string exe = emu::locateExecutable(dest, src->executable);
        if (exe.empty()) {
            progress.fail("Installed, but " + src->executable + " was not found in the download.");
            return false;
        }
        emu::recordInstall(lib, def_, dl.version, dest, exe);
        progress.setStage(def_.name + " " + dl.version + " installed");
        return true;
    }

    LaunchResult launch(const LaunchRequest& req) override {
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        if (exe.empty()) return emu::fail(def_.name + " is not installed.");
        if (!req.entry || !req.entry->hasFiles()) return emu::fail("Add your game files first.");
        const GameSettings& s = req.settings;

        // An .iso, or an extracted game folder (its default.xex).
        fs::path target = utf8ToPath(req.entry->files.path);
        std::error_code ec;
        if (fs::is_directory(target, ec)) target /= "default.xex";

        // Picture: an integer scale of the 720-line frame; widened by the aspect patch, the
        // frame is stretched sideways, so its width gets proportionally more pixels.
        const int scaleY = emu::renderScale(req, 720, kMaxScaleAxis);
        const double wanted = aspectRatio(s.aspect, req.display.width, req.display.height);
        const bool patchable = hasTitle(req, kRr6TitleId);
        const double ratio = patchable && wanted > kOriginalAspect + 0.01 ? wanted : 0;
        const int scaleX = ratio > 0 ? std::clamp(static_cast<int>(std::ceil(scaleY * ratio / kOriginalAspect - 0.01)),
                                                  scaleY, kMaxScaleAxis)
                                     : scaleY;

        const fs::path storage = emu::dataDir(def_.id);
        std::vector<std::pair<std::string, std::string>> cfg = {
            {"GPU.draw_resolution_scale_x", std::to_string(scaleX)},
            {"GPU.draw_resolution_scale_y", std::to_string(scaleY)},
            // Guest vertical blank at 60 Hz: the game's speed depends on it.
            {"GPU.vsync", "true"},
            {"GPU.framerate_limit", "60"},
            {"Display.fullscreen", emu::boolStr(s.fullscreen)},
            {"Display.present_letterbox", emu::boolStr(ratio <= 0)},  // widened: fill the screen
            {"Display.postprocess_antialiasing", tomlString(s.antiAlias == "fxaa" ? "fxaa" : "none")},
            {"General.apply_patches", "true"},
            // Display sync: no tearing (and no variable refresh) with vsync on.
            {"D3D12.d3d12_allow_variable_refresh_rate_and_tearing", emu::boolStr(!s.vsync)},
            {"Vulkan.vulkan_allow_present_mode_immediate", emu::boolStr(!s.vsync)},
        };
        if (ratio > 0 && !s.fullscreen) {
            // A window of the chosen shape: the stretched frame would be distorted in a 16:9 one.
            cfg.push_back({"UI.window_size_x", std::to_string(static_cast<int>(std::lround(720 * ratio)))});
            cfg.push_back({"UI.window_size_y", "720"});
        }
        const fs::path configFile = storage / "xenia-canary.config.toml";
        std::string error;
        if (!cfg::tomlSet(pathToUtf8(configFile), cfg, &error)) return emu::fail("Could not write the Xenia config: " + error);
        if (patchable && !emu::writeText(storage / "patches" / "4E4D07D3 - Ridge Racer 6.patch.toml", rr6PatchFile(ratio)))
            return emu::fail("Could not write the Ridge Racer 6 patch file.");

        const fs::path content = paths().saves / utf8ToPath(req.game ? req.game->id : std::string("xbox360")) / def_.id;
        fs::create_directories(content, ec);
        ProcessSpec spec;
        spec.argv = {exe, "--storage_root=" + pathToUtf8(storage), "--content_root=" + pathToUtf8(content),
                     pathToUtf8(target)};
        spec.workingDir = pathToUtf8(utf8ToPath(exe).parent_path());

        std::vector<std::string> notes = {
            "config " + pathToUtf8(configFile) + ": render " + std::to_string(scaleX) + "x" + std::to_string(scaleY) +
            (ratio > 0 ? ", aspect patch " + emu::fmtNumber(ratio) + ":1 (" + floatBits(ratio) + ")" : ", 16:9")};
        if (wanted > 0 && wanted < kOriginalAspect - 0.01) notes.push_back(s.aspect + " is not offered: original 16:9");
        if (wanted > kOriginalAspect + 0.01 && !patchable) notes.push_back("no aspect ratio patch for this game");
        if (resolveFps(s) != 0) notes.push_back("the game runs at 60 fps (its speed is tied to 60 Hz)");
        {
            std::ofstream log(paths().logs / utf8ToPath(def_.id + ".log"), std::ios::binary | std::ios::app);
            log << "\n==== " << emu::timestamp() << " Marathon settings\n";
            for (auto& n : notes) log << "  [marathon] " << n << "\n";
        }
        return emu::spawnLogged(def_.id, spec);
    }
};

}  // namespace

std::unique_ptr<Backend> makeXenia(const BackendDef& def) { return std::make_unique<Xenia>(def); }

}  // namespace rrm
