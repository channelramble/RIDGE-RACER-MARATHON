// recomp_tool: drives one recomp / native-port backend from the command line,
// the way the app does, so installs and launches can be checked without the UI.
//
//   recomp_tool --data <dir> --game <gameId> --backend <id> --file <path>
//               [--install] [--link <path>] [--dry-run] [--set key=value ...]
//               [--wait <seconds>] [--catalog <dir>] [--display WxH]
//               [--home <dir>] [--controller key=value ...] [--pad NAME[:GUID[:TYPE]] ...]
//
// --data     Marathon data directory to use (backends/, saves/, logs/ ...); extra catalog
//            files in its catalog/ folder are merged as in Marathon (docs/CATALOG.md).
// --file     the game's files as the library would record them (.cue, .iso, ROM folder).
// --install  runs Backend::install() (download/extract/build) first.
// --link     runs Backend::linkExisting() on an existing build.
// --dry-run  writes the configs and prints the command but starts nothing
//            (sets RRM_RECOMP_DRY_RUN=1, which the recomp adapters honour).
// --set      unified GameSettings fields, e.g. --set resolution=4k --set redraw=true
// --wait     seconds to let the game run before terminating it (default 20).
// --home     runs the backend with HOME (and CFFIXED_USER_HOME / XDG_*) pointed at
//            this folder, so ports that keep data in the user's home stay isolated.
// --controller  a controller setting (docs/CONTROLLERS.md): steering=stick|dpad,
//            pedals=triggers|buttons, vibration=true|false, deadzone=0..40, playerN="" | none | <GUID>
// --pad      a connected controller, repeatable, in connection order (players assigned as in
//            Marathon); none by default.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "backends/Backend.h"
#include "core/Catalog.h"
#include "core/Controllers.h"
#include "core/Library.h"
#include "core/Platform.h"
#include "core/Process.h"
#include "core/Settings.h"

#ifndef RRM_SOURCE_DIR
#define RRM_SOURCE_DIR "."
#endif

using namespace rrm;

namespace {

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::string quote(const std::string& s) {
    if (!s.empty() && s.find_first_of(" \t\"'\\$`") == std::string::npos) return s;
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

std::string formatFor(const std::string& path) {
    std::error_code ec;
    if (fs::is_directory(utf8ToPath(path), ec)) return "romset-dir";
    std::string ext = pathToUtf8(utf8ToPath(path).extension());
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".cue") return "cue";
    if (ext == ".iso") return "iso";
    if (ext == ".chd") return "chd";
    if (ext == ".zip") return "romset-zip";
    return "folder";
}

json parseValue(const std::string& v) {
    try {
        return json::parse(v);
    } catch (...) {
        return v;  // bare strings: resolution=4k
    }
}

void printLogTail(const fs::path& log, size_t maxLines) {
    std::string text;
    if (!readFile(log, text)) {
        std::printf("(no log at %s)\n", pathToUtf8(log).c_str());
        return;
    }
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    size_t from = lines.size() > maxLines ? lines.size() - maxLines : 0;
    std::printf("---- %s (last %zu of %zu lines) ----\n", pathToUtf8(log).c_str(), lines.size() - from, lines.size());
    for (size_t i = from; i < lines.size(); ++i) std::printf("%s\n", lines[i].c_str());
    std::printf("---- end of log ----\n");
}

const char* stateName(InstallState s) {
    switch (s) {
        case InstallState::NotInstalled: return "NotInstalled";
        case InstallState::Installed: return "Installed";
        case InstallState::Unavailable: return "Unavailable";
        case InstallState::UserProvided: return "UserProvided";
    }
    return "?";
}

int usage() {
    std::fprintf(stderr,
                 "usage: recomp_tool --data <dir> --game <gameId> --backend <id> --file <path>\n"
                 "                   [--install] [--link <path>] [--dry-run] [--set key=value ...]\n"
                 "                   [--wait <seconds>] [--catalog <dir>] [--display WxH] [--home <dir>]\n"
                 "                   [--controller key=value ...] [--pad NAME[:GUID[:TYPE]] ...]\n");
    return 2;
}

// --controller key=value into ControllerSettings JSON (fromJson validates it).
void setControllerField(json& c, const std::string& k, const std::string& v) {
    if (k.size() == 7 && k.rfind("player", 0) == 0 && k[6] >= '1' && k[6] <= '4') {
        if (!c.contains("players")) c["players"] = json::array({json::object(), json::object(), json::object(), json::object()});
        c["players"][static_cast<size_t>(k[6] - '1')]["controller"] = v;
    } else if (v == "true" || v == "false") {
        c[k] = v == "true";
    } else if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) {
        c[k] = std::stoi(v);
    } else {
        c[k] = v;
    }
}

