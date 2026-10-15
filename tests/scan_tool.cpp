// Developer CLI for the scanner.
//   scan_tool <catalogDir> <folder> [--verify] [--verify-max-mb N] [--depth N]
//             [--rebuild-mame <gameId> <outZip>] [--extract-crc <crc32> <nameInZip> <outZip>]
//             [--identify <path>]... [--system <file>]...
// Scans <folder>, prints every hit, optionally full-hashes them and rebuilds a
// MAME set (or a single-ROM zip such as namcoc74.zip) from the romset found.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/Catalog.h"
#include "core/Platform.h"
#include "core/RomSet.h"
#include "core/Scanner.h"

using namespace rrm;

namespace {

bool readText(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::string shortPath(const std::string& path, const fs::path& root) {
    const fs::path rel = utf8ToPath(path).lexically_relative(root);
    const std::string s = pathToUtf8(rel);
    return (s.empty() || s.rfind("..", 0) == 0) ? path : s;
}

void printHit(const ScanHit& h, const fs::path& root) {
    std::printf("%-18s %-11s %-24s %-30s %s\n", h.gameId.c_str(), h.files.format.c_str(), h.files.detectedBy.c_str(),
                h.files.verified.empty() ? "-" : h.files.verified.c_str(), shortPath(h.files.path, root).c_str());
    if (!h.message.empty()) std::printf("%-18s   note: %s\n", "", h.message.c_str());
}

const ScanHit* firstRomset(const std::vector<ScanHit>& hits, const std::string& gameId) {
    for (const auto& h : hits)
        if ((gameId.empty() || h.gameId == gameId) &&
            (h.files.format == "romset-dir" || h.files.format == "romset-zip"))
            return &h;
    return nullptr;
}

int usage() {
    std::fprintf(stderr,
                 "usage: scan_tool <catalogDir> <folder> [--verify] [--verify-max-mb N] [--depth N]\n"
                 "                 [--rebuild-mame <gameId> <outZip>] [--extract-crc <crc32> <nameInZip> <outZip>]\n"
                 "                 [--identify <path>]... [--system <file>]...\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string catalogDir = argv[1], folder = argv[2];
    bool verify = false;
    uint64_t verifyMax = UINT64_MAX;
    int depth = 6;
    std::string rebuildId, rebuildOut, extractCrc, extractName, extractOut;
    std::vector<std::string> identifyPaths, systemFiles;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s: missing argument\n", a.c_str());
                std::exit(usage());
            }
            return argv[++i];
        };
        if (a == "--verify") {
            verify = true;
        } else if (a == "--verify-max-mb") {
            verifyMax = std::strtoull(next().c_str(), nullptr, 10) << 20;
        } else if (a == "--depth") {
            depth = std::atoi(next().c_str());
        } else if (a == "--rebuild-mame") {
            rebuildId = next();
            rebuildOut = next();
        } else if (a == "--extract-crc") {
            extractCrc = next();
            extractName = next();
            extractOut = next();
        } else if (a == "--identify") {
            identifyPaths.push_back(next());
        } else if (a == "--system") {
            systemFiles.push_back(next());
        } else {
            return usage();
        }
    }

    std::string games, backends, recomps, err;
    const fs::path cat = utf8ToPath(catalogDir);
    if (!readText(cat / "games.json", games) || !readText(cat / "backends.json", backends)) {
        std::fprintf(stderr, "cannot read games.json/backends.json in %s\n", catalogDir.c_str());
        return 1;
    }
    readText(cat / "recomps.json", recomps);  // optional second backend list
    Catalog catalog;
    if (!catalog.loadFromStrings(games, backends, err, recomps)) {
        std::fprintf(stderr, "catalog: %s\n", err.c_str());
        return 1;
    }

    std::error_code ec;
    fs::path root = fs::absolute(utf8ToPath(folder), ec).lexically_normal();
    ScanProgress progress;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<ScanHit> hits = scanFolder(catalog, folder, &progress, depth);
    std::printf("scanned %s\n  %d files, %zu hits in %.3f s\n\n", folder.c_str(), progress.filesSeen.load(),
                hits.size(), secondsSince(t0));
    std::printf("%-18s %-11s %-24s %-30s %s\n", "game", "format", "detected by", "verified", "path");

    int failures = 0;
    for (auto& h : hits) {
        if (verify && h.files.size <= verifyMax) {
            const auto tv = std::chrono::steady_clock::now();
            verifyHit(catalog, h);
            std::printf("%-18s   verified in %.2f s\n", "", secondsSince(tv));
            if (h.files.verified == "mismatch") ++failures;
        }
        printHit(h, root);
    }

    std::set<std::string> found;
    for (const auto& h : hits) found.insert(h.gameId);
    std::string missing;
    for (const auto& g : catalog.games())
        if (!found.count(g.id)) missing += " " + g.id;
    std::printf("\ncatalog games found: %zu/%zu%s%s\n", found.size(), catalog.games().size(),
                missing.empty() ? "" : "  missing:", missing.c_str());

    for (const auto& p : identifyPaths) {
        ScanHit h;
        const bool ok = identifyPath(catalog, p, h);
        std::printf("\nidentify %s -> %s\n", p.c_str(), ok ? "" : "(not a catalog game)");
        if (ok || !h.message.empty() || !h.serial.empty()) printHit(h, root);
    }
    for (const auto& s : systemFiles) {
        const std::string kind = identifySystemFile(s);
        std::printf("system %s -> %s\n", s.c_str(), kind.empty() ? "(unknown)" : kind.c_str());
    }

    if (!rebuildId.empty()) {
        const GameDef* g = catalog.game(rebuildId);
        const ScanHit* src = firstRomset(hits, rebuildId);
        if (!g || !src) {
            std::fprintf(stderr, "\nrebuild: no romset found for %s\n", rebuildId.c_str());
            return 1;
        }
        const auto tr = std::chrono::steady_clock::now();
        if (!rebuildMameSet(*g, src->files.path, rebuildOut, err)) {
            std::fprintf(stderr, "\nrebuild failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("\nrebuilt %s from %s -> %s in %.2f s\n", rebuildId.c_str(), src->files.path.c_str(),
                    rebuildOut.c_str(), secondsSince(tr));
    }
    if (!extractCrc.empty()) {
        const ScanHit* src = firstRomset(hits, "");
        std::string bytes;
        if (!src || !findFileByCrc(src->files.path, extractCrc, bytes)) {
            std::fprintf(stderr, "\nextract: crc %s not found\n", extractCrc.c_str());
            return 1;
        }
        if (!writeStoredZip({{extractName, bytes}}, extractOut, err)) {
            std::fprintf(stderr, "\nextract failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("wrote %s (%s, %zu bytes)\n", extractOut.c_str(), extractName.c_str(), bytes.size());
    }
    return failures ? 3 : 0;
}
