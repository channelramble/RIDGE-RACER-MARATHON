// In-process libretro cores: SwanStation and PCSX-ReARMed (PS1), PPSSPP (PSP) and
// MAME (arcade). The same path runs on every OS, so it is the only way to play on
// iOS and the default on Android.
//
// Cores come from the libretro buildbot (desktop, Android) or are bundled into the
// app (iOS: Frameworks/<core>_libretro.framework). The buildbot publishes no
// checksums; cores are only ever loaded from Marathon's own cores folder.
//
// Unified settings become core options. Ultrawide on PS1 uses the cores' widescreen
// hacks (a wider 3D field of view; 2D HUD elements are stretched), and the player
// then follows the aspect ratio the core reports. On PSP the European discs'
// widescreen code (PspCommon.h) widens the projection and the player presents at
// that ratio.
//
// Controller settings become Marathon's rrm_* input options and the cores' own input
// options (LibretroControls.cpp): e.g. the PS1 racers get an analog neGcon.
#include <cmath>
#include <utility>

#include <SDL3/SDL.h>

#include "backends/Adapters.h"
#include "backends/EmuCommon.h"
#include "backends/LibretroControls.h"
#include "backends/PspCommon.h"

namespace rrm {

namespace {

using namespace emu;

constexpr const char* kBuildbot = "https://buildbot.libretro.com/nightly/";

struct CoreFile {
    std::string fileName;  // as loaded from paths().cores (or the iOS bundle)
    std::string url;       // buildbot zip ("" = bundled)
};

CoreFile coreFileFor(const BackendDef& def) {
    CoreFile f;
    const std::string& core = def.core;
    switch (currentOs()) {
        case Os::MacOS:
            f.fileName = core + "_libretro.dylib";
            f.url = std::string(kBuildbot) + "apple/osx/" + cpuArch() + "/latest/" + f.fileName + ".zip";
            break;
        case Os::Windows:
            f.fileName = core + "_libretro.dll";
            f.url = std::string(kBuildbot) + "windows/x86_64/latest/" + f.fileName + ".zip";
            break;
        case Os::Linux:
            f.fileName = core + "_libretro.so";
            f.url = std::string(kBuildbot) + "linux/" + cpuArch() + "/latest/" + f.fileName + ".zip";
            break;
        case Os::Android:
            f.fileName = core + "_libretro_android.so";
            f.url = std::string(kBuildbot) + "android/latest/arm64-v8a/" + f.fileName + ".zip";
            break;
        case Os::iOS:
            f.fileName = core + "_libretro";
            break;
    }
    // A catalog source of kind "url" pins a specific build instead of the nightly.
    if (const BackendSource* src = def.sourceFor(currentPlatformKey()); src && src->kind == "url" && !src->url.empty())
        f.url = src->url;
    return f;
}

// iOS: cores are embedded frameworks inside the app bundle.
std::string bundledCore(const BackendDef& def) {
    if (currentOs() != Os::iOS) return {};
    const char* base = SDL_GetBasePath();
    if (!base) return {};
    fs::path p = utf8ToPath(base) / "Frameworks" / utf8ToPath(def.core + "_libretro.framework") /
                 utf8ToPath(def.core + "_libretro");
    std::error_code ec;
    return fs::exists(p, ec) ? pathToUtf8(p) : std::string();
}

// Closest n:d with both terms in 1..50 (SwanStation's custom aspect range).
std::pair<int, int> aspectFraction(double ratio) {
    int bestN = 16, bestD = 9;
    double bestErr = 1e9;
    for (int d = 1; d <= 50; ++d) {
        const int n = static_cast<int>(std::lround(ratio * d));
        if (n < 1 || n > 50) continue;
        const double err = std::fabs(static_cast<double>(n) / d - ratio);
        if (err + 1e-9 < bestErr) {
            bestErr = err;
            bestN = n;
            bestD = d;
        }
    }
    return {bestN, bestD};
}

// Wider than 4:3 asked for (fixed ratio or the screen's shape)? 0 = no.
double wantedWidescreen(const LaunchRequest& req) {
    const double r = aspectRatio(req.settings.aspect, req.display.width, req.display.height);
    return r > 4.0 / 3.0 + 0.01 ? r : 0.0;
}

void swanstationOptions(const LaunchRequest& req, json& o) {
    const GameSettings& s = req.settings;
    const int scale = renderScale(req, 240, 16);
    o["swanstation_GPU_Renderer"] = "Auto";
    o["swanstation_GPU_ResolutionScale"] = std::to_string(scale);
    o["swanstation_GPU_TrueColor"] = boolStr(scale > 1);  // undithered 24-bit colour when enhancing
    o["swanstation_GPU_TextureFilter"] = s.filter == "linear" ? "Bilinear" : s.filter == "xbr" ? "xBR" : "Nearest";
    o["swanstation_GPU_MSAA"] = s.antiAlias == "msaa" ? "4" : s.antiAlias == "ssaa" ? "4-ssaa" : "1";
    o["swanstation_GPU_PGXPEnable"] = boolStr(s.precision);
    o["swanstation_GPU_PGXPCulling"] = "true";
    o["swanstation_GPU_PGXPTextureCorrection"] = "true";
    o["swanstation_BIOS_PatchFastBoot"] = boolStr(s.fastBoot);
    if (double ratio = wantedWidescreen(req); ratio > 0) {
        o["swanstation_GPU_WidescreenHack"] = "true";
        auto [n, d] = aspectFraction(ratio);
        if (n == 16 && d == 9) {
            o["swanstation_Display_AspectRatio"] = "16:9";
        } else {
            o["swanstation_Display_AspectRatio"] = "Custom";
            o["swanstation_Display_CustomAspectRatioNumerator"] = std::to_string(n);
            o["swanstation_Display_CustomAspectRatioDenominator"] = std::to_string(d);
        }
    } else {
        o["swanstation_GPU_WidescreenHack"] = "false";
        o["swanstation_Display_AspectRatio"] = "4:3";
    }
}

void pcsxRearmedOptions(const LaunchRequest& req, json& o) {
    // The NEON enhancement (2x) only exists in the ARM builds; other builds ignore the key.
    o["pcsx_rearmed_neon_enhancement_enable"] = boolStr(renderScale(req, 240, 2) >= 2, "enabled", "disabled");
    o["pcsx_rearmed_show_bios_bootlogo"] = req.settings.fastBoot ? "disabled" : "enabled";
}

// The core's memory stick is the save folder: cheats in PSP/Cheats/<DISC_ID>.ini and texture
// packs in PSP/TEXTURES/<DISC_ID>, both read when the game loads.
void ppssppOptions(const LaunchRequest& req, const fs::path& memstick, json& o, LaunchResult& r) {
    const int k = renderScale(req, 272, 10);
    o["ppsspp_internal_resolution"] = std::to_string(480 * k) + "x" + std::to_string(272 * k);
    o["ppsspp_backend"] = "auto";

    const std::string discId = psp::discIdFor(req);
    if (discId.empty()) return;  // no widescreen code or texture pack: missingRequirements() warns
    const double wanted = aspectRatio(req.settings.aspect, req.display.width, req.display.height);
    const psp::WideCheat* cheat = wanted > psp::kNativeAspect + 0.03 ? psp::wideCheatFor(discId) : nullptr;
    const double ratio = std::min(wanted, 4.0);
    const fs::path cheats = memstick / "PSP" / "Cheats" / utf8ToPath(discId + ".ini");
    if (!dryRun()) psp::writeCheatFile(cheats, discId, cheat, ratio);  // also removes Marathon's code when off
    if (cheat) {
        o["ppsspp_cheats"] = "enabled";
        o["ppsspp_cropto16x9"] = "disabled";  // the code sets the exact shape
        r.presentAspect = std::to_string(ratio);
    }

    const fs::path textures = memstick / "PSP" / "TEXTURES" / utf8ToPath(discId);
    const std::string pack = jget<std::string>(req.settings.extra, "texturePack", "");
    std::error_code ec;
    if (pack.empty()) {
        if (!dryRun() && psp::ownedPackDir(textures)) fs::remove_all(textures, ec);
        return;
    }
    psp::PackSource source = psp::findPack(pack, discId);
    std::string error;
    if (source.error.empty() && (dryRun() || psp::installPack(source, textures, error)))
        o["ppsspp_texture_replacement"] = "enabled";
    if (!error.empty() || !source.error.empty())
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[ppsspp] texture pack: %s",
                    (source.error.empty() ? error : source.error).c_str());
}

// PS1 BIOS: optional for both PS1 cores (SwanStation falls back to its built-in
// OpenBIOS, PCSX-ReARMed to HLE). A user-supplied BIOS is copied into the core
// system folder; SwanStation is pointed at it when the name is one it accepts.
void providePs1Bios(const LaunchRequest& req, const fs::path& systemDir, json& o, const std::string& core) {
    if (!req.library) return;
    const std::string bios = req.library->systemFile("ps1-bios");
    if (bios.empty()) return;
    fs::path src = utf8ToPath(bios);
    std::string name = lower(pathToUtf8(src.filename()));
    const bool known = name == "scph5500.bin" || name == "scph5501.bin" || name == "scph5502.bin" ||
                       name == "psxonpsp660.bin" || name == "ps1_rom.bin";
    if (!known) name = "ps1_rom.bin";
    std::string error;
    if (!copyIfChanged(src, systemDir / utf8ToPath(name), error)) return;
    if (core == "swanstation") {
        o["swanstation_BIOS_PathNTSCU"] = name;
        o["swanstation_BIOS_PathNTSCJ"] = name;
        o["swanstation_BIOS_PathPAL"] = name;
    }
}

class Libretro : public Backend {
public:
    using Backend::Backend;

