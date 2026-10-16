// The user's own files: which game dumps were found where, which BIOS /
// firmware files are present, and per-game settings. Persisted as JSON in
// the data directory. Marathon never copies game data into its release.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Settings.h"

namespace rrm {

struct GameFiles {
    std::string path;        // main file: .cue / .iso / romset folder or .zip
    std::string format;      // "cue" | "iso" | "chd" | "romset-dir" | "romset-zip" | "folder"
    std::string detectedBy;  // "serial SCUS_943.00", "hash", "mame crc", ...
    std::string verified;    // "" (not checked) | "match:<label>" | "mismatch" | "partial"
    uint64_t size = 0;
    int64_t addedAt = 0;
};

struct LibraryEntry {
    std::string gameId;
    GameFiles files;
    bool hasFiles() const { return !files.path.empty(); }
    bool filesAvailable() const;  // recheck moved/deleted files and disconnected drives
    GameSettings settings;
    bool customSettings = false;   // false = use global defaults
    int64_t lastPlayed = 0;
    int64_t playSeconds = 0;
};

struct BackendInstall {
    std::string backendId;
    std::string version;
    std::string root;        // install directory
    std::string executable;  // resolved executable / core library
    bool userProvided = false;
};

class Library {
public:
    bool load();
    bool save() const;

    LibraryEntry& entry(const std::string& gameId);          // creates on demand
    const LibraryEntry* find(const std::string& gameId) const;
    std::vector<std::string> gameIds() const;

    void setInstall(const BackendInstall& install);
    const BackendInstall* install(const std::string& backendId) const;
    void removeInstall(const std::string& backendId);

    // BIOS / firmware: key "ps1", "ps2", "ps3-firmware" -> path
    std::string systemFile(const std::string& key) const;
    void setSystemFile(const std::string& key, const std::string& path);

    std::vector<std::string> scanFolders;  // remembered folders

private:
    std::map<std::string, LibraryEntry> entries_;
    std::map<std::string, BackendInstall> installs_;
    std::map<std::string, std::string> systemFiles_;
};

}  // namespace rrm
