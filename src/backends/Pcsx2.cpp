// PCSX2 (desktop): Ridge Racer V (PS2) through the official PCSX2 builds.
//
// Data folder: PCSX2 always starts with `-datapath <Marathon config>/pcsx2`, so its
// PCSX2.ini, memory cards, BIOS copy, patches and per-game settings live in
// <datapath>/PCSX2/ and the user's own PCSX2 setup is never read or written.
// (portable.ini / portable.txt beside the program wins over -datapath in PCSX2; a
// linked install in portable mode is reported as a warning.) The first start runs
// `-testconfig` once so PCSX2 writes its complete defaults (controller bindings,
// hotkeys, folders); Marathon then edits only the keys it owns, on every launch.
//
// Widescreen: 16:9 turns on the per-game "Widescreen 16:9" patches bundled in
// PCSX2's patches.zip ([EmuCore] EnableWideScreenPatches). No wider patch exists for
// any Ridge Racer game, so for Ridge Racer V (USA) Marathon derives one from No.47's
// 16:9 patch (kRrvUsa below) and writes it as a Marathon-owned pnach in
// <datapath>/PCSX2/patches, enabled through the per-game settings file.
// Frame rate: Ridge Racer V renders 60 fps and PCSX2 cannot add frames, so high
// refresh displays get VSync/VRR presentation only.
//
// macOS: the official macOS build is x86_64 only and runs through Rosetta 2.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>

#include "backends/Adapters.h"
#include "backends/EmuCommon.h"
#include "core/ConfigFiles.h"