    InstallState installState(const Library& lib) const override {
        if (currentOs() == Os::iOS) return bundledCore(def_).empty() ? InstallState::Unavailable : InstallState::Installed;
        return Backend::installState(lib);
    }

    bool install(Library& lib, TaskProgress& progress) override {
        RunningGuard running(progress);
        const CoreFile cf = coreFileFor(def_);
        if (cf.url.empty()) {
            progress.fail(def_.name + " is built into Marathon on this device.");
            return false;
        }
        Download dl;
        dl.url = cf.url;
        dl.name = cf.fileName + ".zip";
        dl.version = "nightly-" + today();
        fs::path unpacked;
        std::string error;
        if (!downloadAndUnpack(def_, progress, dl, "zip", unpacked, error)) {
            progress.fail(error);
            return false;
        }
        const std::string found = findInTree(pathToUtf8(unpacked), cf.fileName, 2);
        if (found.empty()) {
            progress.fail("The download did not contain " + cf.fileName + ".");
            return false;
        }
        // Cores live in one flat folder so they never depend on a versioned install directory.
        std::error_code ec;
        fs::create_directories(paths().cores, ec);
        const fs::path dest = paths().cores / utf8ToPath(cf.fileName);
        fs::copy_file(utf8ToPath(found), dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            progress.fail("Could not install " + cf.fileName + ": " + ec.message());
            return false;
        }
        fs::remove_all(unpacked, ec);
        recordInstall(lib, def_, dl.version, paths().cores, pathToUtf8(dest));
        progress.setStage(def_.name + " installed");
        return true;
    }

