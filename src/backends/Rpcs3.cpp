// RPCS3 (desktop): Ridge Racer 7 (PS3) through the official RPCS3 builds.
//
// Builds come from RPCS3/rpcs3-binaries-{win,linux,mac-arm64,mac}: one release per
// master build, the newest being /releases/latest (the release list itself is not in
// date order, so the generic installer's "first match" would pick an old build).
//
// Data folder: RPCS3 keeps config.yml, the installed PS3 system software (dev_flash),
// saves (dev_hdd0) and its caches in one "config dir". Marathon moves that to
// <Marathon config>/rpcs3 for the child process only, through the variables RPCS3's
// fs::get_config_dir()/get_cache_dir() read: RPCS3_CONFIG_DIR on Windows,
// XDG_CONFIG_HOME/XDG_CACHE_HOME on Linux, HOME on macOS. The user's own RPCS3 setup
// is never read or written. A portable/ folder beside a linked RPCS3 wins over those
// variables; then Marathon passes its settings with --config instead (and warns that
// firmware and saves are the linked install's own).
//
// Firmware: `rpcs3 --headless --installfw PS3UPDAT.PUP` installs it with no dialog.
// Marathon runs it once, right after installing RPCS3 or before the first launch.
//
// Disc: current builds (ISO loader since December 2025) boot a decrypted .iso, an
// encrypted Redump ISO with its .dkey beside it, or a PS3_GAME folder directly.
//
// Ridge Racer 7 renders 16:9 at 60 fps and the patch.yml served by rpcs3.net has no Ridge
// Racer entry. Marathon's own patch (catalog option params "aspectPatch", found by reverse
// engineering the USA disc's executable) widens the race view: the game rebuilds its
// projection every frame from one aspect constant, so setting it to the screen's shape gives
// Hor+ (more road at the sides, same vertical view). The game still renders a 16:9 image,
// which RPCS3 then stretches to the screen ("Stretch To Display Area"): so ultrawide is for
// full screen on a screen wider than 16:9, and the 2D HUD is stretched with it.
// Above 60 fps on macOS comes from frame generation (EmuCommon.h).
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>

#include "backends/Adapters.h"
#include "backends/EmuCommon.h"
#include "core/ConfigFiles.h"