namespace rrm {

namespace {

using namespace emu;

// ---- derived ultrawide patches ----------------------------------------------------------------

// One FOV constant of a 16:9 pnach, given by its value at 4:3. Projection distances and
// horizontal factors shrink with the picture width (16:9 = 4:3 value x 0.75) and vertical
// factors grow by the same amount (16:9 = 4:3 value / 0.75): the vertical view stays as on
// the PS2 and the picture gains width (Hor+). With k = (4/3) / aspect the same rule gives
// any wider shape, and k = 0.75 reproduces the published 16:9 values.
struct FovWord {
    const char* address;  // pnach address field (8 hex digits, leading "2" = 32-bit write)
    uint32_t bits;        // IEEE-754 single at 4:3
    bool vertical;        // value / k instead of value x k
    const char* what;
};

struct WidescreenDef {
    const char* serial;      // PCSX2 serial, "SLUS-20002"
    const char* bootSerial;  // SYSTEM.CNF form the scanner records, "SLUS_200.02"
    const char* crc;         // ELF CRC as PCSX2 names pnach / game settings files
    const char* title;
    std::vector<FovWord> words;
};

// Ridge Racer V (USA), from PCSX2/pcsx2_patches patches/SLUS-20002_06AD9CA0.pnach
// [Widescreen 16:9] by No.47. Its seven words are 530.0 -> 397.5, 530.0 -> 300.0 (chase
// camera, see below), 0.46 -> 0.6133 (x3), 397.5 -> 298.125 and 0.8 -> 0.6: every one is
// the original x 0.75 or / 0.75.
const WidescreenDef kRrvUsa{
    "SLUS-20002",
    "SLUS_200.02",
    "06AD9CA0",
    "Ridge Racer V",
    {
        {"20332694", 0x44048000u, false, "single player FOV, bumper camera (530.0)"},
        // No.47 widens the chase camera further ("shows the whole car"): 300.0 at 16:9, which
        // is 400.0 on the 4:3 scale. Marathon keeps that framing at every width.
        {"203326B4", 0x43C80000u, false, "single player FOV, chase camera (400.0 = No.47's improved chase cam)"},
        {"20332EC4", 0x3EEB851Fu, true, "single player vertical FOV (0.46)"},
        {"20332690", 0x43C6C000u, false, "split screen FOV (397.5)"},
        {"20332ED0", 0x3EEB851Fu, true, "split screen vertical FOV, top (0.46)"},
        {"20332ED4", 0x3EEB851Fu, true, "split screen vertical FOV, bottom (0.46)"},
        {"20332F80", 0x3F4CCCCDu, false, "menu horizontal FOV (0.8)"},
    }};

float floatFromBits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

std::string floatHex(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", u);
    return buf;
}

// The game's entry in the table above, when the user's disc is that release.
const WidescreenDef* derivedWidescreenFor(const LaunchRequest& req) {
    if (!req.game || req.game->serial != kRrvUsa.serial) return nullptr;
    // The scanner records "boot <SYSTEM.CNF serial>"; the PAL/JP discs share the game id.
    const std::string by = req.entry ? req.entry->files.detectedBy : std::string();
    if (by.rfind("boot ", 0) == 0 && by.find(kRrvUsa.bootSerial) == std::string::npos) return nullptr;
    return &kRrvUsa;
}

// "21:9" / "32:9" as chosen; "fit" = the display reduced (3440x1440 -> 43:18).
std::string ratioLabel(const LaunchRequest& req) {
    if (req.settings.aspect != "fit") return req.settings.aspect;
    const int w = std::max(1, req.display.width), h = std::max(1, req.display.height);
    const int g = std::gcd(w, h);
    return std::to_string(w / g) + ":" + std::to_string(h / g);
}

std::string derivedPnach(const WidescreenDef& def, const std::string& group, const std::string& ratio, double aspect) {
    const double k = (4.0 / 3.0) / aspect;
    std::string out;
    out += "gametitle=" + std::string(def.title) + " (" + def.serial + "), written by Ridge Racer Marathon\n\n";
    out += "[" + group + "]\n";
    out += "author=Ridge Racer Marathon, derived from No.47's Widescreen 16:9 patch\n";
    out += "description=Hor+ field of view for " + ratio + ": the 16:9 patch's constants rescaled with k = (4/3) / " +
           fmtNumber(aspect) + " = " + fmtNumber(k) + ". 2D HUD elements are stretched to the width.\n";
    out += "gsaspectratio=" + ratio + "\n";
    for (const FovWord& w : def.words) {
        const double base = floatFromBits(w.bits);
        const float value = static_cast<float>(w.vertical ? base / k : base * k);
        out += "patch=1,EE," + std::string(w.address) + ",word," + floatHex(value) + " // " + w.what +
               (w.vertical ? " / k = " : " x k = ") + fmtNumber(value) + "\n";
    }
    return out;
}

// ---- data folder ----------------------------------------------------------------------------

// Copies the user's BIOS (and its .rom1/.rom2/.erom companions) into Marathon's data root:
// PCSX2 writes the console's NVM/MEC files next to the BIOS, which must never happen in the
// user's own folder. Returns the BIOS file name for [Filenames] BIOS ("" = PCSX2 picks one).
std::string provideBios(const LaunchRequest& req, const fs::path& biosDir, std::vector<std::string>& notes) {
    if (!req.library) return {};
    const std::string configured = req.library->systemFile("ps2-bios");
    if (configured.empty()) return {};
    std::error_code ec;
    fs::create_directories(biosDir, ec);
    const fs::path src = utf8ToPath(configured);
    std::string error;
    if (fs::is_directory(src, ec)) {
        // A folder of dumps: copy what can be a PS2 BIOS set and let PCSX2 choose.
        for (auto& e : fs::directory_iterator(src, ec)) {
            if (!e.is_regular_file(ec) || e.file_size(ec) > 16u * 1024 * 1024) continue;
            if (!copyIfChanged(e.path(), biosDir / e.path().filename(), error)) notes.push_back(error);
        }
        return {};
    }
    if (!copyIfChanged(src, biosDir / src.filename(), error)) {
        notes.push_back(error);
        return {};
    }
    const std::string stem = lower(pathToUtf8(src.stem()));
    for (auto& e : fs::directory_iterator(src.parent_path(), ec)) {
        if (!e.is_regular_file(ec) || e.path() == src) continue;
        const std::string name = lower(pathToUtf8(e.path().filename()));
        if (name.rfind(stem + ".", 0) != 0) continue;
        const fs::path dst = biosDir / e.path().filename();
        // NVM/MEC hold console state PCSX2 keeps updating: take the user's copy once only.
        if ((endsWithI(name, ".nvm") || endsWithI(name, ".mec")) && fs::exists(dst, ec)) continue;
        if (!copyIfChanged(e.path(), dst, error)) notes.push_back(error);
    }
    return pathToUtf8(src.filename());
}

// PCSX2.ini must exist with [UI] SettingsVersion before Marathon edits it; otherwise PCSX2
// resets it to bare defaults or asks the user what to do. `-testconfig` makes PCSX2 write its
// own complete defaults and exit.
bool ensureBaseConfig(const std::string& exe, const fs::path& root, const fs::path& ini, std::vector<std::string>& notes,
                      std::string& error) {
    std::error_code ec;
    cfg::Ini probe;
    if (fs::exists(ini, ec) && probe.load(pathToUtf8(ini)) && !probe.get("UI", "SettingsVersion").empty()) return true;
    if (dryRun()) {
        notes.push_back("first start: runs " + exe + " -testconfig -datapath " + pathToUtf8(root) +
                        " so PCSX2 writes its complete defaults before Marathon's keys are applied");
        return true;
    }
    // Only Marathon writes here; an incomplete file would make PCSX2 offer to reset it.
    if (fs::exists(ini, ec)) {
        const fs::path old = utf8ToPath(pathToUtf8(ini) + ".incomplete");
        fs::remove(old, ec);
        fs::rename(ini, old, ec);
        if (ec) fs::remove(ini, ec);
    }
    ProcessSpec spec;
    spec.argv = {exe, "-testconfig", "-datapath", pathToUtf8(root)};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    std::string output;
    const int rc = runProcess(spec, &output, [&] { return std::chrono::steady_clock::now() < deadline; });
    if (fs::exists(ini, ec)) return true;
    error = "PCSX2 could not create its settings (" + exe + " -testconfig exited with " + std::to_string(rc) + ").";
    if (!output.empty()) error += "\n" + output.substr(0, 600);
    return false;
}

// settings.extra.pcsx2Ini {"Section": {"Key": value}}: advanced overrides, applied last.
void applyExtraIni(const json& extra, cfg::Ini& ini) {
    auto it = extra.find("pcsx2Ini");
    if (it == extra.end() || !it->is_object()) return;
    for (auto& [section, keys] : it->items()) {
        if (!keys.is_object()) continue;
        for (auto& [key, v] : keys.items()) {
            if (v.is_string()) ini.set(section, key, v.get<std::string>());
            else if (v.is_boolean()) ini.set(section, key, boolStr(v.get<bool>()));
            else if (v.is_number()) ini.set(section, key, fmtNumber(v.get<double>()));
        }
    }
}

// portable.ini / portable.txt beside a linked PCSX2 overrides -datapath.
bool linkedPortable(const std::string& exe) {
    std::error_code ec;
    const fs::path dir = programFolder(exe);
    return fs::exists(dir / "portable.ini", ec) || fs::exists(dir / "portable.txt", ec);
}

void appendNotes(const std::string& backendId, const std::vector<std::string>& notes) {
    if (notes.empty()) return;
    std::ofstream out(paths().logs / utf8ToPath(backendId + ".log"), std::ios::binary | std::ios::app);
    for (auto& n : notes) out << "  [marathon] " << n << "\n";
}

class Pcsx2 : public Backend {
public:
    using Backend::Backend;

