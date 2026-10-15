// Shared helpers for the emulator backends (libretro cores, PCSX2, RPCS3,
// Xenia, PPSSPP, MAME, Android hand-off). Header-only; included by those
// backend .cpp files only.
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>

#include "backends/Backend.h"
#include "core/Net.h"
#include "core/Platform.h"
#include "core/Process.h"
#include "core/RomSet.h"

namespace rrm::emu {

// Test hook (tests/launch_tool --dry-run): launch() writes configs and fills
// LaunchResult::commandLine but does not start anything.
inline bool& dryRun() {
    static bool flag = false;
    return flag;
}

// Entries of missingRequirements() starting with this prefix are advisory, not blockers.
inline const char* kWarningPrefix = "Warning: ";
inline bool isWarning(const std::string& s) { return isRequirementWarning(s); }

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool endsWithI(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return lower(s.substr(s.size() - suffix.size())) == lower(suffix);
}

// "1.5", "2", "0.25" (no locale, no trailing zeros).
inline std::string fmtNumber(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

inline std::string boolStr(bool b, const char* yes = "true", const char* no = "false") { return b ? yes : no; }

// Native output height of the game: option params, then backend params, then the fallback.
inline int nativeHeight(const LaunchRequest& req, int fallback) {
    if (req.option) {
        int h = jget(req.option->params, "nativeHeight", 0);
        if (h > 0) return h;
    }
    if (req.backend) {
        int h = jget(req.backend->params, "nativeHeight", 0);
        if (h > 0) return h;
    }
    return fallback;
}

// Integer render scale for the unified "resolution" setting.
inline int renderScale(const LaunchRequest& req, int nativeFallback, int maxScale) {
    return resolveScale(req.settings.resolution, nativeHeight(req, nativeFallback), req.display.height, maxScale);
}

// Smallest supported step >= wanted (steps ascending); the largest step if none.
inline int pickStep(int wanted, const std::vector<int>& steps) {
    for (int s : steps)
        if (s >= wanted) return s;
    return steps.empty() ? wanted : steps.back();
}

inline std::string gamePath(const LaunchRequest& req) { return req.entry ? req.entry->files.path : std::string(); }

// Marathon-owned, persistent data folder for a backend (survives backend updates).
inline fs::path dataDir(const std::string& backendId) {
    fs::path p = paths().config / utf8ToPath(backendId);
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

// Copies src over dst unless dst already has the same size and is not older.
inline bool copyIfChanged(const fs::path& src, const fs::path& dst, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) {
        error = "Not found: " + pathToUtf8(src);
        return false;
    }
    if (fs::exists(dst, ec) && fs::file_size(dst, ec) == fs::file_size(src, ec) &&
        fs::last_write_time(dst, ec) >= fs::last_write_time(src, ec))
        return true;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "Could not copy " + pathToUtf8(src) + " to " + pathToUtf8(dst) + ": " + ec.message();
        return false;
    }
    return true;
}

inline bool writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

inline std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline std::string timestamp() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

inline std::string today() {
    std::time_t t = std::time(nullptr);
    char buf[16];
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
    return buf;
}

// Starts an external backend with its output appended to logs/<id>.log.
inline LaunchResult spawnLogged(const std::string& backendId, ProcessSpec spec) {
    LaunchResult r;
    r.mode = LaunchResult::Mode::ExternalProcess;
    r.commandLine = spec.argv;
    fs::path log = paths().logs / utf8ToPath(backendId + ".log");
    spec.logFile = pathToUtf8(log);
    {
        std::ofstream out(log, std::ios::binary | std::ios::app);
        if (out) {
            out << "\n==== " << timestamp() << " Marathon launch ====\n";
            for (auto& a : spec.argv) out << "  " << a << "\n";
            for (auto& [k, v] : spec.env) out << "  env " << k << "=" << v << "\n";
        }
    }
    if (dryRun()) {
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

inline LaunchResult fail(const std::string& message) {
    LaunchResult r;
    r.ok = false;
    r.error = message;
    return r;
}

// ---- macOS: frame generation for external backends ----------------------------------------------

// Marathon's present library (tools/framegrab -> librrm_present.dylib): the app bundle's Frameworks
// folder, else beside the running executable (developer builds); RRM_PRESENT_LIBRARY overrides.
inline std::string presentLibraryPath() {
    if (const char* env = std::getenv("RRM_PRESENT_LIBRARY"); env && *env) return env;
    std::error_code ec;
    std::vector<fs::path> candidates;
    if (!paths().resources.empty()) candidates.push_back(paths().resources / ".." / "Frameworks" / "librrm_present.dylib");
    if (const char* base = SDL_GetBasePath()) {
        candidates.push_back(utf8ToPath(base) / ".." / "Frameworks" / "librrm_present.dylib");
        candidates.push_back(utf8ToPath(base) / "librrm_present.dylib");
    }
    for (auto& c : candidates)
        if (fs::is_regular_file(c, ec)) return pathToUtf8(fs::weakly_canonical(c, ec));
    return {};
}

// Programs signed with the hardened runtime ignore DYLD_INSERT_LIBRARIES.
inline bool hardenedRuntime(const std::string& exe) {
    ProcessSpec spec;
    spec.argv = {"/usr/bin/codesign", "-dv", "--verbose=1", exe};
    std::string out;
    if (runProcess(spec, &out) < 0) return false;
    return out.find("(runtime") != std::string::npos || out.find(",runtime") != std::string::npos;
}

// Shows a game running at `gameFps` at up to the chosen frame rate by injecting the present library,
// which inserts motion-compensated in-between images (one game frame of extra latency; the game's
// own speed and physics are untouched). macOS only; Metal and MoltenVK programs that are not
// hardened. Returns false (with a note) when it does not apply.
inline bool applyFrameGeneration(ProcessSpec& spec, const std::string& exe, const LaunchRequest& req, double gameFps,
                                 std::vector<std::string>& notes) {
    // (Interpolated images only: a "blend" choice, which these backends cannot do, gets those too.)
    const int fps = resolveFps(req.settings);
    if (fps == 0) return false;
    if (currentOs() != Os::MacOS) {
        notes.push_back("frame generation is only available on macOS for this backend");
        return false;
    }
    const double target = std::min(360.0, fps < 0 ? double(req.display.refreshHz) : double(fps));
    // Whole images per game frame, never above the cap (the library rounds the same way).
    const int images = static_cast<int>(std::floor(target / gameFps + 0.02));
    if (images < 2) {
        notes.push_back("frame generation off: " + fmtNumber(target) + " Hz is less than twice the game's " +
                        fmtNumber(gameFps) + " fps");
        return false;
    }
    const std::string lib = presentLibraryPath();
    if (lib.empty()) {
        notes.push_back("frame generation unavailable: librrm_present.dylib is missing from Marathon");
        return false;
    }
    if (!dryRun() && hardenedRuntime(exe)) {
        notes.push_back("frame generation unavailable: " + exe + " uses the hardened runtime");
        return false;
    }
    spec.env["DYLD_INSERT_LIBRARIES"] = lib;
    spec.env["RRM_FRAMEGEN_FPS"] = fmtNumber(target);
    spec.env["RRM_FRAMEGEN_GAMEFPS"] = fmtNumber(gameFps);
    notes.push_back("frame generation: " + fmtNumber(gameFps) + " fps game shown at up to " +
                    fmtNumber(images * gameFps) + " images per second (" + std::to_string(images) +
                    " per game frame, fewer if the screen is slower)");
    return true;
}

// ---- macOS: Intel-only binaries need Rosetta 2 -------------------------------------------------

inline bool rosettaInstalled() {
#if defined(__APPLE__)
    std::error_code ec;
    return fs::exists("/Library/Apple/usr/libexec/oah/libRosettaRuntime", ec);
#else
    return false;
#endif
}

// True when a Mach-O file has an x86_64 slice but no arm64 slice.
inline bool machoIsIntelOnly(const std::string& exe) {
    std::ifstream in(utf8ToPath(exe), std::ios::binary);
    if (!in) return false;
    unsigned char h[4096] = {};
    in.read(reinterpret_cast<char*>(h), sizeof(h));
    size_t n = static_cast<size_t>(in.gcount());
    if (n < 8) return false;
    auto be32 = [&](size_t o) { return o + 4 <= n ? (uint32_t(h[o]) << 24 | uint32_t(h[o + 1]) << 16 | uint32_t(h[o + 2]) << 8 | h[o + 3]) : 0u; };
    auto le32 = [&](size_t o) { return o + 4 <= n ? (uint32_t(h[o + 3]) << 24 | uint32_t(h[o + 2]) << 16 | uint32_t(h[o + 1]) << 8 | h[o]) : 0u; };
    const uint32_t kX86 = 0x01000007, kArm = 0x0100000C;
    uint32_t magicBe = be32(0);
    if (magicBe == 0xCAFEBABE || magicBe == 0xCAFEBABF) {  // fat binary (big-endian header)
        uint32_t count = be32(4);
        bool x86 = false, arm = false;
        size_t stride = magicBe == 0xCAFEBABF ? 32 : 20;
        for (uint32_t i = 0; i < count && i < 16; ++i) {
            uint32_t cpu = be32(8 + i * stride);
            x86 = x86 || cpu == kX86;
            arm = arm || cpu == kArm;
        }
        return x86 && !arm;
    }
    uint32_t magicLe = le32(0);
    if (magicLe == 0xFEEDFACF) return le32(4) == kX86;
    return false;
}

// ---- installs -----------------------------------------------------------------------------------

struct Download {
    std::string url, version, name, sha256;  // sha256: lower-case hex, empty if unknown
    uint64_t size = 0;
};

// The latest *stable* release (GitHub's /releases/latest), robust for projects that
// publish dozens of nightly pre-releases between stable versions.
inline bool githubStableAsset(const std::string& repo, const std::string& pattern, Download& out, std::string& error) {
    std::string body;
    if (!httpGet("https://api.github.com/repos/" + repo + "/releases/latest", body, error)) return false;
    json rel;
    try {
        rel = json::parse(body);
    } catch (const std::exception& e) {
        error = std::string("GitHub API returned invalid JSON: ") + e.what();
        return false;
    }
    for (auto& a : rel.value("assets", json::array())) {
        std::string name = jget<std::string>(a, "name", "");
        if (!globMatch(pattern, name)) continue;
        out.name = name;
        out.url = jget<std::string>(a, "browser_download_url", "");
        out.size = jget<uint64_t>(a, "size", 0);
        out.version = jget<std::string>(rel, "tag_name", "latest");
        std::string digest = jget<std::string>(a, "digest", "");
        if (digest.rfind("sha256:", 0) == 0) out.sha256 = lower(digest.substr(7));
        return true;
    }
    error = "No asset matching \"" + pattern + "\" in the latest " + repo + " release";
    return false;
}

inline bool githubAnyAsset(const std::string& repo, const std::string& pattern, bool prerelease, Download& out,
                           std::string& error) {
    GithubAsset a;
    if (!githubLatestAsset(repo, pattern, prerelease, a, error)) return false;
    out.url = a.url;
    out.name = a.name;
    out.version = a.tag;
    out.size = a.size;
    out.sha256 = lower(a.sha256);
    return true;
}

// Downloads `dl` and unpacks it into a fresh paths().backends/<id>/<version>.
// archive: zip | 7z | tar.gz | tar.xz | dmg | appimage | none
inline bool downloadAndUnpack(const BackendDef& def, TaskProgress& progress, const Download& dl, const std::string& archive,
                              fs::path& destOut, std::string& error) {
    std::error_code ec;
    std::string name = dl.name.empty() ? dl.url.substr(dl.url.find_last_of('/') + 1) : dl.name;
    if (auto q = name.find('?'); q != std::string::npos) name = name.substr(0, q);
    fs::path cache = paths().cache / "downloads";
    fs::create_directories(cache, ec);
    fs::path file = cache / utf8ToPath(name);
    progress.total = dl.size;
    progress.done = 0;
    progress.setStage("Downloading " + name);
    if (!downloadFile(dl.url, pathToUtf8(file), &progress, error)) return false;
    if (progress.cancel) {
        fs::remove(file, ec);
        error = "Cancelled";
        return false;
    }
    if (!dl.sha256.empty()) {
        progress.setStage("Verifying " + name);
        std::string actual = sha256File(pathToUtf8(file));
        if (actual != dl.sha256) {
            fs::remove(file, ec);
            error = "Checksum mismatch for " + name + " (expected " + dl.sha256 + ", got " + actual + ")";
            return false;
        }
        progress.addLog("sha256 ok " + actual);
    }
    std::string version = dl.version.empty() ? "latest" : dl.version;
    for (auto& c : version)
        if (c == '/' || c == '\\' || c == ':') c = '_';
    fs::path dest = paths().backends / utf8ToPath(def.id) / utf8ToPath(version);
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    if (archive == "none" || archive == "appimage" || archive.empty()) {
        fs::copy_file(file, dest / file.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Copy failed: " + ec.message();
            return false;
        }
    } else {
        progress.setStage("Extracting " + name);
        if (!extractArchive(pathToUtf8(file), pathToUtf8(dest), error, &progress)) return false;
    }
    fs::remove(file, ec);
#if defined(__APPLE__)
    if (currentOs() == Os::MacOS) {
        ProcessSpec spec;
        spec.argv = {"/usr/bin/xattr", "-dr", "com.apple.quarantine", pathToUtf8(dest)};
        runProcess(spec);
    }
#endif
    destOut = dest;
    return true;
}

// Finds the program inside an install (glob relative to root) and marks it executable.
inline std::string locateExecutable(const fs::path& root, const std::string& glob) {
    if (glob.empty()) return {};
    std::string exe = findInTree(pathToUtf8(root), glob);
#if !defined(_WIN32)
    if (!exe.empty()) {
        std::error_code ec;
        fs::permissions(utf8ToPath(exe), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, ec);
    }
#endif
    return exe;
}

inline void recordInstall(Library& lib, const BackendDef& def, const std::string& version, const fs::path& root,
                          const std::string& exe) {
    BackendInstall bi;
    bi.backendId = def.id;
    bi.version = version;
    bi.root = pathToUtf8(root);
    bi.executable = exe;
    lib.setInstall(bi);  // the app merges and saves (install() runs on a worker thread)
}

// RAII "running" flag for TaskProgress.
struct RunningGuard {
    TaskProgress& p;
    explicit RunningGuard(TaskProgress& progress) : p(progress) { p.running = true; }
    ~RunningGuard() { p.running = false; }
    RunningGuard(const RunningGuard&) = delete;
    RunningGuard& operator=(const RunningGuard&) = delete;
};

// Directory that contains a macOS .app bundle (or the executable's own folder elsewhere).
inline fs::path programFolder(const std::string& exe) {
    fs::path p = utf8ToPath(exe);
    for (fs::path q = p.parent_path(); !q.empty() && q != q.root_path(); q = q.parent_path())
        if (endsWithI(pathToUtf8(q.filename()), ".app")) return q.parent_path();
    return p.parent_path();
}

// ---- MAME romsets ---------------------------------------------------------------------------------

// The MAME set name for an arcade game: option params "set", else the catalog's identify.mameSet.
inline std::string mameSetName(const LaunchRequest& req) {
    std::string set;
    if (req.option) set = jget<std::string>(req.option->params, "set", "");
    if (set.empty() && req.game) set = req.game->identify.mameSet;
    return set;
}

// Rebuilds <outDir>/<set>.zip from the user's ROM folder/zip (files matched by CRC32) unless an
// up-to-date copy exists. Extra ROMs the catalog does not list yet (device ROMs such as
// C74/C71 internal ROMs, MCU data) come from backend params "setExtras": {"<set>": [{file,size,crc32}]}
// and are included when the user's source has them. Returns the zip path or "" with error.
inline std::string prepareMameSet(const LaunchRequest& req, const fs::path& outDir, std::string& error) {
    if (!req.game || !req.entry || !req.entry->hasFiles()) {
        error = "No romset has been added for this game.";
        return {};
    }
    const std::string set = mameSetName(req);
    if (set.empty()) {
        error = "The catalog has no MAME set name for " + req.game->title + ".";
        return {};
    }
    const std::string source = req.entry->files.path;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    fs::path zip = outDir / utf8ToPath(set + ".zip");
    fs::path stampFile = outDir / utf8ToPath(set + ".zip.stamp");

    GameDef def = *req.game;
    // Extras listed by the backend for this set, kept only if present in the source.
    json extras = json::array();
    if (req.backend) {
        json all = req.backend->params.value("setExtras", json::object());
        if (all.is_object()) extras = all.value(set, json::array());
    }
    std::vector<KnownDump> wanted;
    for (auto& e : extras) {
        KnownDump d;
        d.label = "MAME " + set;
        d.file = jget<std::string>(e, "file", "");
        d.size = jget<uint64_t>(e, "size", 0);
        d.crc32 = lower(jget<std::string>(e, "crc32", ""));
        if (d.file.empty() || d.crc32.empty()) continue;
        bool listed = false;
        for (auto& k : def.identify.dumps) listed = listed || lower(k.file) == lower(d.file);
        if (!listed) wanted.push_back(d);
    }

    // Stamp: source identity + expected entry names, so catalog or source changes trigger a rebuild.
    std::string stamp = source;
    auto srcTime = fs::last_write_time(utf8ToPath(source), ec);
    if (!ec) stamp += "|" + std::to_string(static_cast<long long>(srcTime.time_since_epoch().count()));
    for (auto& d : def.identify.dumps) stamp += "|" + d.file;
    for (auto& d : wanted) stamp += "|+" + d.file;
    if (fs::exists(zip, ec) && readText(stampFile) == stamp) return pathToUtf8(zip);

    if (!wanted.empty()) {
        std::vector<RomFile> files;
        if (listRomSource(source, files, nullptr)) {
            computeRomCrcs(files, [&](const RomFile& f) {
                for (auto& d : wanted)
                    if (d.size == 0 || d.size == f.size) return true;
                return false;
            });
            for (auto& d : wanted) {
                uint32_t crc = 0;
                if (!parseCrc32(d.crc32, crc)) continue;
                for (auto& f : files)
                    if (f.hasCrc && f.crc32 == crc) {
                        def.identify.dumps.push_back(d);
                        break;
                    }
            }
        }
    }
    if (!rebuildMameSet(def, source, pathToUtf8(zip), error)) return {};
    writeText(stampFile, stamp);
    return pathToUtf8(zip);
}

// Writes the device sets a machine needs beside its romset (MAME looks for them in the
// same folder), e.g. Namco System 22's namcoc71.zip / namcoc74.zip MCU ROMs. Option params
// "devices": [{"set": "namcoc74", "file": "c74.bin", "crc32": "a3dce360"}, ...]; each file is
// found by CRC in the user's ROM folder or zip.
inline bool prepareMameDevices(const LaunchRequest& req, const fs::path& outDir, std::string& error) {
    if (!req.option || !req.entry || !req.game) return true;
    json devices = req.option->params.value("devices", json::array());
    if (!devices.is_array()) return true;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    for (auto& d : devices) {
        const std::string set = jget<std::string>(d, "set", ""), file = jget<std::string>(d, "file", "");
        const std::string crc = jget<std::string>(d, "crc32", "");
        if (set.empty() || file.empty() || crc.empty()) continue;
        fs::path zip = outDir / utf8ToPath(set + ".zip");
        if (fs::exists(zip, ec)) continue;
        std::string bytes;
        if (!findFileByCrc(req.entry->files.path, crc, bytes)) {
            error = "Your " + req.game->title + " ROMs are missing " + file + " (CRC " + crc + "), the " + set +
                    " device ROM that MAME needs. Add it to the same folder or zip as the game ROMs.";
            return false;
        }
        if (!writeStoredZip({{file, bytes}}, pathToUtf8(zip), error)) return false;
    }
    return true;
}

}  // namespace rrm::emu