namespace rrm {

namespace {

using namespace emu;

// Where RPCS3 keeps its data for one launch, and how to point it there.
struct Rpcs3Dirs {
    fs::path emuDir;       // RPCS3's config dir ($(EmulatorDir)): dev_flash, dev_hdd0, config...
    fs::path configYml;    // global config.yml inside it, or Marathon's --config file
    std::map<std::string, std::string> env;
    bool linkedPortable = false;  // a linked RPCS3 with portable/: its own folders, our --config
};

Rpcs3Dirs rpcs3Dirs(const std::string& backendId, const std::string& exe) {
    Rpcs3Dirs d;
    const fs::path root = dataDir(backendId);
    // RPCS3 checks <executable dir>/portable/ before anything else: the folder holding RPCS3.app on
    // macOS (Utilities/File.cpp: the bundle is three levels above the binary), the AppImage's own
    // folder on Linux, the exe's on Windows. Marathon never creates one in its own installs.
    std::error_code ec;
    fs::path exeDir = utf8ToPath(exe).parent_path();
    if (currentOs() == Os::MacOS && exeDir.parent_path().parent_path().extension() == ".app")
        exeDir = exeDir.parent_path().parent_path().parent_path();  // .../RPCS3.app/Contents/MacOS -> ...
    const fs::path portable = exeDir / "portable";
    if (!exe.empty() && fs::is_directory(portable, ec)) {
        d.emuDir = portable;
        d.configYml = root / "config.yml";
        d.linkedPortable = true;
        return d;
    }
    switch (currentOs()) {
        case Os::Windows:
            // The value must end with a separator: RPCS3 cuts it after the last '/'.
            d.emuDir = root / "rpcs3";
            d.configYml = d.emuDir / "config" / "config.yml";
            d.env["RPCS3_CONFIG_DIR"] = pathToUtf8(d.emuDir) + "/";
            break;
        case Os::Linux:
            d.emuDir = root / "config" / "rpcs3";
            d.configYml = d.emuDir / "config.yml";
            d.env["XDG_CONFIG_HOME"] = pathToUtf8(root / "config");
            d.env["XDG_CACHE_HOME"] = pathToUtf8(root / "cache");
            break;
        default:  // macOS: $HOME/Library/Application Support/rpcs3 and $HOME/Library/Caches/rpcs3
            d.emuDir = root / "Library" / "Application Support" / "rpcs3";
            d.configYml = d.emuDir / "config.yml";
            d.env["HOME"] = pathToUtf8(root);
            break;
    }
    return d;
}

bool firmwareInstalled(const fs::path& emuDir) {
    std::error_code ec;
    return fs::is_regular_file(emuDir / "dev_flash" / "vsh" / "etc" / "version.txt", ec);
}

std::string firmwareVersion(const fs::path& emuDir) {
    // "release:04.92:" -> "4.92"
    std::string text = readText(emuDir / "dev_flash" / "vsh" / "etc" / "version.txt");
    size_t a = text.find(':'), b = a == std::string::npos ? a : text.find(':', a + 1);
    return b == std::string::npos ? std::string() : text.substr(a + 1, b - a - 1);
}

// Installs PS3UPDAT.PUP into the config dir with `--headless --installfw` (no dialogs; it
// decrypts and unpacks dev_flash, typically 10-60 s). Success = version.txt exists afterwards.
bool installFirmware(const std::string& exe, const Rpcs3Dirs& dirs, const std::string& pup, std::string& error,
                     const std::atomic<bool>* cancel = nullptr) {
    std::error_code ec;
    if (!fs::is_regular_file(utf8ToPath(pup), ec)) {
        error = "PS3 firmware file not found: " + pup;
        return false;
    }
    fs::create_directories(dirs.emuDir, ec);
    ProcessSpec spec;
    spec.argv = {exe, "--headless", "--installfw", pup};
    spec.env = dirs.env;
    spec.workingDir = pathToUtf8(utf8ToPath(exe).parent_path());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    std::string output;
    const int rc = runProcess(spec, &output, [&] {
        return std::chrono::steady_clock::now() < deadline && !(cancel && cancel->load());
    });
    {
        std::ofstream log(paths().logs / "rpcs3-firmware.log", std::ios::binary | std::ios::app);
        log << "\n==== " << timestamp() << " " << exe << " --headless --installfw " << pup << " (exit " << rc << ")\n" << output;
    }
    if (firmwareInstalled(dirs.emuDir)) return true;
    error = "RPCS3 could not install the PS3 system software from " + pup + " (exit " + std::to_string(rc) +
            "; see logs/rpcs3-firmware.log). Use the official PS3UPDAT.PUP from playstation.com.";
    return false;
}

// Ridge Racer 7 renders 1280x720 (nativeHeight); RPCS3 scales render targets by a
// percentage (25-800, its settings dialog snaps to 25). "1080p" = 150, "4k" = 300, "8k" = 600.
int scalePercent(const LaunchRequest& req) {
    const std::string& res = req.settings.resolution;
    const int native = nativeHeight(req, 720);
    double lines = native;
    if (res.size() >= 2 && res.back() == 'x') lines = native * std::max(1, std::atoi(res.c_str()));
    else if (res == "720p") lines = 720;
    else if (res == "1080p") lines = 1080;
    else if (res == "1440p") lines = 1440;
    else if (res == "4k") lines = 2160;
    else if (res == "5k") lines = 2880;
    else if (res == "8k") lines = 4320;
    else if (res == "display") lines = req.display.height > 0 ? req.display.height : 1080;
    int percent = static_cast<int>(std::ceil(lines * 100.0 / native / 25.0 - 1e-9)) * 25;
    return std::clamp(percent, 25, 800);
}

// settings.extra.rpcs3Config {"Video": {"Resolution Scale": 250, ...}}: advanced overrides
// (nested objects follow config.yml's sections), applied last.
void applyExtraYaml(const json& node, std::vector<std::string>& path, cfg::YamlMap& yml) {
    for (auto& [key, v] : node.items()) {
        path.push_back(key);
        if (v.is_object()) applyExtraYaml(v, path, yml);
        else if (v.is_string()) yml.set(path, v.get<std::string>());
        else if (v.is_boolean()) yml.set(path, boolStr(v.get<bool>()));
        else if (v.is_number()) yml.set(path, fmtNumber(v.get<double>()));
        path.pop_back();
    }
}

// Marathon's aspect patch (see the header) for RPCS3's patch engine: params "aspectPatch" =
// {"hash": PPU hash, "game", "serial", "values": [{"address", "scale"}]}, each value a float32
// constant set to ratio * scale. RPCS3 reads patches/imported_patch.yml besides the patch.yml it
// downloads, and the on/off switch from patch_config.yml next to config.yml. Both files are
// Marathon's own (its private RPCS3 config dir); ratio 0 switches the patch off.
bool writeAspectPatch(const Rpcs3Dirs& dirs, const json& spec, double ratio, std::string& error) {
    const std::string hash = spec.value("hash", ""), game = spec.value("game", ""), serial = spec.value("serial", "");
    const json values = spec.value("values", json::array());
    if (hash.empty() || game.empty() || serial.empty() || !values.is_array() || values.empty()) {
        error = "the catalog's aspectPatch is incomplete";
        return false;
    }
    const char* name = "Ridge Racer Marathon aspect";
    std::ostringstream patch;
    patch << "# Written by Ridge Racer Marathon before each launch.\nVersion: 1.2\n\n"
          << hash << ":\n  \"" << name << "\":\n    Games:\n      \"" << game << "\":\n        " << serial
          << ": [ All ]\n    Author: \"Ridge Racer Marathon\"\n    Notes: \"View aspect " << fmtNumber(ratio)
          << "\"\n    Patch Version: 1.0\n    Patch:\n";
    for (const json& v : values) {
        const float f = static_cast<float>((ratio > 0 ? ratio : 16.0 / 9.0) * v.value("scale", 1.0));
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof bits);
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08x", bits);
        patch << "      - [ be32, " << v.value("address", "") << ", " << hex << " ]\n";
    }
    std::ostringstream config;
    config << "# Written by Ridge Racer Marathon before each launch.\n"
           << hash << ":\n  \"" << name << "\":\n    \"" << game << "\":\n      " << serial
           << ":\n        All:\n          Enabled: " << boolStr(ratio > 0) << "\n";
    const fs::path patchFile = dirs.emuDir / "patches" / "imported_patch.yml";
    const fs::path configFile = dirs.configYml.parent_path() / "patch_config.yml";
    if (!writeText(patchFile, patch.str()) || !writeText(configFile, config.str())) {
        error = "could not write " + pathToUtf8(patchFile) + " or " + pathToUtf8(configFile);
        return false;
    }
    return true;
}

void appendNotes(const std::string& backendId, const std::vector<std::string>& notes) {
    if (notes.empty()) return;
    std::ofstream out(paths().logs / utf8ToPath(backendId + ".log"), std::ios::binary | std::ios::app);
    for (auto& n : notes) out << "  [marathon] " << n << "\n";
}

class Rpcs3 : public Backend {
public:
    using Backend::Backend;

