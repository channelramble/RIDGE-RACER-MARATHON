// Verify or install the exact offline payloads used by the app, without a window.
#include <SDL3/SDL.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "backends/Backend.h"
#include "core/BackendBundle.h"
#include "core/Catalog.h"
#include "core/Library.h"
#include "core/Platform.h"

using namespace rrm;

static std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text; text << file.rdbuf(); return text.str();
}

int main(int argc, char** argv) {
    std::string bundle, data, catalogDir;
    bool verify = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--platform") { std::puts(currentPlatformKey().c_str()); return 0; }
        if (arg == "--verify-only") verify = true;
        else if ((arg == "--bundle" || arg == "--data" || arg == "--catalog") && i + 1 < argc) {
            std::string value = argv[++i];
            if (arg == "--bundle") bundle = value;
            else if (arg == "--data") data = value;
            else catalogDir = value;
        } else {
            std::fprintf(stderr, "usage: bundle_tool [--bundle DIR] --data DIR [--catalog DIR] [--verify-only]\n"); return 2;
        }
    }
    // Avoid touching a real install accidentally when used as a developer tool.
    if (data.empty()) { std::fprintf(stderr, "bundle_tool requires --data DIR (use a temporary directory for tests).\n"); return 2; }
    if (!SDL_Init(0)) return 2;
    initPaths(data.c_str());
    Catalog catalog;
    std::string error;
    const fs::path cd = utf8ToPath(catalogDir);
    const bool loaded = catalogDir.empty() ? catalog.load(error) :
        catalog.loadFromStrings(read(cd / "games.json"), read(cd / "backends.json"), error, read(cd / "recomps.json"));
    if (!loaded) { std::fprintf(stderr, "%s\n", error.c_str()); SDL_Quit(); return 2; }
    Library library; library.load();
    TaskProgress progress;
    const auto report = importBackendBundle(catalog, library, progress, verify, utf8ToPath(bundle));
    for (const auto& line : progress.log) std::fprintf(stderr, "%s\n", line.c_str());
    bool saved = true;
    if (!verify && !report.imported.empty()) saved = library.save();
    if (!saved) std::fprintf(stderr, "Cannot save library.json in %s\n", data.c_str());
    const json result{{"found", report.found}, {"imported", report.imported}, {"skipped", report.skipped}, {"errors", report.errors}};
    std::puts(result.dump(2).c_str());
    SDL_Quit();
    return report.found && report.ok() && saved ? 0 : 1;
}
