// Marathon application: window, UI state and the glue between the catalog,
// the user's library and the backends.
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "backends/Backend.h"
#include "core/Catalog.h"
#include "core/Gamepads.h"
#include "core/Library.h"
#include "core/Scanner.h"
#include "core/Settings.h"
#include "libretro/Host.h"

struct ImFont;

namespace rrm {

namespace ui {
enum class PadAction : uint8_t;  // Ui.h
}

enum class Screen { Library, Game, AddGames, Backends, Settings, About, Playing };

// A running external program (recomp, port or emulator).
struct RunningGame {
    std::string gameId, backendId;
    int64_t pid = 0;
    int64_t startedAt = 0;
    std::string logFile;
    std::vector<std::string> commandLine;
    double quitRequestedAt = 0;  // App::now() when Marathon asked it to close (0 = not asked)
    bool forced = false;         // stopped because it did not close in time
};

// A background job with a progress bar (install, scan, verify, link...).
struct Job {
    std::string id;     // e.g. "install:pcsx2", "scan", "verify:r4"
    std::string title;
    std::shared_ptr<TaskProgress> progress = std::make_shared<TaskProgress>();
    std::thread thread;
    bool finished = false;
    bool ok = false;
    std::shared_ptr<Library> scratch;  // install jobs: worker-side library copy, merged on completion
    std::string backendId;             // install jobs
    std::shared_ptr<std::vector<std::string>> importedBackends;  // offline bundle: successful imports
};

// Result of an asynchronous file dialog, delivered to the main thread.
struct DialogResult {
    std::string purpose;  // "scan-folder", "system:ps2-bios", "link:<backend>", "file:<gameId>"
    std::vector<std::string> paths;
};

struct Fonts {
    ImFont* body = nullptr;
    ImFont* bodyBold = nullptr;
    ImFont* display = nullptr;  // Rajdhani for titles
};

class App {
public:
    bool init(int argc, char** argv);
    void shutdown();
    // Returns false to quit.
    bool iterate();
    bool handleEvent(const SDL_Event& e);

    // ---- used by the UI -----------------------------------------------------------
    Catalog catalog;
    Library library;
    GlobalSettings global;
    Fonts fonts;
    Screen screen = Screen::Library;
    std::string selectedGame;
    DisplayInfo display;
    float uiScale = 1.0f;
    bool touchUi = false;  // larger targets + virtual pad (mobile)

    Backend* backend(const std::string& id);
    const BackendDef* backendDef(const std::string& id) const { return catalog.backend(id); }

    // Effective settings for a game (custom or global defaults).
    GameSettings settingsFor(const std::string& gameId) const;
    void setGameSettings(const std::string& gameId, const GameSettings& s, bool custom);

    // Picks the backend option that will be used for a game on this device.
    const GameBackendOption* chosenOption(const GameDef& g) const;
    // "Ready", "Install backend", "Add files", "Not available on this device"...
    enum class GameStatus { Ready, NeedsFiles, NeedsBackend, NeedsSystemFiles, Unsupported, Running };
    GameStatus statusOf(const GameDef& g) const;
    std::string statusText(const GameDef& g) const;

    void play(const std::string& gameId);
    void stopRunning();                         // asks the running program to close (requestQuitRunning)
    const RunningGame* running() const { return running_ ? &*running_ : nullptr; }
    bool playingInProcess() const { return screen == Screen::Playing; }
    lr::Host& host() { return host_; }
    void exitPlayer();

    // Background jobs
    Job* startJob(const std::string& id, const std::string& title, std::function<bool(TaskProgress&)> fn);
    Job* job(const std::string& id);
    std::vector<std::unique_ptr<Job>>& jobs() { return jobs_; }
    void installBackend(const std::string& backendId);
    // Backends available here that Marathon can download and that are not installed yet.
    std::vector<Backend*> missingDownloadableBackends();
    void installAllBackends();  // installBackend() for each of them
    void installBundledBackends();  // first-run offline setup from the Complete package
    void scanFolder(const std::string& folder);
    void verifyGame(const std::string& gameId);
    std::vector<ScanHit> lastScanHits;  // guarded by scanMutex
    std::vector<SystemFileHit> foundSystemFiles;  // consumed on the main thread
    // Existing local installs of backends found while scanning (backendId, path); guarded by scanMutex.
    std::vector<std::pair<std::string, std::string>> foundInstalls;
    std::mutex scanMutex;
    void acceptHit(const ScanHit& hit);

    // Dialogs (asynchronous on every platform)
    void pickFolder(const std::string& purpose);
    void pickFile(const std::string& purpose, const std::vector<std::pair<std::string, std::string>>& filters);

    // Messages shown as a toast / modal
    void toast(const std::string& text, bool error = false);
    struct Toast {
        std::string text;
        bool error = false;
        double until = 0;
    };
    std::vector<Toast> toasts;
    std::string modalTitle, modalText;  // non-empty = show modal

    SDL_Window* window() const { return window_; }
    double now() const;
    void saveAll();
    // Saves the next frame as a PNG (tests and documentation screenshots).
    void requestScreenshot(const std::string& path) { screenshotPath_ = path; }

    // ---- controllers (docs/CONTROLLERS.md) ---------------------------------------------
    GamepadMonitor& gamepads() { return pads_; }
    // Effective controller settings for a game: the global ones with the game's extra.controllers.
    ControllerSettings controllersFor(const std::string& gameId) const;
    std::vector<ConnectedPad> padSnapshot() const { return pads_.pads(); }
    enum class SettingsPage { General, Controllers } settingsPage = SettingsPage::General;
    std::string focusedGame;            // library card under the pad's focus (or the mouse)
    bool focusSettingsRequest = false;  // game page: move the focus to its settings
    bool padTestActive = false;         // Controllers page test: the pad does not drive the UI
    uint32_t padTestId = 0;
    bool lastInputPad = false;          // the newest input came from a gamepad
    bool assumeFocus = false;           // tests: route the pads as if the window had the input focus
    std::vector<ui::PadAction> padActions;  // gamepad buttons meant for the UI, consumed by ui::draw
    void raiseWindow();                 // brings Marathon's window to the front
    // Asks the running external program to close (Stop button or quit chord); pollRunning()
    // stops it if it is still there 6 seconds later.
    void requestQuitRunning(bool fromChord);

private:
    void applyStyle();
    void loadFonts();
    void updateDisplayInfo();
    void pollRunning();
    void pollJobs();
    void handleDialogResults();
    void drawFrame();
    void startInProcess(const LaunchResult& r, const GameSettings& s, const std::string& gameId);
    void handlePads();
    void updatePadRouting();
    void padButtonEvent(const SDL_Event& e);

    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    std::map<std::string, std::unique_ptr<Backend>> backends_;
    std::optional<RunningGame> running_;
    std::vector<std::unique_ptr<Job>> jobs_;
    std::mutex dialogMutex_;
    std::vector<DialogResult> dialogResults_;
    lr::Host host_;
    std::string playingGame_;
    int64_t playStart_ = 0;
    bool quit_ = false;
    std::string glslVersion_;
    GamepadMonitor pads_;
    bool padUi_ = false;                // the UI owns the gamepads (ImGui navigation, padActions)
    bool padModeSet_ = false;
    bool startClean_ = false;           // Start went down with neither Guide nor Back held
    std::map<uint32_t, std::array<bool, 2>> triggerDown_;  // LT / RT read as buttons
    bool slowLoop_ = false;             // 30 Hz while an external game has the focus
    std::string screenshotPath_;
};

App& app();

}  // namespace rrm
