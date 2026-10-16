#include <chrono>
#include <cstdio>
#include <fstream>

#include "backends/Backend.h"
#include "core/Platform.h"

int main() {
    using namespace rrm;
    const auto root = fs::temp_directory_path() /
        ("rrm-rr6-platform-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    initPaths(pathToUtf8(root / "data").c_str());
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
    };
    auto file = [&](const fs::path& path) {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "fixture";
    };
    const auto win = root / "windows/RR6 Launcher.exe";
    const auto mac = root / "mac/Ridge Racer 6.app/Contents/MacOS/rr6-launcher";
    const auto linux = root / "linux/bin/rr6_recomp";
    for (const auto& exe : {win, mac, linux}) file(exe);
    const auto native = currentOs() == Os::MacOS ? mac : currentOs() == Os::Windows ? win : linux;
    const auto foreign = currentOs() == Os::Windows ? mac : win;
    BackendDef def;
    def.id = "rr6-recomp";
    def.kind = "recomp";
    auto backend = createBackend(def);
    Library library;
    std::string error;
    check(backend->linkExisting(library, pathToUtf8(native), error), "native executable links");
    check(!backend->linkExisting(library, pathToUtf8(foreign), error) && !error.empty(),
          "foreign executable is rejected at link time");
    check(library.install(def.id)->executable == pathToUtf8(native), "failed link preserves native install");
    check(!backend->linkExisting(library, pathToUtf8(foreign.parent_path()), error),
          "foreign-only directory is rejected");
    Library persisted;
    persisted.load();
    check(persisted.install(def.id) && persisted.install(def.id)->executable == pathToUtf8(native),
          "failed link does not persist a foreign install");
    check(backend->linkExisting(library, pathToUtf8(root), error) &&
          library.install(def.id)->executable == pathToUtf8(native), "mixed directory selects native package");

    GameDef game;
    game.id = "ridge-racer-6";
    game.title = "Ridge Racer 6";
    auto& entry = library.entry(game.id);
    entry.files.path = pathToUtf8(root / "game.iso");
    file(utf8ToPath(entry.files.path));
    BackendInstall old;
    old.backendId = def.id;
    old.executable = pathToUtf8(foreign);
    old.root = pathToUtf8(foreign.parent_path());
    old.userProvided = true;
    library.setInstall(old);
    LaunchRequest req;
    req.game = &game;
    req.entry = &entry;
    req.library = &library;
    const auto missing = backend->missingRequirements(req);
    bool blocked = false;
    for (const auto& message : missing)
        if (message.find("Link ") == 0 && !isRequirementWarning(message)) blocked = true;
    check(blocked, "legacy foreign install has an actionable launch blocker");
    auto result = backend->launch(req);
    check(!result.ok && result.pid == 0 && result.error.find("Link ") == 0,
          "direct launch rejects foreign install before extracting or spawning");
    fs::remove_all(root);
    return failures ? 1 : 0;
}