    bool install(Library& lib, TaskProgress& progress) override {
        const BackendSource* src = def_.sourceFor(currentPlatformKey());
        if (!src || src->kind != "github") return Backend::install(lib, progress);
        RunningGuard running(progress);
        progress.setStage("Looking up the latest " + def_.name + " build");
        Download dl;
        std::string error;
        if (!githubStableAsset(src->repo, src->assetPattern, dl, error)) {
            progress.fail(error);
            return false;
        }
        // "build-<commit>" says little: name the install after the asset's version
        // (rpcs3-v0.0.43-20296-80603d9c_macos_aarch64.7z -> v0.0.43-20296-80603d9c).
        if (dl.name.rfind("rpcs3-", 0) == 0) dl.version = dl.name.substr(6, dl.name.find('_') - 6);
        fs::path dest;
        if (!downloadAndUnpack(def_, progress, dl, src->archive, dest, error)) {
            progress.fail(error);
            return false;
        }
        const std::string exe = locateExecutable(dest, src->executable);
        if (exe.empty()) {
            progress.fail("Installed, but " + src->executable + " was not found in " + dl.name + ".");
            return false;
        }
        recordInstall(lib, def_, dl.version, dest, exe);
        progress.setStage(def_.name + " " + dl.version + " installed");

        // Firmware right away when the user has already supplied it (this runs on a worker thread).
        const Rpcs3Dirs dirs = rpcs3Dirs(def_.id, exe);
        const std::string pup = lib.systemFile("ps3-firmware");
        if (!pup.empty() && !firmwareInstalled(dirs.emuDir) && !dirs.linkedPortable) {
            progress.setStage("Installing the PS3 system software into RPCS3");
            if (installFirmware(exe, dirs, pup, error, &progress.cancel))
                progress.addLog("PS3 system software " + firmwareVersion(dirs.emuDir) + " installed");
            else
                progress.addLog(error + " Marathon tries again before the first launch.");
        }
        return true;
    }

