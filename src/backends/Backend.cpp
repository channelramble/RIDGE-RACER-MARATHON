#include "backends/Backend.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <system_error>

#include "backends/Adapters.h"
#include "core/Net.h"
#include "core/Platform.h"
#include "core/Process.h"

namespace rrm {

// ---- TaskProgress ---------------------------------------------------------------

void TaskProgress::setStage(const std::string& s) {
    std::lock_guard<std::mutex> lock(mutex);
    stage = s;
    if (log.size() > 400) log.erase(log.begin(), log.begin() + 100);
    log.push_back(s);
}

void TaskProgress::fail(const std::string& e) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        error = e;
        log.push_back("error: " + e);
    }
    failed = true;
}

void TaskProgress::addLog(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    if (log.size() > 400) log.erase(log.begin(), log.begin() + 100);
    log.push_back(line);
}

// ---- Backend defaults --------------------------------------------------------------

bool Backend::availableHere() const {
    if (def_.kind == "libretro") return true;  // cores are downloaded (desktop) or bundled (mobile)
    if (def_.kind == "android-app") return currentOs() == Os::Android;
    if (!isDesktop()) return false;
    return def_.sourceFor(currentPlatformKey()) != nullptr;
}

std::string Backend::installedExecutable(const Library& lib) const {
    const BackendInstall* bi = lib.install(def_.id);
    if (!bi || bi->executable.empty()) return {};
    std::error_code ec;
    if (!fs::exists(utf8ToPath(bi->executable), ec)) return {};
    // A source with a marker does not count a build without it, even one linked before.
    if (const BackendSource* src = def_.sourceFor(currentPlatformKey()); src && !src->accepts(bi->executable)) return {};
    return bi->executable;
}

std::string Backend::installedRoot(const Library& lib) const {
    const BackendInstall* bi = lib.install(def_.id);
    return bi ? bi->root : std::string();
}

InstallState Backend::installState(const Library& lib) const {
    if (!availableHere()) return InstallState::Unavailable;
    const BackendInstall* bi = lib.install(def_.id);
    if (bi && !installedExecutable(lib).empty()) return bi->userProvided ? InstallState::UserProvided : InstallState::Installed;
    return InstallState::NotInstalled;
}

bool Backend::install(Library& lib, TaskProgress& progress) {
    progress.running = true;
    struct Done {
        TaskProgress& p;
        ~Done() { p.running = false; }
    } done{progress};

    const BackendSource* src = def_.sourceFor(currentPlatformKey());
    if (!src) {
        progress.fail(def_.name + " is not available for " + currentPlatformKey());
        return false;
    }
    if (src->kind == "user") {
        progress.fail(src->note.empty() ? "Choose your existing " + def_.name + " installation with \"Link existing\"."
                                        : src->note);
        return false;
    }
    if (src->kind == "external-app" || src->kind == "bundled") {
        progress.fail(def_.name + " is provided outside Marathon on this platform.");
        return false;
    }

    std::string url = src->url, version = "direct", assetName, expectedSha, sumsUrl;
    std::string error;
    if (src->kind == "github") {
        progress.setStage("Looking up the latest " + def_.name + " release");
        GithubAsset asset;
        bool pre = jget(def_.params, "prerelease", false);
        if (!githubLatestAsset(src->repo, src->assetPattern, pre, asset, error)) {
            progress.fail(error);
            return false;
        }
        url = asset.url;
        version = asset.tag;
        assetName = asset.name;
        progress.total = asset.size;
        expectedSha = asset.sha256;
        sumsUrl = asset.checksumsUrl;
    }
    if (url.empty()) {
        progress.fail("No download URL for " + def_.name);
        return false;
    }
    if (assetName.empty()) assetName = url.substr(url.find_last_of('/') + 1);

    fs::path cache = paths().cache / "downloads";
    std::error_code ec;
    fs::create_directories(cache, ec);
    fs::path archive = cache / utf8ToPath(assetName);
    progress.setStage("Downloading " + assetName);
    if (!downloadFile(url, pathToUtf8(archive), &progress, error)) {
        progress.fail(error);
        return false;
    }
    if (progress.cancel) {
        progress.fail("Cancelled");
        return false;
    }
    // Integrity: GitHub's published asset digest, or the release's SHA256SUMS file.
    if (expectedSha.empty() && !sumsUrl.empty()) {
        std::string sums, sumsError;
        if (httpGet(sumsUrl, sums, sumsError)) expectedSha = checksumFor(sums, assetName);
    }
    if (!expectedSha.empty()) {
        progress.setStage("Verifying " + assetName);
        std::string actual = sha256File(pathToUtf8(archive));
        if (actual != expectedSha) {
            fs::remove(archive, ec);
            progress.fail("Checksum mismatch for " + assetName + " (expected " + expectedSha + ", got " + actual + ")");
            return false;
        }
        progress.addLog("sha256 ok " + actual);
    }

    fs::path dest = paths().backends / def_.id / utf8ToPath(version);
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    if (src->archive == "none" || src->archive == "appimage") {
        fs::copy_file(archive, dest / archive.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            progress.fail("Copy failed: " + ec.message());
            return false;
        }
    } else {
        progress.setStage("Extracting " + assetName);
        if (!extractArchive(pathToUtf8(archive), pathToUtf8(dest), error, &progress)) {
            progress.fail(error);
            return false;
        }
    }
    std::string exe = src->executable.empty() ? std::string() : findInTree(pathToUtf8(dest), src->executable);
    if (!src->executable.empty() && exe.empty()) {
        progress.fail("Installed, but " + src->executable + " was not found in the download.");
        return false;
    }
#if !defined(_WIN32)
    if (!exe.empty()) fs::permissions(utf8ToPath(exe), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                                      fs::perm_options::add, ec);
#endif
#if defined(__APPLE__)
    if (currentOs() == Os::MacOS) {
        // Downloads made by Marathon itself are not quarantined, but archives may carry the flag inside.
        ProcessSpec spec;
        spec.argv = {"/usr/bin/xattr", "-dr", "com.apple.quarantine", pathToUtf8(dest)};
        runProcess(spec);
    }
#endif
    fs::remove(archive, ec);

    BackendInstall bi;
    bi.backendId = def_.id;
    bi.version = version;
    bi.root = pathToUtf8(dest);
    bi.executable = exe;
    if (!prepareImported(bi, progress)) return false;
    lib.setInstall(bi);  // the caller merges and saves (install() runs on a worker thread)
    progress.setStage(def_.name + " " + version + " installed");
    return true;
}