    std::vector<std::string> missingRequirements(const LaunchRequest& req) const override {
        std::vector<std::string> out = Backend::missingRequirements(req);
        if (def_.core != "ppsspp" || !req.entry || !req.entry->hasFiles()) return out;
        const std::string discId = psp::discIdFor(req);  // cached: the UI asks every frame
        const double wanted = aspectRatio(req.settings.aspect, req.display.width, req.display.height);
        if (wanted > psp::kNativeAspect + 0.03 && !psp::wideCheatFor(discId)) {
            if (discId.empty())
                out.push_back(std::string(kWarningPrefix) +
                              "The disc id of this image (.chd/.pbp) is unknown, so the widescreen code is off and it "
                              "plays at the PSP's own 16:9. Use an .iso/.cso, or set the advanced option discId (e.g. "
                              "UCES00422).");
            else
                out.push_back(std::string(kWarningPrefix) +
                              "Ultrawide needs the European disc (UCES-00422 Ridge Racer 2, UCES-00002 Ridge Racer); this "
                              "disc is " + discId + ", so it plays at the PSP's own 16:9.");
        }
        if (const std::string pack = jget<std::string>(req.settings.extra, "texturePack", ""); !pack.empty()) {
            if (discId.empty())
                out.push_back(std::string(kWarningPrefix) + "Texture pack skipped: the disc id of this image is unknown.");
            else if (std::string problem = psp::packProblem(pack, discId); !problem.empty())  // zips: unpacked at launch
                out.push_back(kWarningPrefix + problem);
        }
        return out;
    }