    std::vector<std::string> missingRequirements(const LaunchRequest& req) const override {
        std::vector<std::string> out;
        if (!req.entry || !req.entry->hasFiles()) out.push_back("Add your " + req.game->title + " files first.");
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        const Rpcs3Dirs dirs = rpcs3Dirs(def_.id, exe);
        const bool pup = req.library && !req.library->systemFile("ps3-firmware").empty();
        if (!firmwareInstalled(dirs.emuDir) && !(pup && !dirs.linkedPortable)) {
            if (dirs.linkedPortable)
                out.push_back("Your linked RPCS3 (portable mode) has no PS3 system software installed. Install "
                              "PS3UPDAT.PUP in that RPCS3 (File > Install Firmware).");
            else
                out.push_back("RPCS3 needs the official PS3 system software (PS3UPDAT.PUP from playstation.com). "
                              "Add it under Settings > System files.");
        }
        if (dirs.linkedPortable)
            out.push_back(std::string(kWarningPrefix) +
                          "your linked RPCS3 is in portable mode, so it uses its own firmware, saves and caches; "
                          "Marathon's settings are passed with --config.");
        return out;
    }

    LaunchResult launch(const LaunchRequest& req) override {
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        if (exe.empty()) return fail(def_.name + " is not installed.");
        if (!req.entry || !req.entry->hasFiles()) return fail("Add your " + req.game->title + " disc first.");
        const GameSettings& s = req.settings;
        const Rpcs3Dirs dirs = rpcs3Dirs(def_.id, exe);
        std::vector<std::string> notes;
        notes.push_back("RPCS3 config dir: " + pathToUtf8(dirs.emuDir));

        if (!firmwareInstalled(dirs.emuDir)) {
            const std::string pup = req.library->systemFile("ps3-firmware");
            if (dryRun()) {
                notes.push_back(pup.empty() || dirs.linkedPortable ? "PS3 system software missing: a real launch stops here"
                                                                   : "first start: runs " + exe + " --headless --installfw " + pup);
            } else if (pup.empty() || dirs.linkedPortable) {
                return fail("RPCS3 needs the PS3 system software (PS3UPDAT.PUP) first.");
            } else {
                std::string error;
                if (!installFirmware(exe, dirs, pup, error)) return fail(error);
            }
        }

        // Ultrawide through Marathon's aspect patch, where the catalog has one for this game. RPCS3
        // stretches the 16:9 image to the shape of its window, so only full screen is widened, and
        // then to the screen's own shape (like the RR6 recomp's launcher): a window stays 16:9.
        const double wanted = aspectRatio(s.aspect, req.display.width, req.display.height);
        const double screen = req.display.width > 0 && req.display.height > 0
                                  ? static_cast<double>(req.display.width) / req.display.height
                                  : 16.0 / 9.0;
        const json* aspectPatch = nullptr;
        if (req.option && req.option->params.is_object())
            if (auto it = req.option->params.find("aspectPatch"); it != req.option->params.end() && it->is_object())
                aspectPatch = &*it;
        bool wide = false;
        if (wanted > 16.0 / 9.0 + 0.01) {
            const std::string serial = aspectPatch ? aspectPatch->value("serial", "") : std::string();
            const std::string& by = req.entry->files.detectedBy;
            if (!aspectPatch)
                notes.push_back(req.game->title + " has no ultrawide mode: shown at 16:9 with side bars");
            else if (dirs.linkedPortable)
                notes.push_back("ultrawide needs Marathon's own RPCS3 folders; your linked RPCS3 is in portable mode, "
                                "so the game stays 16:9");
            else if (by.rfind("title id ", 0) == 0 && by.substr(9) != serial)
                // The patch matches one executable: on another disc RPCS3 would skip it and only
                // the stretch would remain.
                notes.push_back("Marathon's ultrawide patch is for the " + serial + " disc and this one is " +
                                by.substr(9) + ": shown at 16:9 with side bars");
            else if (!s.fullscreen)
                notes.push_back("a wider picture needs full screen: the window stays 16:9");
            else if (screen <= 16.0 / 9.0 + 0.01)
                notes.push_back("this screen is 16:9, so the game stays 16:9");
            else
                wide = true;
        }
        const double ratio = screen;  // the picture fills the screen, so it has the screen's shape
        if (wide && std::abs(wanted - screen) > 0.02)
            notes.push_back("ultrawide uses the screen's shape " + fmtNumber(screen) + ":1 rather than " + fmtNumber(wanted) +
                            ":1 (the image always fills the screen)");

        cfg::YamlMap yml;
        yml.load(pathToUtf8(dirs.configYml));
        const int percent = scalePercent(req);
        yml.set({"Video", "Resolution"}, "1280x720");
        yml.set({"Video", "Resolution Scale"}, std::to_string(percent));
        // The PS3 tells the game it drives a 16:9 TV. RPCS3 keeps that shape (pillarboxed on wider
        // screens) unless the aspect patch widened the view: then the image fills the screen.
        yml.set({"Video", "Aspect ratio"}, s.aspect == "4:3" ? "4:3" : "16:9");
        yml.set({"Video", "Stretch To Display Area"}, boolStr(wide));
        // The game paces itself to the 60 Hz vblank; "Auto" follows it.
        yml.set({"Video", "Frame limit"}, "Auto");
        yml.set({"Video", "VSync Mode"}, s.vsync ? "Full" : "Disabled");
        yml.set({"Video", "MSAA"}, s.antiAlias == "off" ? "Disabled" : "Auto");
        yml.set({"Video", "Output Scaling Mode"}, s.filter == "nearest" ? "Nearest" : "Bilinear");
        // Defaults to true in RPCS3, so windowed play has to say so.
        yml.set({"Miscellaneous", "Start games in fullscreen mode"}, boolStr(s.fullscreen));
        yml.set({"Miscellaneous", "Exit RPCS3 when process finishes"}, "true");
        // Per-game keys from the catalog (option params "rpcs3Config"), then the user's own.
        for (const json* src : {req.option ? &req.option->params : nullptr, &s.extra}) {
            if (!src || !src->is_object()) continue;
            if (auto it = src->find("rpcs3Config"); it != src->end() && it->is_object()) {
                std::vector<std::string> path;
                applyExtraYaml(*it, path, yml);
            }
        }
        // An override that stops the stretch would leave the widened view squeezed into 16:9.
        if (wide && yml.get({"Video", "Stretch To Display Area"}) != "true") {
            notes.push_back("ultrawide off: your rpcs3Config override turns Stretch To Display Area off");
            wide = false;
        }
        if (!yml.save(pathToUtf8(dirs.configYml))) return fail("Could not write " + pathToUtf8(dirs.configYml));
        if (aspectPatch && !dirs.linkedPortable && !dryRun()) {
            std::string error;
            if (!writeAspectPatch(dirs, *aspectPatch, wide ? ratio : 0.0, error)) {
                notes.push_back("ultrawide patch not written: " + error);
                if (wide) {
                    // Without the patch the stretch would only distort the 16:9 picture.
                    yml.set({"Video", "Stretch To Display Area"}, "false");
                    yml.save(pathToUtf8(dirs.configYml));
                    wide = false;
                }
            }
        }
        if (wide)
            notes.push_back("ultrawide: view aspect " + fmtNumber(ratio) +
                            " (Hor+), the 16:9 image stretched to the screen; 2D HUD stretched");

        if (resolveFps(s) != 0 && currentOs() != Os::MacOS)
            notes.push_back(req.game->title + " is locked to 60 fps (no RPCS3 patch); use VSync/VRR on high refresh displays");
        if (s.speed != 100) notes.push_back("game speed is not adjustable for this game in RPCS3");

        ProcessSpec spec;
        spec.argv = {exe, "--no-gui"};
        if (s.fullscreen) spec.argv.push_back("--fullscreen");  // only valid together with --no-gui
        if (dirs.linkedPortable) {
            // Its own config.yml stays untouched; RPCS3 reads Marathon's file for this boot only.
            spec.argv.push_back("--config");
            spec.argv.push_back(pathToUtf8(dirs.configYml));
        }
        spec.argv.push_back(gamePath(req));  // .iso, PS3_GAME folder or EBOOT.BIN
        spec.env = dirs.env;
        spec.workingDir = pathToUtf8(utf8ToPath(exe).parent_path());
        // The game logic runs at 60 fps; higher rates on macOS come from frame generation (MoltenVK).
        emu::applyFrameGeneration(spec, exe, req, 60.0, notes);
        LaunchResult r = spawnLogged(def_.id, spec);
        appendNotes(def_.id, notes);
        return r;
    }
};

}  // namespace

std::unique_ptr<Backend> makeRpcs3(const BackendDef& def) { return std::make_unique<Rpcs3>(def); }

}  // namespace rrm