// --pad NAME[:GUID[:TYPE]] specs as connected pads, numbered and given player slots like Marathon's.
std::vector<ConnectedPad> padsFrom(const std::vector<std::string>& specs, const ControllerSettings& c) {
    std::vector<ConnectedPad> pads;
    for (auto& spec : specs) {
        ConnectedPad p;
        p.id = static_cast<uint32_t>(pads.size() + 1);
        const size_t a = spec.find(':');
        p.name = spec.substr(0, a);
        if (a != std::string::npos) {
            const size_t b = spec.find(':', a + 1);
            p.guid = spec.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            if (b != std::string::npos) p.type = spec.substr(b + 1);
        }
        if (p.type.empty()) p.type = "standard";
        for (auto& q : pads)
            if (q.name == p.name) ++p.nameIndex;
        pads.push_back(p);
    }
    applyAssignment(pads, assignPlayers(pads, c.players));
    return pads;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dataDir, gameId, backendId, file, linkPath, catalogDir = std::string(RRM_SOURCE_DIR) + "/catalog";
    std::string home;
    bool install = false, dryRun = false;
    int waitSeconds = 20;
    int dispW = 0, dispH = 0;
    std::vector<std::pair<std::string, std::string>> sets;
    json controllerJson = json::object();
    std::vector<std::string> padSpecs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--data" && next(v)) dataDir = v;
        else if (a == "--game" && next(v)) gameId = v;
        else if (a == "--backend" && next(v)) backendId = v;
        else if (a == "--file" && next(v)) file = v;
        else if (a == "--link" && next(v)) linkPath = v;
        else if (a == "--catalog" && next(v)) catalogDir = v;
        else if (a == "--home" && next(v)) home = v;
        else if (a == "--wait" && next(v)) waitSeconds = std::atoi(v.c_str());
        else if (a == "--display" && next(v)) std::sscanf(v.c_str(), "%dx%d", &dispW, &dispH);
        else if (a == "--install") install = true;
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--set" && next(v)) {
            size_t eq = v.find('=');
            if (eq == std::string::npos) return usage();
            sets.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        } else if (a == "--controller" && next(v)) {
            size_t eq = v.find('=');
            if (eq == std::string::npos) return usage();
            setControllerField(controllerJson, v.substr(0, eq), v.substr(eq + 1));
        } else if (a == "--pad" && next(v)) {
            padSpecs.push_back(v);
        } else if (a == "--help" || a == "-h") {
            return usage();
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return usage();
        }
    }
    if (dataDir.empty() || gameId.empty() || backendId.empty()) return usage();

    if (!home.empty()) {
        std::error_code ec;
        fs::create_directories(utf8ToPath(home), ec);
        setEnv("HOME", home);
        setEnv("CFFIXED_USER_HOME", home);  // CoreFoundation / NSFileManager on macOS
        setEnv("XDG_DATA_HOME", home + "/.local/share");
        setEnv("XDG_CONFIG_HOME", home + "/.config");
        setEnv("XDG_STATE_HOME", home + "/.local/state");
        std::printf("HOME isolated at %s\n", home.c_str());
    }
    if (dryRun) setEnv("RRM_RECOMP_DRY_RUN", "1");

    initPaths(dataDir.c_str());
    std::printf("platform %s, data %s\n", currentPlatformKey().c_str(), pathToUtf8(paths().data).c_str());

    std::string games, backends, recomps, error;
    if (!readFile(utf8ToPath(catalogDir) / "games.json", games) ||
        !readFile(utf8ToPath(catalogDir) / "backends.json", backends)) {
        std::fprintf(stderr, "cannot read catalog from %s\n", catalogDir.c_str());
        return 1;
    }
    readFile(utf8ToPath(catalogDir) / "recomps.json", recomps);
    Catalog catalog;
    if (!catalog.loadFromStrings(games, backends, error, recomps)) {
        std::fprintf(stderr, "catalog: %s\n", error.c_str());
        return 1;
    }
    catalog.loadExtraFiles(extraCatalogDir());
    const GameDef* game = catalog.game(gameId);
    const BackendDef* def = catalog.backend(backendId);
    if (!game || !def) {
        std::fprintf(stderr, "unknown game or backend\n");
        return 1;
    }
    const GameBackendOption* option = nullptr;
    for (auto& o : game->backends)
        if (o.backend == backendId) option = &o;
    if (!option) std::printf("note: %s is not an option for %s in games.json\n", backendId.c_str(), gameId.c_str());

    auto backend = createBackend(*def);
    if (!backend) {
        std::fprintf(stderr, "no implementation for %s\n", backendId.c_str());
        return 1;
    }

    Library lib;
    lib.load();

    if (!linkPath.empty()) {
        if (!backend->linkExisting(lib, linkPath, error)) {
            std::fprintf(stderr, "link failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("linked %s\n", linkPath.c_str());
    }
    if (install) {
        TaskProgress progress;
        size_t shown = 0;
        bool ok = false;
        std::atomic<bool> finished{false};
        std::thread worker([&] {
            ok = backend->install(lib, progress);
            finished = true;
        });
        auto flush = [&] {
            std::lock_guard<std::mutex> lock(progress.mutex);
            // TaskProgress caps its log by dropping the oldest lines.
            if (shown > progress.log.size()) shown = 0;
            for (; shown < progress.log.size(); ++shown) std::printf("  [install] %s\n", progress.log[shown].c_str());
            std::fflush(stdout);
        };
        while (!finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            flush();
        }
        worker.join();
        flush();
        if (!ok) {
            std::lock_guard<std::mutex> lock(progress.mutex);
            std::fprintf(stderr, "install failed: %s\n", progress.error.c_str());
            return 1;
        }
        lib.save();  // the app merges + saves on the main thread; the tool does it here
        std::printf("installed\n");
    }

    LibraryEntry& entry = lib.entry(gameId);
    if (!file.empty()) {
        entry.files.path = file;
        entry.files.format = formatFor(file);
    }
    json sj = GameSettings().toJson();
    for (auto& [k, v] : sets) sj[k] = parseValue(v);
    GameSettings settings = GameSettings::fromJson(sj);
    entry.settings = settings;
    entry.customSettings = true;
    lib.save();

    DisplayInfo display;
    if (dispW > 0 && dispH > 0) {
        display.width = dispW;
        display.height = dispH;
    } else if (SDL_Init(SDL_INIT_VIDEO)) {
        if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
            display.dpiScale = m->pixel_density > 0 ? m->pixel_density : 1.0f;
            display.width = (int)(m->w * display.dpiScale);
            display.height = (int)(m->h * display.dpiScale);
            display.refreshHz = m->refresh_rate > 0 ? (int)(m->refresh_rate + 0.5f) : 60;
        }
        SDL_Quit();
    }
    std::printf("display %dx%d @ %d Hz\n", display.width, display.height, display.refreshHz);

    LaunchRequest req;
    req.game = game;
    req.option = option;
    req.backend = def;
    req.entry = lib.find(gameId);
    req.settings = settings;
    req.display = display;
    req.library = &lib;
    const ControllerSettings controllers = ControllerSettings::fromJson(controllerJson);
    req.controllers = controllers.withOverrides(settings.extra);
    req.pads = padsFrom(padSpecs, controllers);
    std::printf("controllers: %s\n", req.controllers.toJson().dump().c_str());
    for (auto& p : req.pads)
        std::printf("pad: %s %d (%s) -> %s\n", p.name.c_str(), p.nameIndex, p.guid.empty() ? "no GUID" : p.guid.c_str(),
                    p.player < 0 ? "no player" : ("Player " + std::to_string(p.player + 1)).c_str());

    std::printf("available here: %s, install state: %s\n", backend->availableHere() ? "yes" : "no",
                stateName(backend->installState(lib)));
    if (const BackendInstall* bi = lib.install(backendId))
        std::printf("install: version=%s root=%s exe=%s\n", bi->version.c_str(), bi->root.c_str(), bi->executable.c_str());
    std::printf("capabilities: %s\n", backend->capabilities(option).dump().c_str());
    std::printf("settings: %s\n", settings.toJson().dump().c_str());
    auto missing = backend->missingRequirements(req);
    for (auto& m : missing) std::printf("requirement: %s\n", m.c_str());

    LaunchResult r = backend->launch(req);
    std::string cmd;
    for (auto& a : r.commandLine) cmd += (cmd.empty() ? "" : " ") + quote(a);
    std::printf("launch: ok=%d pid=%lld error=%s\ncommand: %s\n", r.ok ? 1 : 0, (long long)r.pid, r.error.c_str(),
                cmd.c_str());
    fs::path log = paths().logs / (backendId + ".log");
    if (!r.ok || dryRun || r.pid <= 0) {
        printLogTail(log, 60);
        return r.ok ? 0 : 1;
    }

    int exitCode = 0;
    bool exited = false;
    for (int i = 0; i < waitSeconds * 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!processRunning(r.pid, &exitCode)) {
            exited = true;
            break;
        }
    }
    if (exited) {
        std::printf("process %lld EXITED early with code %d\n", (long long)r.pid, exitCode);
    } else {
        std::printf("process %lld still running after %d s: terminating\n", (long long)r.pid, waitSeconds);
        terminateProcess(r.pid);
        for (int i = 0; i < 60 && processRunning(r.pid, &exitCode); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printLogTail(log, 80);
    return exited && exitCode != 0 ? 3 : 0;
}
