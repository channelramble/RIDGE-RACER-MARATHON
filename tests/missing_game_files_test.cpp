#include <chrono>
#include <cstdio>
#include <fstream>

#include "backends/Backend.h"
#include "core/Platform.h"
#include "libretro/Host.h"

int main() {
    using namespace rrm;
    const auto root = fs::temp_directory_path() /
        ("rrm-missing-game-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
    };
    LibraryEntry entry;
    check(!entry.filesAvailable(), "unassigned game needs files");
    entry.files.path = pathToUtf8(root / "Ridge Racer.cue");
    check(!entry.filesAvailable(), "missing registered disc is unavailable");
    BackendDef def;
    def.id = "lr-swanstation";
    def.kind = "libretro";
    def.core = "swanstation";
    auto backend = createBackend(def);
    GameDef game;
    game.id = "ridge-racer";
    game.title = "Ridge Racer";
    LaunchRequest req;
    req.game = &game;
    req.entry = &entry;
    auto missing = backend->missingRequirements(req);
    check(missing.size() == 1 && missing[0].find(entry.files.path) != std::string::npos &&
          !isRequirementWarning(missing[0]), "missing disc blocks launch with its saved path");

    lr::Host host;
    lr::HostConfig cfg;
    cfg.contentPath = entry.files.path;
    std::string error;
    check(!host.start(nullptr, cfg, error) && error.find("Game file is missing") != std::string::npos,
          "host rejects missing content before opening the core or graphics");
    std::ofstream(root / "Ridge Racer.cue") << "fixture";
    check(entry.filesAvailable(), "restored file becomes available without rescanning");
    check(backend->missingRequirements(req).empty(), "restored disc clears launch blocker");
    check(!host.start(nullptr, cfg, error) && error == "no current OpenGL context",
          "existing content passes the path guard");
    fs::rename(root / "Ridge Racer.cue", root / "moved.cue");
    check(!entry.filesAvailable(), "moving a registered disc invalidates availability");
    entry.files.path = pathToUtf8(root);
    check(entry.filesAvailable(), "external backends can still use game folders");
    cfg.contentPath = entry.files.path;
    check(!host.start(nullptr, cfg, error) && error.find("Game file is missing") != std::string::npos,
          "in-process cores require a file rather than a directory");
    cfg.contentPath.clear();
    check(!host.start(nullptr, cfg, error) && error == "no current OpenGL context",
          "intentional no-content sessions are preserved");
    fs::remove_all(root);
    return failures ? 1 : 0;
}
