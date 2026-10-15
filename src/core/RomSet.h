// MAME romsets: list a romset source (folder of loose ROMs or .zip), find
// files by CRC32 and rebuild a set under the catalog's (current MAME) names.
// Sources are only read; output zips are stored (uncompressed).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Catalog.h"

namespace rrm {

// One file of a romset source: a loose file in a folder or an entry of a .zip.
struct RomFile {
    std::string name;     // file name; zip entry names may contain '/'
    std::string path;     // folder/single-file sources: full path; empty for zip entries
    uint64_t size = 0;
    uint32_t crc32 = 0;
    bool hasCrc = false;  // zip: from the central directory; folder: after computeRomCrcs()
    // Zip entry location (used by readRomFile).
    uint64_t zipOffset = 0;  // local header offset
    uint64_t compressedSize = 0;
    uint16_t method = 0;     // 0 = stored, 8 = deflate
    uint16_t flags = 0;
};

// Lists a folder (regular files directly inside, hidden files skipped), a .zip
// (central directory only, nothing is decompressed) or a single file.
bool listRomSource(const std::string& sourcePath, std::vector<RomFile>& out, std::string* error = nullptr);

// Computes CRC32s of folder entries that pass `filter` (all when empty).
// Returns false when cancelled.
bool computeRomCrcs(std::vector<RomFile>& files, const std::function<bool(const RomFile&)>& filter = {},
                    const std::atomic<bool>* cancel = nullptr);

// Reads one file of a source fully (zip entries are inflated) and checks its CRC32 when known.
bool readRomFile(const std::string& sourcePath, const RomFile& file, std::string& out, std::string* error = nullptr);

// Finds a file by CRC32 ("a3dce360", "0xA3DCE360") in a folder, .zip or single file.
bool findFileByCrc(const std::string& sourcePath, const std::string& crc32hex, std::string& outBytes);

// Writes a stored zip from (name in zip, bytes) pairs; outZip is replaced atomically.
bool writeStoredZip(const std::vector<std::pair<std::string, std::string>>& entries, const std::string& outZip,
                    std::string& error);
bool writeSingleFileZip(const std::string& srcFile, const std::string& nameInZip, const std::string& outZip,
                        std::string& error);

// Writes every file of game.identify.dumps under its catalog name, sourced by
// CRC32 from the user's folder or zip (file names there do not matter). Fails
// without writing anything when a ROM is missing.
bool rebuildMameSet(const GameDef& game, const std::string& sourcePath, const std::string& outZipPath,
                    std::string& error);

// Parses 1-8 hex digits with an optional "0x" prefix.
bool parseCrc32(const std::string& hex, uint32_t& out);

}  // namespace rrm
