// The built-in catalog: which games Marathon knows, how to recognise the
// user's dumps, and which backends can run each game on each OS.
// Data lives in catalog/games.json, catalog/backends.json and catalog/recomps.json;
// extra catalog files in the user's data folder add to it (docs/CATALOG.md).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Platform.h"

namespace rrm {

// A known good dump (Redump/MAME) used to verify the user's file.
struct KnownDump {
    std::string label;  // "Redump USA", "MAME raveracw"
    std::string file;   // canonical file name (informational)
    uint64_t size = 0;
    std::string crc32, md5, sha1;  // lower-case hex, may be empty
};

// How a game is recognised.
struct IdentifySpec {
    std::vector<std::string> bootSerials;  // PS1/PS2 SYSTEM.CNF boot exe, e.g. "SCUS_943.00"
    std::vector<std::string> discIds;      // PSP/PS3 PARAM.SFO DISC_ID / TITLE_ID, e.g. "ULES00xxx"
    std::vector<std::string> xboxTitleIds; // Xbox 360 XEX title id, hex upper-case "4E4D07D2"
    std::string mameSet;                   // MAME romset name, e.g. "raveracw"
    std::vector<std::string> mameParents;  // accepted alternative set names
    std::vector<KnownDump> dumps;          // ROM CRCs for MAME, track 1 / ISO hashes for discs
    std::vector<std::string> nameHints;    // lower-case file name fragments for a cheap first pass
};

// One way of running a game, ranked per OS.
struct GameBackendOption {
    std::string backend;            // id in backends.json
    std::vector<Os> os;             // where this option applies
    int priority = 0;               // higher wins when several are usable
    std::string status;             // "recommended" | "supported" | "experimental" | "external-app"
    std::string note;               // shown in the UI
    json params;                    // backend-specific (recomp project, MAME set, patches...)
    json capabilities;              // overrides/extends the backend's capabilities for this game
};

struct GameDef {
    std::string id;          // stable id, e.g. "ridge-racer"
    std::string title;       // "Ridge Racer"
    std::string system;      // "ps1" | "arcade" | "psp" | "ps2" | "ps3" | "x360"
    std::string systemLabel; // "PlayStation", "Namco System 22", ...
    int year = 0;
    std::string region;      // "USA"
    std::string serial;      // "SCUS-94300"
    std::string accent;      // "#F5C400" card colour
    std::string blurb;
    std::string mediaKind;   // "cue" | "iso" | "romset"
    std::vector<std::string> acceptedFormats; // "cue", "chd", "iso", "zip", "folder"
    IdentifySpec identify;
    std::vector<GameBackendOption> backends;
    std::vector<std::string> requirements;    // "bios:ps2", "firmware:ps3"
    std::string fileHelp;                     // what the user should supply
};

// External programs, libretro cores and in-app recomp runtimes.
struct BackendSource {
    std::string platform;  // "macos-arm64", "windows-x86_64", "linux-x86_64", "android-arm64", "ios-arm64"
    std::string kind;      // "url" | "github" | "bundled" | "external-app" | "user"
    std::string url;       // direct URL (kind=url)
    std::string sha256;    // optional pinned digest for direct downloads
    std::string repo;      // owner/name (kind=github)
    std::string assetPattern;   // glob for the release asset (kind=github)
    std::string archive;   // "zip" | "7z" | "tar.gz" | "dmg" | "appimage" | "none"
    std::string executable;     // path inside the extracted tree (glob allowed)
    std::string marker;         // file that must exist relative to the executable's folder ("" = none):
                                // tells a build apart from others with the same executable name
    std::string androidPackage; // kind=external-app on Android
    std::string note;
    bool accepts(const std::string& exe) const;  // exe carries the marker (always true without one)
};

struct BackendDef {
    std::string id;        // "ppsspp-core", "pcsx2", "psxrecomp-r4" ...
    std::string adapter;   // adapter createBackend() uses (catalog "adapter", default: the id),
                           // so one adapter can run several entries with different params
    std::string name;      // "PPSSPP"
    std::string kind;      // "libretro" | "external" | "recomp" | "port" | "android-app"
    std::string license;
    std::string homepage;
    std::string description;
    std::string core;      // libretro core base name, e.g. "ppsspp" (kind=libretro)
    json capabilities;     // see docs/SETTINGS.md
    std::vector<BackendSource> sources;
    json params;           // free-form defaults
    const BackendSource* sourceFor(const std::string& platform) const;
};

class Catalog {
public:
    // From resources, then the extra catalog files in extraCatalogDir().
    bool load(std::string& error);
    // recompsJson: optional second backend list (catalog/recomps.json), merged after backendsJson.
    bool loadFromStrings(const std::string& gamesJson, const std::string& backendsJson, std::string& error,
                         const std::string& recompsJson = {});

    // Extra catalog files (docs/CATALOG.md): every *.json in dir, in file-name order, merged after
    // the bundled catalog. {"schema": 1, "backends": [...], "gameOptions": {"<game id>": [...]}}:
    // a backend with a known id replaces it, a new one is added; game options are appended.
    // A file with any error is skipped as a whole; the reason is logged and kept in warnings().
    void loadExtraFiles(const fs::path& dir);
    // One extra catalog file's text. On error returns false and leaves the catalog unchanged.
    bool mergeExtra(const std::string& extraJson, std::string& error);
    // Extra files that were skipped, with the reason ("<file>: <error>").
    const std::vector<std::string>& warnings() const { return warnings_; }

    const std::vector<GameDef>& games() const { return games_; }
    const std::vector<BackendDef>& backends() const { return backends_; }
    const GameDef* game(const std::string& id) const;
    const BackendDef* backend(const std::string& id) const;

    // Options for this OS, best first.
    std::vector<const GameBackendOption*> optionsFor(const GameDef& g, Os os) const;

private:
    std::vector<GameDef> games_;
    std::vector<BackendDef> backends_;
    std::vector<std::string> warnings_;
};

// "macos-arm64", "windows-x86_64", ... for the running build.
std::string currentPlatformKey();

// Where the user's extra catalog files live: <data dir>/catalog.
fs::path extraCatalogDir();

}  // namespace rrm
