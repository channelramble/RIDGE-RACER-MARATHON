// Recognises the user's game files. Cheap header-based identification first
// (PS1/PS2 SYSTEM.CNF, PSP/PS3 PARAM.SFO, Xbox 360 XEX title id, MAME ROM
// CRCs), optional full-hash verification against catalog dumps second.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Catalog.h"
#include "core/Library.h"

namespace rrm {

struct ScanHit {
    std::string gameId;      // catalog id; empty if unknown
    GameFiles files;
    std::string serial;      // what we read from the media, e.g. "SCUS_943.00", "BLUS30001"
    std::string message;     // human-readable detail / warning
};

struct ScanProgress {
    std::atomic<int> filesSeen{0};
    std::atomic<int> hits{0};
    std::atomic<bool> cancel{false};
    std::string current;     // last path looked at (informational, racy by design)
};

struct SystemFileHit {
    std::string kind;
    std::string path;
};

// Identify a single path (file or folder). Returns false if not a known game.
bool identifyPath(const Catalog& catalog, const std::string& path, ScanHit& out);

// Walk a folder recursively (bounded depth), returning every recognised game.
std::vector<ScanHit> scanFolder(const Catalog& catalog, const std::string& folder,
                                ScanProgress* progress = nullptr, int maxDepth = 6,
                                std::vector<SystemFileHit>* systemFiles = nullptr);

// Full-hash verification of a hit against the catalog's known dumps.
// Fills hit.files.verified with "match:<label>", "mismatch" or "partial".
void verifyHit(const Catalog& catalog, ScanHit& hit, std::atomic<bool>* cancel = nullptr,
               std::function<void(uint64_t done, uint64_t total)> onProgress = {});

// System files (BIOS/firmware). Returns "ps1-bios", "ps2-bios", "ps3-firmware" or "".
std::string identifySystemFile(const std::string& path);

}  // namespace rrm