    ControlsInfo controls(const LaunchRequest& req) const override {
        return libretroControls(def_.core, req.game ? req.game->id : std::string(), req.controllers);
    }

    bool uninstall(Library& lib) override {
        if (const BackendInstall* bi = lib.install(def_.id); bi && !bi->userProvided && !bi->executable.empty()) {
            std::error_code ec;
            fs::path core = utf8ToPath(bi->executable);
            if (core.parent_path() == paths().cores) fs::remove(core, ec);
        }
        return Backend::uninstall(lib);
    }

    LaunchResult launch(const LaunchRequest& req) override {
        LaunchResult r;
        r.mode = LaunchResult::Mode::InProcessCore;
        r.corePath = currentOs() == Os::iOS ? bundledCore(def_) : (req.library ? installedExecutable(*req.library) : "");
        if (r.corePath.empty()) return fail(def_.name + " is not installed.");
        if (!req.entry || !req.entry->hasFiles()) return fail("Add your " + req.game->title + " files first.");

        const fs::path systemDir = paths().system / "libretro";
        const fs::path saveDir = paths().saves / utf8ToPath(req.game->id);
        std::error_code ec;
        fs::create_directories(systemDir, ec);
        fs::create_directories(saveDir, ec);
        r.systemDir = pathToUtf8(systemDir);
        r.saveDir = pathToUtf8(saveDir);
        r.contentPath = gamePath(req);
        r.presentAspect = "auto";  // every core below reports its real display aspect

        json o = json::object();
        const std::string& core = def_.core;
        if (core == "swanstation") {
            swanstationOptions(req, o);
            providePs1Bios(req, systemDir, o, core);
        } else if (core == "pcsx_rearmed") {
            pcsxRearmedOptions(req, o);
            providePs1Bios(req, systemDir, o, core);
        } else if (core == "ppsspp") {
            ppssppOptions(req, saveDir, o, r);
        } else if (core == "mame") {
            // MAME wants <set>.zip and its device sets side by side, named exactly.
            const fs::path setDir = paths().cache / "mame-sets" / utf8ToPath(req.game->id);
            std::string error;
            r.contentPath = prepareMameSet(req, setDir, error);
            if (r.contentPath.empty()) return fail(error);
            if (!prepareMameDevices(req, setDir, error)) return fail(error);
        }
        const json pad = libretroControllerOptions(core, req.game->id, req.controllers);
        for (auto& [k, v] : pad.items()) o[k] = v;
        // Advanced: settings.extra.coreOptions {"key": "value"} wins over Marathon's mapping.
        if (auto it = req.settings.extra.find("coreOptions"); it != req.settings.extra.end() && it->is_object())
            for (auto& [k, v] : it->items())
                if (v.is_string()) o[k] = v;
        r.coreOptions = o;
        r.ok = true;
        return r;
    }
};

}  // namespace

std::unique_ptr<Backend> makeLibretro(const BackendDef& def) { return std::make_unique<Libretro>(def); }

}  // namespace rrm
