// A backend runs one game: a native recomp/port, an external emulator, a
// libretro core hosted inside Marathon, or (Android) another installed app.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Catalog.h"
#include "core/Controllers.h"
#include "core/Library.h"
#include "core/Settings.h"

namespace rrm {

struct DisplayInfo {
    int width = 1920, height = 1080;  // pixels of the display Marathon is on
    int refreshHz = 60;
    float dpiScale = 1.0f;
};

// Shared progress object for long tasks (downloads, extraction, builds).
struct TaskProgress {
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> done{0}, total{0};
    std::mutex mutex;
    std::string stage;   // guarded by mutex
    std::string error;   // guarded by mutex
    std::vector<std::string> log;  // guarded by mutex, capped
    void setStage(const std::string& s);
    void fail(const std::string& e);
    void addLog(const std::string& line);
};

struct LaunchRequest {
    const GameDef* game = nullptr;
    const GameBackendOption* option = nullptr;   // the catalog option chosen for this game
    const BackendDef* backend = nullptr;
    const LibraryEntry* entry = nullptr;
    GameSettings settings;                        // effective settings
    DisplayInfo display;
    const Library* library = nullptr;             // for BIOS / firmware lookups
    ControllerSettings controllers;               // effective: global.controllers.withOverrides(settings.extra)
    std::vector<ConnectedPad> pads;               // connected gamepads, connection order, player slots set (may be empty)
};

// How a backend uses a standard gamepad for one game (the game page "CONTROLS" panel, docs).
struct ControlsInfo {
    bool steering = false, pedals = false, vibration = false, deadzone = false;  // settings applied here
    bool playerChoice = false;  // Marathon's Player 1/2 choice is passed on (else the program numbers pads itself)
    int players = 1;            // local players with their own controller
    std::vector<std::string> layout;  // e.g. "Left stick: steer (analog)"
    std::string menu;           // the program's own menu / quit, pad and keyboard
    std::string note;           // a one-time action for the player, if any
};

// Result of a launch. External backends hand back a running process;
// the libretro backend asks the app to switch into its in-process player.
struct LaunchResult {
    bool ok = false;
    std::string error;
    enum class Mode { ExternalProcess, InProcessCore, ExternalApp } mode = Mode::ExternalProcess;
    int64_t pid = 0;
    std::string corePath;        // InProcessCore: core library to load
    std::string contentPath;     // InProcessCore: content to load
    json coreOptions;            // InProcessCore: key -> value
    std::string presentAspect;   // InProcessCore: player aspect override ("auto" = as the core reports); "" = settings
    std::string systemDir, saveDir;
    // ExternalProcess: argv for the log. ExternalApp (Android): intent spec, see launchExternalApp()
    // in core/Platform.h: {"android-intent", "<pkg>/<activity>", "<action>", "<path|uri>", "extra:k=v"...}
    std::vector<std::string> commandLine;
};

enum class InstallState { NotInstalled, Installed, Unavailable, UserProvided };

// missingRequirements() entries that start with "Warning:" are advice, not blockers.
inline bool isRequirementWarning(const std::string& s) { return s.rfind("Warning:", 0) == 0; }

class Backend {
public:
    explicit Backend(const BackendDef& def) : def_(def) {}
    virtual ~Backend() = default;

    const BackendDef& def() const { return def_; }

    // Can this backend run at all on the current OS/arch?
    virtual bool availableHere() const;
    virtual InstallState installState(const Library& lib) const;
    // Downloads/installs on a WORKER THREAD into a scratch copy of the library:
    // record the result with lib.setInstall() but never call lib.save() here —
    // the app merges the record on the main thread. Returns false with progress.error set.
    virtual bool install(Library& lib, TaskProgress& progress);
    virtual bool uninstall(Library& lib);
    // Uses an installation the user already has (a folder or an executable).
    virtual bool linkExisting(Library& lib, const std::string& path, std::string& error);
    // Where the installed program lives ("" if not installed).
    std::string installedExecutable(const Library& lib) const;
    std::string installedRoot(const Library& lib) const;

    // Missing requirements, human readable ("PS2 BIOS required: ...").
    virtual std::vector<std::string> missingRequirements(const LaunchRequest& req) const;

    // Writes configs and starts the game.
    virtual LaunchResult launch(const LaunchRequest& req) = 0;

    // Which unified settings this backend honours for a game (see docs/SETTINGS.md).
    // Merges backend capabilities with the game option's overrides.
    json capabilities(const GameBackendOption* option) const;

    // How the game is played with a gamepad through this backend (docs/CONTROLLERS.md).
    virtual ControlsInfo controls(const LaunchRequest&) const { return {}; }

protected:
    const BackendDef& def_;
};

// Factory: creates the right implementation for a catalog entry.
std::unique_ptr<Backend> createBackend(const BackendDef& def);

}  // namespace rrm