    // PCSX2 publishes a nightly pre-release almost every day, so the newest 30 releases the
    // generic installer scans usually hold no stable build: resolve /releases/latest instead.
    bool install(Library& lib, TaskProgress& progress) override {
        const BackendSource* src = def_.sourceFor(currentPlatformKey());
        if (!src || src->kind != "github" || jget(def_.params, "prerelease", false)) return Backend::install(lib, progress);
        RunningGuard running(progress);
        progress.setStage("Looking up the latest " + def_.name + " release");
        Download dl;
        std::string error;
        if (!githubStableAsset(src->repo, src->assetPattern, dl, error)) {
            progress.fail(error);
            return false;
        }
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
        return true;
    }

    std::vector<std::string> missingRequirements(const LaunchRequest& req) const override {
        std::vector<std::string> out = Backend::missingRequirements(req);
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        if (exe.empty()) return out;
        if (currentOs() == Os::MacOS && std::string(cpuArch()) == "arm64" && machoIsIntelOnly(exe) && !rosettaInstalled())
            out.push_back("PCSX2's macOS build is Intel-only and needs Rosetta 2. Install it once from Terminal with "
                          "\"softwareupdate --install-rosetta --agree-to-license\", then start the game again.");
        const BackendInstall* bi = req.library->install(def_.id);
        if (bi && bi->userProvided && linkedPortable(exe))
            out.push_back(std::string(kWarningPrefix) +
                          "your linked PCSX2 is in portable mode (portable.ini/portable.txt beside it), which makes it "
                          "ignore Marathon's -datapath: it will use its own settings, not Marathon's.");
        return out;
    }