bool Backend::uninstall(Library& lib) {
    const BackendInstall* bi = lib.install(def_.id);
    if (!bi) return true;
    if (!bi->userProvided && !bi->root.empty()) {
        std::error_code ec;
        fs::path root = utf8ToPath(bi->root);
        // Only ever delete inside Marathon's own backends directory.
        auto rel = fs::relative(root, paths().backends, ec);
        if (!ec && !rel.empty() && rel.native()[0] != '.') fs::remove_all(root, ec);
    }
    lib.removeInstall(def_.id);
    lib.save();
    return true;
}

bool Backend::linkExisting(Library& lib, const std::string& path, std::string& error) {
    std::error_code ec;
    fs::path p = utf8ToPath(path);
    if (!fs::exists(p, ec)) {
        error = "Not found: " + path;
        return false;
    }
    const BackendSource* src = def_.sourceFor(currentPlatformKey());
    // A source with a marker takes only the build carrying it (e.g. a modified build that has
    // the same executable name as the official release).
    const auto marked = [src](const std::string& e) { return !src || src->accepts(e); };
    std::string exe;
    fs::path root = p;
    if (fs::is_directory(p, ec)) {
        const auto find = [&](const std::function<bool(const std::string&)>& accept) {
            std::string found = src && !src->executable.empty() ? findInTree(path, src->executable, 6, accept) : std::string();
            if (found.empty() && src) {
                // Accept a pattern match on the file name alone, anywhere in the tree.
                std::string base = src->executable.substr(src->executable.find_last_of('/') + 1);
                found = findInTree(path, "*" + base, 6, accept);
            }
            return found;
        };
        exe = find(marked);
        if (exe.empty() && src && !src->marker.empty()) exe = find({});  // an unmarked build: refused below, saying why
        if (exe.empty()) {
            error = "Could not find the " + def_.name + " program inside " + path;
            return false;
        }
    } else {
        exe = path;
        root = p.parent_path();
    }
    if (!marked(exe)) {
        error = exe + " has no " + pathToUtf8(utf8ToPath(src->marker).filename()) + " mark, so it is not " + def_.name + "." +
                (src->note.empty() ? std::string() : " " + src->note);
        return false;
    }
    BackendInstall bi;
    bi.backendId = def_.id;
    bi.version = "user";
    bi.root = pathToUtf8(root);
    bi.executable = exe;
    bi.userProvided = true;
    lib.setInstall(bi);
    lib.save();
    return true;
}

std::vector<std::string> Backend::missingRequirements(const LaunchRequest& req) const {
    std::vector<std::string> out;
    if (!req.entry || !req.entry->hasFiles()) out.push_back("Add your " + req.game->title + " files first.");
    else if (!req.entry->filesAvailable())
        out.push_back("The saved game path is missing or unavailable: " + req.entry->files.path +
                      ". Reconnect the drive or add/scan your game files again at their current location.");
    const std::string& adapter = def_.adapter.empty() ? def_.id : def_.adapter;
    for (auto& r : req.game->requirements) {
        if (r == "bios:ps2" && req.library && req.library->systemFile("ps2-bios").empty() && adapter == "pcsx2")
            out.push_back("PCSX2 needs your own PS2 BIOS dump. Add it under Settings > System files.");
        if (r == "firmware:ps3" && req.library && req.library->systemFile("ps3-firmware").empty() && adapter == "rpcs3")
            out.push_back("RPCS3 needs the official PS3 system software (PS3UPDAT.PUP from playstation.com). "
                          "Add it under Settings > System files.");
    }
    return out;
}

json Backend::capabilities(const GameBackendOption* option) const {
    json caps = def_.capabilities.is_object() ? def_.capabilities : json::object();
    if (option && option->capabilities.is_object())
        for (auto& [k, v] : option->capabilities.items()) caps[k] = v;
    return caps;
}

// ---- factory ------------------------------------------------------------------------------

std::unique_ptr<Backend> createBackend(const BackendDef& def) {
    // The catalog's "adapter" (default: the id) picks the implementation, so an extra catalog
    // entry can reuse an adapter under its own id with its own params.
    const std::string& id = def.adapter.empty() ? def.id : def.adapter;
    if (def.kind == "libretro") return makeLibretro(def);
    if (def.kind == "android-app") return makeAndroidApp(def);
    if (id == "rr1-recomp" || id == "r4-recomp") return makePsxRecomp(def);
    if (id == "rage-racer-pc") return makeRageRacerPc(def);
    if (id == "rave-racer-native") return makeRaveRacerNative(def);
    if (id == "fukami") return makeFukami(def);
    if (id == "rr6-recomp") return makeRr6Recomp(def);
    if (id == "pcsx2") return makePcsx2(def);
    if (id == "rpcs3") return makeRpcs3(def);
    if (id == "xenia-canary") return makeXenia(def);
    if (id == "ppsspp") return makePpssppStandalone(def);
    if (id == "mame") return makeMameStandalone(def);
    return nullptr;
}

}  // namespace rrm
