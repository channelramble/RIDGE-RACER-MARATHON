// MAME (standalone) for Rave Racer (Namco System 22, set "raverace").
//
// Install: Windows uses the official mamedev/mame release package
// (mame<ver>b_x64.exe, a 7-Zip self-extracting archive, checked against GitHub's
// sha256 digest). Upstream publishes no macOS or Linux binaries, so there the
// user links their own MAME (Homebrew, distro package, SDLMAME build).
//
// Marathon rebuilds raverace.zip plus the namcoc71/namcoc74 device sets from the
// user's ROMs (CRC-matched, the same folder the in-app MAME core uses) and starts
// `mame raverace -rompath <that folder>` with every folder MAME writes to
// (ini, cfg, nvram, states, snapshots, plugin data ...) pointed into Marathon's
// data, so the user's own MAME configuration is never read for input mappings
// or written. The working directory is MAME's own folder, so its relative
// defaults (bgfx, plugins, hash, language) still resolve.
//
// Video: MAME's namcos22 driver draws the 3D scene with its software renderer
// straight into the 640x480 screen bitmap (59.906 Hz raster timing); it has no
// internal-resolution option, so MAME can only scale that image. Aspect is the
// cabinet's 4:3.
#include <string>
#include <system_error>
#include <vector>

#include "backends/Adapters.h"
#include "backends/EmuCommon.h"

namespace rrm {

namespace {

using namespace emu;

// MAME's folders, relative to Marathon's data folder for this backend.
struct MameDirs {
    const char* option;
    const char* folder;
};
constexpr MameDirs kDirs[] = {
    {"-cfg_directory", "cfg"},          {"-nvram_directory", "nvram"},       {"-state_directory", "sta"},
    {"-snapshot_directory", "snap"},    {"-input_directory", "inp"},         {"-diff_directory", "diff"},
    {"-comment_directory", "comments"}, {"-homepath", "home"},
};

class MameStandalone : public Backend {
public:
    using Backend::Backend;

    // Windows: the official self-extracting package. libarchive (Windows' tar.exe, macOS'
    // bsdtar 3.7) reads 7-Zip SFX executables directly; older tar.exe builds without LZMA
    // fall back to 7-Zip (7zr.exe) and finally to the package's own silent extraction.
    bool install(Library& lib, TaskProgress& progress) override {
        const BackendSource* src = def_.sourceFor(currentPlatformKey());
        if (!src || src->kind != "github") return Backend::install(lib, progress);
        RunningGuard running(progress);
        progress.setStage("Looking up the latest MAME release");
        Download dl;
        std::string error;
        if (!githubStableAsset(src->repo, src->assetPattern, dl, error)) {
            progress.fail(error);
            return false;
        }
        fs::path dest;
        if (!downloadAndUnpack(def_, progress, dl, "none", dest, error)) {
            progress.fail(error);
            return false;
        }
        const fs::path sfx = dest / utf8ToPath(dl.name);
        progress.setStage("Extracting " + dl.name);
        std::error_code ec;
        bool ok = extractArchive(pathToUtf8(sfx), pathToUtf8(dest), error, &progress);
        if (!ok) {
            progress.addLog("tar could not read the package (" + error + "), trying 7-Zip");
            const fs::path as7z = dest / "package.7z";
            fs::copy_file(sfx, as7z, fs::copy_options::overwrite_existing, ec);
            ok = !ec && extractArchive(pathToUtf8(as7z), pathToUtf8(dest), error, &progress);
            fs::remove(as7z, ec);
        }
#if defined(_WIN32)
        if (!ok) {
            progress.addLog("7-Zip failed (" + error + "), running the package's own extractor");
            ProcessSpec self;
            self.argv = {pathToUtf8(sfx), "-y", "-o" + pathToUtf8(dest)};
            ok = runProcess(self) == 0;
        }
#endif
        fs::remove(sfx, ec);
        const std::string exe = ok ? locateExecutable(dest, src->executable.empty() ? "mame.exe" : src->executable) : "";
        if (exe.empty()) {
            progress.fail(ok ? "The MAME package did not contain mame.exe." : "Could not extract " + dl.name + ": " + error);
            return false;
        }
        recordInstall(lib, def_, dl.version, dest, exe);
        progress.setStage(def_.name + " " + dl.version + " installed");
        return true;
    }

    LaunchResult launch(const LaunchRequest& req) override {
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        if (exe.empty()) return fail(def_.name + " is not installed.");
        if (!req.entry || !req.entry->hasFiles()) return fail("Add your " + req.game->title + " ROMs first.");
        const GameSettings& s = req.settings;

        // <set>.zip and its device sets side by side, named exactly (shared with the in-app MAME core).
        const fs::path setDir = paths().cache / "mame-sets" / utf8ToPath(req.game->id);
        std::string error;
        if (prepareMameSet(req, setDir, error).empty()) return fail(error);
        if (!prepareMameDevices(req, setDir, error)) return fail(error);

        const fs::path data = dataDir(def_.id);
        std::error_code ec;
        std::string iniPath = pathToUtf8(data / "ini");
        fs::create_directories(data / "ini", ec);
#if defined(__linux__)
        // Distro packages keep their path defaults (bgfx, plugins, fonts) in /etc/mame/mame.ini.
        if (fs::exists("/etc/mame/mame.ini", ec)) iniPath += ";/etc/mame";
#endif
        ProcessSpec spec;
        spec.argv = {exe, mameSetName(req), "-rompath", pathToUtf8(setDir), "-inipath", iniPath};
        for (auto& d : kDirs) {
            fs::create_directories(data / d.folder, ec);
            spec.argv.push_back(d.option);
            spec.argv.push_back(pathToUtf8(data / d.folder));
        }
        spec.argv.push_back(s.fullscreen ? "-nowindow" : "-window");
        spec.argv.push_back(s.vsync ? "-waitvsync" : "-nowaitvsync");
        spec.argv.push_back(s.filter == "linear" ? "-filter" : "-nofilter");
        spec.argv.push_back("-skip_gameinfo");
        // Base speed (0.1..100 x realtime). Fast-forward is MAME's own unthrottle key (Insert).
        if (s.speed != 100) {
            spec.argv.push_back("-speed");
            spec.argv.push_back(fmtNumber(s.speed / 100.0));
        }
        // Advanced: settings.extra.mameArgs ["-video", "bgfx", ...] appended as they are.
        if (auto it = s.extra.find("mameArgs"); it != s.extra.end() && it->is_array())
            for (auto& a : *it)
                if (a.is_string()) spec.argv.push_back(a.get<std::string>());
        spec.workingDir = pathToUtf8(programFolder(exe));
        return spawnLogged(def_.id, spec);
    }
};

}  // namespace

std::unique_ptr<Backend> makeMameStandalone(const BackendDef& def) { return std::make_unique<MameStandalone>(def); }

}  // namespace rrm