    LaunchResult launch(const LaunchRequest& req) override {
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        if (exe.empty()) return fail(def_.name + " is not installed.");
        if (!req.entry || !req.entry->hasFiles()) return fail("Add your " + req.game->title + " disc image first.");
        const GameSettings& s = req.settings;
        std::vector<std::string> notes;
        std::string error;

        // -datapath <root> makes PCSX2 use <root>/PCSX2 as its data root.
        const fs::path root = dataDir(def_.id);
        const fs::path data = root / "PCSX2";
        const fs::path iniPath = data / "inis" / "PCSX2.ini";
        if (!ensureBaseConfig(exe, root, iniPath, notes, error)) return fail(error);

        cfg::Ini ini;
        ini.load(pathToUtf8(iniPath));
        ini.set("UI", "SetupWizardIncomplete", "false");
        ini.set("UI", "ConfirmShutdown", "false");
        ini.set("UI", "StartFullscreen", boolStr(s.fullscreen));
        ini.set("AutoUpdater", "CheckAtStartup", "false");

        // BIOS: Marathon's own copy (PCSX2 writes NVM/MEC files beside it).
        ini.set("Folders", "Bios", "bios");
        const std::string biosName = provideBios(req, data / "bios", notes);
        if (!biosName.empty()) ini.set("Filenames", "BIOS", biosName);

        // Resolution: integer multiples of the 448-line PS2 frame (1080p = 3x, 4K = 5x, 8K = 10x).
        const int scale = renderScale(req, 448, 12);
        ini.set("EmuCore/GS", "upscale_multiplier", std::to_string(scale));
        ini.set("EmuCore/GS", "VsyncEnable", boolStr(s.vsync));
        ini.set("EmuCore/GS", "fxaa", boolStr(s.antiAlias == "fxaa"));
        // Texture filtering: PS2 games choose per texture. "Sharp" keeps the console's own choice
        // (Bilinear (PS2) = 2), "Smooth" forces bilinear everywhere (1). PCSX2's forced nearest (0)
        // is never what a PS2 game drew, so Marathon does not use it.
        ini.set("EmuCore/GS", "filter", s.filter == "nearest" ? "2" : "1");

        // Speed: base speed and fast-forward speed (0 = unlimited). Tab toggles fast-forward.
        ini.set("Framerate", "NominalScalar", fmtNumber(s.speed / 100.0));
        ini.set("Framerate", "TurboScalar", fmtNumber(s.turbo / 100.0));
        ini.set("EmuCore", "EnableFastBoot", boolStr(s.fastBoot));
        ini.set("EmuCore", "EnablePatches", "true");
        // Progressive frames (PCSX2's curated No-Interlacing patches) once the image is upscaled.
        ini.set("EmuCore", "EnableNoInterlacingPatches", boolStr(scale > 1));

        // Aspect ratio. "Auto 4:3/3:2" lets an enabled widescreen patch set the shape through
        // gsaspectratio; FMVs stay 4:3 while a widescreen patch is active.
        const double wanted = aspectRatio(s.aspect, req.display.width, req.display.height);
        const bool wide = wanted > 4.0 / 3.0 + 0.01;
        const bool is169 = std::fabs(wanted - 16.0 / 9.0) < 0.01;
        const WidescreenDef* derived = wide && !is169 ? derivedWidescreenFor(req) : nullptr;
        const std::string derivedGroup = derived ? "Marathon Widescreen " + ratioLabel(req) : std::string();
        ini.set("EmuCore/GS", "AspectRatio", s.aspect == "4:3" ? "4:3" : "Auto 4:3/3:2");
        ini.set("EmuCore/GS", "FMVAspectRatioSwitch", wide ? "4:3" : "Off");
        ini.set("EmuCore", "EnableWideScreenPatches", boolStr(wide && !derived));
        if (wide && !is169 && !derived)
            notes.push_back("no " + ratioLabel(req) + " patch for this disc: PCSX2's 16:9 widescreen patch is used");
        applyExtraIni(s.extra, ini);
        if (!ini.save(pathToUtf8(iniPath))) return fail("Could not write " + pathToUtf8(iniPath));

        // The derived pnach and the per-game settings that enable it ([Patches] Enable).
        const std::string id = std::string(kRrvUsa.serial) + "_" + kRrvUsa.crc;
        const fs::path patchFile = data / "patches" / utf8ToPath(id + "_marathon.pnach");
        const fs::path gameIni = data / "gamesettings" / utf8ToPath(id + ".ini");
        std::error_code ec;
        if (derived) {
            if (!writeText(patchFile, derivedPnach(*derived, derivedGroup, ratioLabel(req), wanted)))
                return fail("Could not write " + pathToUtf8(patchFile));
            cfg::Ini game;
            game.load(pathToUtf8(gameIni));
            game.set("Patches", "Enable", derivedGroup);
            if (!game.save(pathToUtf8(gameIni))) return fail("Could not write " + pathToUtf8(gameIni));
            notes.push_back("ultrawide: [" + derivedGroup + "] in " + pathToUtf8(patchFile));
        } else if (req.game && req.game->serial == kRrvUsa.serial) {
            // Nothing Marathon-made stays enabled once it is no longer wanted.
            fs::remove(patchFile, ec);
            if (fs::exists(gameIni, ec)) {
                cfg::Ini game;
                game.load(pathToUtf8(gameIni));
                game.set("Patches", "Enable", "");
                game.save(pathToUtf8(gameIni));
            }
        }
        if (resolveFps(s) != 0) notes.push_back(req.game->title + " keeps its own 60 fps under PCSX2 (no frame generation)");

        ProcessSpec spec;
        spec.argv = {exe,           "-batch", "-nogui", s.fullscreen ? "-fullscreen" : "-nofullscreen",
                     s.fastBoot ? "-fastboot" : "-slowboot", "-datapath", pathToUtf8(root), "--", gamePath(req)};
        spec.workingDir = pathToUtf8(programFolder(exe));
        LaunchResult r = spawnLogged(def_.id, spec);
        appendNotes(def_.id, notes);
        return r;
    }
};

}  // namespace

std::unique_ptr<Backend> makePcsx2(const BackendDef& def) { return std::make_unique<Pcsx2>(def); }

}  // namespace rrm
