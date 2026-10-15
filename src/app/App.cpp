#include "app/App.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

#include "app/Ui.h"
#include "core/BackendBundle.h"
#include "core/Net.h"
#include "core/Platform.h"
#include "core/Process.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

namespace rrm {

namespace {

// The few GL calls Marathon itself makes; loaded through SDL so no GL import library is needed.
struct Gl {
    void (*Viewport)(int, int, int, int) = nullptr;
    void (*ClearColor)(float, float, float, float) = nullptr;
    void (*Clear)(unsigned) = nullptr;
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void*) = nullptr;
    void (*BindFramebuffer)(unsigned, unsigned) = nullptr;
    void load() {
        Viewport = (decltype(Viewport))SDL_GL_GetProcAddress("glViewport");
        ClearColor = (decltype(ClearColor))SDL_GL_GetProcAddress("glClearColor");
        Clear = (decltype(Clear))SDL_GL_GetProcAddress("glClear");
        ReadPixels = (decltype(ReadPixels))SDL_GL_GetProcAddress("glReadPixels");
        BindFramebuffer = (decltype(BindFramebuffer))SDL_GL_GetProcAddress("glBindFramebuffer");
    }
} gl;
constexpr unsigned GL_COLOR_BUFFER_BIT_ = 0x4000;
constexpr unsigned GL_RGBA_ = 0x1908;
constexpr unsigned GL_UNSIGNED_BYTE_ = 0x1401;

int64_t unixNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

fs::path settingsPath() { return paths().data / "settings.json"; }

// Command-line options for automation and tests.
struct Options {
    std::string screenshot;   // save a PNG after `frames` frames and quit
    int frames = 90;
    bool waitForSetup = false;  // package smoke test waits for the offline import
    std::string scan;         // scan this folder at start-up
    std::string openGame;     // open a game page
    std::string playGame;     // launch a game
    std::string screen;       // library|add|backends|settings|controllers|about
    int width = 1280, height = 800;
} g_opts;

}  // namespace

App& app() {
    static App instance;
    return instance;
}

double App::now() const { return (double)SDL_GetTicksNS() / 1e9; }

bool App::init(int argc, char** argv) {
    std::string dataDir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--data") dataDir = next();
        else if (a == "--screenshot") g_opts.screenshot = next();
        else if (a == "--frames") g_opts.frames = std::max(2, std::atoi(next().c_str()));
        else if (a == "--wait-for-setup") g_opts.waitForSetup = true;
        else if (a == "--scan") g_opts.scan = next();
        else if (a == "--game") g_opts.openGame = next();
        else if (a == "--play") g_opts.playGame = next();
        else if (a == "--screen") g_opts.screen = next();
        else if (a == "--size") {
            std::string v = next();
            sscanf(v.c_str(), "%dx%d", &g_opts.width, &g_opts.height);
        }
    }

    SDL_SetAppMetadata("Ridge Racer Marathon", RRM_VERSION, "io.github.ridgeracermarathon");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    // The quit chord must work while another program has focus (docs/CONTROLLERS.md).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // PlayStation pads keep the report mode the running game expects unless Marathon itself rumbles them.
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "auto");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    initPaths(dataDir.empty() ? nullptr : dataDir.c_str());
    Os os = currentOs();
    touchUi = (os == Os::Android || os == Os::iOS);

    // OpenGL context: desktop core profile, GLES 3 on phones/tablets.
#if defined(RRM_GLES) || defined(__ANDROID__) || (defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    glslVersion_ = "#version 300 es";
#elif defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    glslVersion_ = "#version 150";
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    glslVersion_ = "#version 150";
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (touchUi) flags |= SDL_WINDOW_FULLSCREEN;
    window_ = SDL_CreateWindow("Ridge Racer Marathon", g_opts.width, g_opts.height, flags);
    if (!window_) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    gl_ = SDL_GL_CreateContext(window_);
    if (!gl_) {
        SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, gl_);
    SDL_GL_SetSwapInterval(1);
    gl.load();
    // Dear ImGui's SDL backend enables native text input only for an active
    // text field. Enabling it here also gives held gameplay keys to macOS IME.

    // Persistent state
    std::string error;
    if (!catalog.load(error)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ridge Racer Marathon", error.c_str(), window_);
        return false;
    }
    library.load();
    json gs;
    if (readJsonFile(settingsPath(), gs)) global = GlobalSettings::fromJson(gs);
    for (auto& def : catalog.backends()) {
        if (auto b = createBackend(def)) backends_[def.id] = std::move(b);
        else SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[catalog] %s: unknown adapter \"%s\"", def.id.c_str(),
                         def.adapter.c_str());  // (a typo in an extra catalog file)
    }

    updateDisplayInfo();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    static std::string iniPath = pathToUtf8(paths().data / "imgui.ini");
    io.IniFilename = iniPath.c_str();
    loadFonts();
    applyStyle();
    ImGui_ImplSDL3_InitForOpenGL(window_, gl_);
    ImGui_ImplOpenGL3_Init(glslVersion_.c_str());
    // Every pad drives the menus; updatePadRouting() hands them to games and back.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    padUi_ = padModeSet_ = true;
    pads_.open();

    installBundledBackends();

    if (!g_opts.scan.empty()) scanFolder(g_opts.scan);
    if (!g_opts.openGame.empty() && catalog.game(g_opts.openGame)) {
        selectedGame = g_opts.openGame;
        screen = Screen::Game;
    }
    if (g_opts.screen == "add") screen = Screen::AddGames;
    else if (g_opts.screen == "backends") screen = Screen::Backends;
    else if (g_opts.screen == "settings") screen = Screen::Settings;
    else if (g_opts.screen == "controllers") {
        screen = Screen::Settings;
        settingsPage = SettingsPage::Controllers;
    }
    else if (g_opts.screen == "about") screen = Screen::About;
    if (!g_opts.playGame.empty()) play(g_opts.playGame);
    SDL_Log("Ridge Racer Marathon %s on %s, data in %s", RRM_VERSION, currentPlatformKey().c_str(),
            pathToUtf8(paths().data).c_str());
    return true;
}

void App::shutdown() {
    if (host_.running()) exitPlayer();
    for (auto& j : jobs_) {
        if (j->progress) j->progress->cancel = true;
        if (j->thread.joinable()) j->thread.join();
    }
    // Closing during first-run setup must still remember every completed import.
    pollJobs();
    saveAll();
    pads_.close();
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (gl_) SDL_GL_DestroyContext(gl_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void App::saveAll() {
    library.save();
    writeJsonFile(settingsPath(), global.toJson());
}

void App::updateDisplayInfo() {
    SDL_DisplayID id = SDL_GetDisplayForWindow(window_);
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(id)) {
        display.width = (int)(m->w * m->pixel_density);
        display.height = (int)(m->h * m->pixel_density);
        display.refreshHz = m->refresh_rate > 1 ? (int)(m->refresh_rate + 0.5f) : 60;
    }
    display.dpiScale = SDL_GetWindowDisplayScale(window_);
    if (display.dpiScale <= 0) display.dpiScale = 1.0f;
    float scale = global.uiScale > 0 ? global.uiScale : (touchUi ? 1.25f : 1.0f);
    uiScale = scale;
}

Backend* App::backend(const std::string& id) {
    auto it = backends_.find(id);
    return it == backends_.end() ? nullptr : it->second.get();
}

GameSettings App::settingsFor(const std::string& gameId) const {
    const LibraryEntry* e = library.find(gameId);
    if (e && e->customSettings) return e->settings;
    GameSettings s = global.defaults;
    if (e) s.backend = e->settings.backend;  // backend choice is always per game
    return s;
}

ControllerSettings App::controllersFor(const std::string& gameId) const {
    return global.controllers.withOverrides(settingsFor(gameId).extra);
}

void App::setGameSettings(const std::string& gameId, const GameSettings& s, bool custom) {
    LibraryEntry& e = library.entry(gameId);
    e.settings = s;
    e.customSettings = custom;
    library.save();
}

const GameBackendOption* App::chosenOption(const GameDef& g) const {
    auto options = catalog.optionsFor(g, currentOs());
    if (options.empty()) return nullptr;
    GameSettings s = settingsFor(g.id);
    auto self = const_cast<App*>(this);
    if (s.backend != "auto")
        for (auto* o : options)
            if (o->backend == s.backend) return o;
    // Prefer an option that is ready; otherwise the best installable one.
    for (auto* o : options) {
        Backend* b = self->backend(o->backend);
        if (!b || !b->availableHere()) continue;
        auto st = b->installState(library);
        if (st == InstallState::Installed || st == InstallState::UserProvided) return o;
    }
    for (auto* o : options) {
        Backend* b = self->backend(o->backend);
        if (!b || !b->availableHere()) continue;
        const BackendSource* src = b->def().sourceFor(currentPlatformKey());
        if (b->def().kind == "libretro" || (src && src->kind != "user")) return o;
    }
    for (auto* o : options) {
        Backend* b = self->backend(o->backend);
        if (b && b->availableHere()) return o;
    }
    return options.front();
}

App::GameStatus App::statusOf(const GameDef& g) const {
    if (running_ && running_->gameId == g.id) return GameStatus::Running;
    if (playingGame_ == g.id && host_.running()) return GameStatus::Running;
    const GameBackendOption* o = chosenOption(g);
    auto self = const_cast<App*>(this);
    if (!o) return GameStatus::Unsupported;
    Backend* b = self->backend(o->backend);
    if (!b || !b->availableHere()) return GameStatus::Unsupported;
    const LibraryEntry* e = library.find(g.id);
    if (!e || !e->hasFiles()) return GameStatus::NeedsFiles;
    auto st = b->installState(library);
    if (st != InstallState::Installed && st != InstallState::UserProvided) return GameStatus::NeedsBackend;
    LaunchRequest req;
    req.game = &g;
    req.option = o;
    req.backend = &b->def();
    req.entry = e;
    req.library = &library;
    req.settings = settingsFor(g.id);
    req.display = display;
    req.controllers = controllersFor(g.id);
    for (auto& m : b->missingRequirements(req))
        if (!isRequirementWarning(m)) return GameStatus::NeedsSystemFiles;
    return GameStatus::Ready;
}

std::string App::statusText(const GameDef& g) const {
    switch (statusOf(g)) {
        case GameStatus::Ready: return "Ready";
        case GameStatus::Running: return "Running";
        case GameStatus::NeedsFiles: return "Add your game files";
        case GameStatus::NeedsBackend: return "Backend not installed";
        case GameStatus::NeedsSystemFiles: return "Needs BIOS / firmware";
        case GameStatus::Unsupported: return "Not available on this device";
    }
    return "";
}

// ---- launching -----------------------------------------------------------------------------

void App::play(const std::string& gameId) {
    if (Job* setup = job("bundle"); setup && !setup->finished) {
        toast("Setting up the included backends. Please wait for setup to finish.");
        return;
    }
    const GameDef* g = catalog.game(gameId);
    if (!g) return;
    if (running_ || host_.running()) {
        toast("Another game is already running.", true);
        return;
    }
    const GameBackendOption* o = chosenOption(*g);
    Backend* b = o ? backend(o->backend) : nullptr;
    if (!b || !b->availableHere()) {
        modalTitle = g->title;
        modalText = "There is no way to run " + g->title + " on this device yet.";
        return;
    }
    LaunchRequest req;
    req.game = g;
    req.option = o;
    req.backend = &b->def();
    req.entry = library.find(gameId);
    req.library = &library;
    req.settings = settingsFor(gameId);
    req.display = display;
    req.controllers = controllersFor(gameId);
    req.pads = padSnapshot();
    auto missing = b->missingRequirements(req);
    auto st = b->installState(library);
    if (st != InstallState::Installed && st != InstallState::UserProvided)
        missing.insert(missing.begin(), b->def().name + " is not installed yet. Install it on the game page or under Backends.");
    const bool blocked = std::any_of(missing.begin(), missing.end(), [](auto& m) { return !isRequirementWarning(m); });
    if (blocked) {
        modalTitle = "Before you can play " + g->title;
        modalText.clear();
        for (auto& m : missing) modalText += "- " + m + "\n";
        return;
    }
    // A quit chord still held from the last game must not close this one.
    pads_.resetChord();
    LaunchResult r = b->launch(req);
    for (auto& m : missing) {  // only warnings remain here
        const size_t text = m.find_first_not_of(' ', m.find(':') + 1);
        toast(text == std::string::npos ? m : m.substr(text));
    }
    if (!r.ok) {
        modalTitle = "Could not start " + g->title;
        modalText = r.error;
        return;
    }
    LibraryEntry& e = library.entry(gameId);
    e.lastPlayed = unixNow();
    library.save();
    if (r.mode == LaunchResult::Mode::InProcessCore) {
        startInProcess(r, req.settings, gameId);
    } else if (r.mode == LaunchResult::Mode::ExternalProcess) {
        RunningGame rg;
        rg.gameId = gameId;
        rg.backendId = b->def().id;
        rg.pid = r.pid;
        rg.startedAt = unixNow();
        rg.commandLine = r.commandLine;
        rg.logFile = pathToUtf8(paths().logs / (b->def().id + ".log"));
        running_ = rg;
        toast(g->title + " started in " + b->def().name);
    } else {
        std::string err;
        if (launchExternalApp(r.commandLine, err)) {
            toast(g->title + " was handed to " + b->def().name);
        } else {
            modalTitle = "Could not open " + b->def().name;
            modalText = err;
        }
    }
}

void App::startInProcess(const LaunchResult& r, const GameSettings& s, const std::string& gameId) {
    lr::HostConfig cfg;
    cfg.corePath = r.corePath;
    cfg.contentPath = r.contentPath;
    cfg.systemDir = r.systemDir;
    cfg.saveDir = r.saveDir;
    cfg.coreOptions = r.coreOptions.is_object() ? r.coreOptions : json::object();
    cfg.speedPercent = s.speed;
    cfg.turboPercent = s.turbo;
    cfg.aspect = r.presentAspect.empty() ? s.aspect : r.presentAspect;
    cfg.filter = s.filter == "linear" ? "linear" : "nearest";
    cfg.vsync = s.vsync;
    // In-app cores smooth above their rate: motion-compensated interpolation, or a crossfade.
    cfg.smoothing = resolveFps(s) == 0 ? 0 : s.fpsMethod == "blend" ? 1 : 2;
    cfg.smoothFps = std::max(0, resolveFps(s));
    std::error_code ec;
    if (!cfg.systemDir.empty()) fs::create_directories(utf8ToPath(cfg.systemDir), ec);
    if (!cfg.saveDir.empty()) fs::create_directories(utf8ToPath(cfg.saveDir), ec);
    std::string error;
    if (!host_.start(window_, cfg, error)) {
        modalTitle = "The emulator core could not start";
        modalText = error;
        return;
    }
    playingGame_ = gameId;
    selectedGame = gameId;
    playStart_ = unixNow();
    screen = Screen::Playing;
    if (s.fullscreen && !touchUi) SDL_SetWindowFullscreen(window_, true);
    ui::onPlayerStarted(*this);
}

void App::exitPlayer() {
    if (host_.running()) host_.stop();
    if (!playingGame_.empty()) {
        LibraryEntry& e = library.entry(playingGame_);
        e.playSeconds += std::max<int64_t>(0, unixNow() - playStart_);
        library.save();
    }
    if (!touchUi) SDL_SetWindowFullscreen(window_, false);
    screen = playingGame_.empty() ? Screen::Library : Screen::Game;
    selectedGame = playingGame_.empty() ? selectedGame : playingGame_;
    playingGame_.clear();
}

void App::stopRunning() { requestQuitRunning(false); }

void App::requestQuitRunning(bool fromChord) {
    if (!running_) return;
    const GameDef* g = catalog.game(running_->gameId);
    const std::string title = g ? g->title : running_->gameId;
    // As the window's close button would; asking again (a second Stop) is harmless.
    const bool asked = requestProcessExit(running_->pid);
    SDL_Log("[app] closing %s (%s)%s", title.c_str(), fromChord ? "quit chord" : "Stop", asked ? "" : ": no window to ask yet");
    if (running_->quitRequestedAt > 0) return;
    running_->quitRequestedAt = now();
    toast("Closing " + title + "...");
}

void App::raiseWindow() {
    if (!window_) return;
    if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(window_);
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
    SDL_RaiseWindow(window_);
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "0");
}

void App::pollRunning() {
    if (!running_) return;
    int code = 0;
    if (processRunning(running_->pid, &code)) {
        if (running_->quitRequestedAt > 0 && now() - running_->quitRequestedAt > 6.0 && !running_->forced) {
            killProcess(running_->pid);
            running_->forced = true;
            const GameDef* g = catalog.game(running_->gameId);
            toast((g ? g->title : running_->gameId) + " did not close in time, so Marathon stopped it.");
        }
        return;
    }
    raiseWindow();
    LibraryEntry& e = library.entry(running_->gameId);
    e.playSeconds += std::max<int64_t>(0, unixNow() - running_->startedAt);
    library.save();
    const GameDef* g = catalog.game(running_->gameId);
    if (code != 0 && unixNow() - running_->startedAt < 5 && running_->quitRequestedAt <= 0) {
        modalTitle = (g ? g->title : running_->gameId) + " stopped right away";
        modalText = "Exit code " + std::to_string(code) + ". The backend log is at:\n" + running_->logFile;
    } else {
        toast((g ? g->title : running_->gameId) + " closed");
    }
    running_.reset();
}

// ---- jobs ------------------------------------------------------------------------------------

Job* App::startJob(const std::string& id, const std::string& title, std::function<bool(TaskProgress&)> fn) {
    if (Job* existing = job(id); existing && !existing->finished) return existing;
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [&](const std::unique_ptr<Job>& j) {
                                   if (j->id != id) return false;
                                   if (j->thread.joinable()) j->thread.join();
                                   return true;
                               }),
                jobs_.end());
    auto j = std::make_unique<Job>();
    j->id = id;
    j->title = title;
    j->progress->running = true;
    Job* raw = j.get();
    std::shared_ptr<TaskProgress> progress = j->progress;
    j->thread = std::thread([raw, progress, fn] {
        bool ok = false;
        try {
            ok = fn(*progress);
        } catch (const std::exception& e) {
            progress->fail(e.what());
        }
        raw->ok = ok && !progress->failed;
        progress->running = false;
    });
    jobs_.push_back(std::move(j));
    return raw;
}

Job* App::job(const std::string& id) {
    for (auto& j : jobs_)
        if (j->id == id) return j.get();
    return nullptr;
}

void App::pollJobs() {
    for (auto& j : jobs_) {
        if (j->finished || j->progress->running) continue;
        if (j->thread.joinable()) j->thread.join();
        j->finished = true;
        std::string err;
        {
            std::lock_guard<std::mutex> lock(j->progress->mutex);
            err = j->progress->error;
        }
        if (j->ok) toast(j->title + " - done");
        else toast(j->title + " failed: " + err, true);
        if (j->scratch && j->ok && !j->backendId.empty()) {
            if (const BackendInstall* bi = j->scratch->install(j->backendId)) library.setInstall(*bi);
            // Some backends also register system files (e.g. installed firmware).
            library.save();
        }
        if (j->scratch && j->importedBackends) {
            // A cancelled or partially failed import still preserves completed backends.
            for (const auto& id : *j->importedBackends) {
                // A scan or Link existing action may have supplied an installation
                // after the worker took its snapshot. Preserve that newer choice.
                std::error_code ec;
                if (const auto* current = library.install(id); current &&
                    (current->userProvided || (!current->executable.empty() && fs::is_regular_file(utf8ToPath(current->executable), ec))))
                    continue;
                if (const auto* installed = j->scratch->install(id)) library.setInstall(*installed);
            }
            library.save();
            if (!j->ok && !err.empty()) {
                modalTitle = "Complete package setup needs attention";
                modalText = err + "\n\nCompleted backends are ready. Restart Marathon to retry, or use Backends > Install all.";
            }
        }
    }
}

std::vector<Backend*> App::missingDownloadableBackends() {
    std::vector<Backend*> out;
    for (auto& def : catalog.backends()) {
        Backend* b = backend(def.id);
        if (!b || !b->availableHere() || b->installState(library) != InstallState::NotInstalled) continue;
        const BackendSource* src = def.sourceFor(currentPlatformKey());
        const bool downloadable = def.kind == "libretro" ? isDesktop() : (src && src->kind != "user" && src->kind != "external-app");
        if (downloadable) out.push_back(b);
    }
    return out;
}

void App::installAllBackends() {
    if (Job* setup = job("bundle"); setup && !setup->finished) {
        toast("Setting up the included backends. Please wait for setup to finish.");
        return;
    }
    for (Backend* b : missingDownloadableBackends()) installBackend(b->def().id);
}

void App::installBundledBackends() {
    const auto directory = backendBundleDirectory();
    if (directory.empty() || !fs::exists(directory / "manifest.json")) return;
    auto scratch = std::make_shared<Library>(library);
    auto imported = std::make_shared<std::vector<std::string>>();
    Job* j = startJob("bundle", "Setting up included backends",
                      [this, scratch, imported, directory](TaskProgress& p) {
        auto report = importBackendBundle(catalog, *scratch, p, false, directory);
        *imported = std::move(report.imported);
        return report.ok();
    });
    j->scratch = scratch;
    j->importedBackends = imported;
}

void App::installBackend(const std::string& backendId) {
    if (Job* setup = job("bundle"); setup && !setup->finished) {
        toast("Setting up the included backends. Please wait for setup to finish.");
        return;
    }
    Backend* b = backend(backendId);
    if (!b) return;
    if (Job* running = job("install:" + backendId); running && !running->finished) return;
    // The worker installs into a private copy; pollJobs() merges the new record on the main thread.
    auto scratch = std::make_shared<Library>(library);
    Job* j = startJob("install:" + backendId, "Installing " + b->def().name,
                      [b, scratch](TaskProgress& p) { return b->install(*scratch, p); });
    j->scratch = scratch;
    j->backendId = backendId;
}

void App::scanFolder(const std::string& folder) {
    if (folder.empty()) return;
    global.lastScanFolder = folder;
    if (std::find(library.scanFolders.begin(), library.scanFolders.end(), folder) == library.scanFolders.end())
        library.scanFolders.push_back(folder);
    library.save();
    // Backend executables to look for (e.g. a recomp the user already built), resolved on the main thread:
    // backend id, file pattern and a copy of the source (its marker decides which builds count).
    struct Wanted {
        std::string id, pattern;
        BackendSource src;
    };
    std::vector<Wanted> patterns;
    if (isDesktop()) {
        for (auto& def : catalog.backends()) {
            if (def.kind == "libretro") continue;
            const BackendSource* src = def.sourceFor(currentPlatformKey());
            if (!src || src->executable.empty()) continue;
            std::string pat = src->executable;
            if (pat.rfind("*", 0) != 0) pat = "*" + pat;
            patterns.push_back({def.id, pat, *src});
        }
    }
    startJob("scan", "Scanning " + folder, [this, folder, patterns](TaskProgress& p) {
        p.setStage("Scanning " + folder);
        ScanProgress sp;
        auto hits = rrm::scanFolder(catalog, folder, &sp);
        std::vector<std::pair<std::string, std::string>> installs;
        for (auto& w : patterns) {
            if (p.cancel) break;
            // A source with a marker (a local build) is only offered for a build carrying it.
            const BackendSource* src = &w.src;  // (MSVC cannot capture the local struct itself here)
            std::string exe = findInTree(folder, w.pattern, 5, [src](const std::string& e) { return src->accepts(e); });
            if (!exe.empty()) installs.push_back({w.id, exe});
        }
        p.setStage("Found " + std::to_string(hits.size()) + " game file(s)" +
                   (installs.empty() ? std::string() : " and " + std::to_string(installs.size()) + " existing install(s)"));
        std::lock_guard<std::mutex> lock(scanMutex);
        lastScanHits = hits;
        foundInstalls = installs;
        return true;
    });
}

void App::acceptHit(const ScanHit& hit) {
    if (hit.gameId.empty()) return;
    LibraryEntry& e = library.entry(hit.gameId);
    e.files = hit.files;
    if (!e.files.addedAt) e.files.addedAt = unixNow();
    library.save();
}

void App::verifyGame(const std::string& gameId) {
    const LibraryEntry* e = library.find(gameId);
    if (!e || !e->hasFiles()) return;
    ScanHit hit;
    hit.gameId = gameId;
    hit.files = e->files;
    startJob("verify:" + gameId, "Verifying " + gameId, [this, hit](TaskProgress& p) mutable {
        p.setStage("Hashing files");
        std::atomic<bool>* cancel = &p.cancel;
        verifyHit(catalog, hit, cancel, [&p](uint64_t done, uint64_t total) {
            p.done = done;
            p.total = total;
        });
        std::lock_guard<std::mutex> lock(scanMutex);
        lastScanHits.push_back(hit);  // picked up by the main thread below
        p.setStage("Result: " + (hit.files.verified.empty() ? std::string("unknown") : hit.files.verified));
        return hit.files.verified.rfind("match", 0) == 0 || hit.files.verified == "partial";
    });
}

// ---- dialogs -----------------------------------------------------------------------------------

namespace {
struct DialogCtx {
    App* app;
    std::string purpose;
};
void SDLCALL dialogCallback(void* userdata, const char* const* filelist, int) {
    std::unique_ptr<DialogCtx> ctx(static_cast<DialogCtx*>(userdata));
    if (!filelist) return;
    DialogResult r;
    r.purpose = ctx->purpose;
    for (const char* const* f = filelist; *f; ++f) r.paths.push_back(*f);
    if (r.paths.empty()) return;
    ui::deliverDialog(*ctx->app, std::move(r));
}
}  // namespace

void App::pickFolder(const std::string& purpose) {
    auto* ctx = new DialogCtx{this, purpose};
    std::string start = global.lastScanFolder.empty() ? pathToUtf8(paths().userFiles) : global.lastScanFolder;
    SDL_ShowOpenFolderDialog(dialogCallback, ctx, window_, start.c_str(), false);
}

void App::pickFile(const std::string& purpose, const std::vector<std::pair<std::string, std::string>>& filters) {
    auto* ctx = new DialogCtx{this, purpose};
    static std::vector<std::string> storage;  // SDL keeps pointers until the dialog closes
    static std::vector<SDL_DialogFileFilter> sdlFilters;
    storage.clear();
    sdlFilters.clear();
    storage.reserve(filters.size() * 2);
    for (auto& [name, pattern] : filters) {
        storage.push_back(name);
        storage.push_back(pattern);
    }
    for (size_t i = 0; i < filters.size(); ++i) sdlFilters.push_back({storage[i * 2].c_str(), storage[i * 2 + 1].c_str()});
    SDL_ShowOpenFileDialog(dialogCallback, ctx, window_, sdlFilters.empty() ? nullptr : sdlFilters.data(),
                           (int)sdlFilters.size(), nullptr, false);
}

void App::handleDialogResults() {
    std::vector<DialogResult> results = ui::takeDialogs(*this);
    for (auto& r : results) ui::onDialogResult(*this, r);
}

void App::toast(const std::string& text, bool error) {
    toasts.push_back({text, error, now() + (error ? 8.0 : 4.0)});
    if (toasts.size() > 5) toasts.erase(toasts.begin());
}

// ---- controllers ---------------------------------------------------------------------------------

void App::handlePads() {
    for (const PadEvent& ev : pads_.update(SDL_GetTicks(), global.controllers)) {
        switch (ev.kind) {
            case PadEvent::Kind::Connected:
                toast("Controller connected: " + ev.pad.name +
                      (ev.pad.player >= 0 ? " (Player " + std::to_string(ev.pad.player + 1) + ")" : std::string()));
                break;
            case PadEvent::Kind::Disconnected: toast("Controller disconnected: " + ev.pad.name); break;
            case PadEvent::Kind::QuitChord:
                if (running_) requestQuitRunning(true);
                else if (screen == Screen::Playing) exitPlayer();
                break;
        }
    }
    updatePadRouting();
    // "The button labelled A confirms": Nintendo pads have B at the bottom.
    if (uint32_t id = pads_.lastUsed())
        ImGui::GetIO().ConfigNavSwapGamepadButtons =
            SDL_GetGamepadButtonLabelForType(SDL_GetGamepadTypeForID(id), SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_B;
    // While an external game has the focus Marathon only watches the pads: on macOS the swap of a
    // hidden window does not block, so without this it would draw as fast as it can.
    const bool focused = (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool slow = running_ && !focused;
    if (slow != slowLoop_) {
        SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, slow ? "30" : "0");
        slowLoop_ = slow;
    }
}

// The menus own the pads while Marathon's window has the focus and no game uses them
// (an external game, the in-app player without its pause menu, the Controllers page test).
void App::updatePadRouting() {
    const bool focused = assumeFocus || (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool ui = focused && !running_ && !padTestActive && (screen != Screen::Playing || ui::playerMenuOpen());
    if (ui == padUi_ && padModeSet_) return;
    if (ui) ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    else ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
    padUi_ = ui;
    padModeSet_ = true;
    startClean_ = false;
    triggerDown_.clear();
}

// Gamepad buttons the UI acts on itself (ImGui handles moving and confirming). docs/CONTROLLERS.md
void App::padButtonEvent(const SDL_Event& e) {
    using ui::PadAction;
    if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        // LT / RT as buttons: pressed past 50 %, released below 25 %.
        if (e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && e.gaxis.axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return;
        const bool right = e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        bool& down = triggerDown_[e.gaxis.which][right ? 1 : 0];
        if (!down && e.gaxis.value > 16384) {
            down = true;
            padActions.push_back(right ? PadAction::PageNext : PadAction::PagePrev);
        } else if (down && e.gaxis.value < 8192) {
            down = false;
        }
        return;
    }
    if (e.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.type != SDL_EVENT_GAMEPAD_BUTTON_UP) return;
    SDL_Gamepad* pad = SDL_GetGamepadFromID(e.gbutton.which);
    const auto button = static_cast<SDL_GamepadButton>(e.gbutton.button);
    const bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    auto held = [pad](SDL_GamepadButton b) { return pad && SDL_GetGamepadButton(pad, b); };
    // Start alone (released without Guide or Back): Guide + Start is the quit chord.
    if (button == SDL_GAMEPAD_BUTTON_START) {
        if (down) startClean_ = !held(SDL_GAMEPAD_BUTTON_GUIDE) && !held(SDL_GAMEPAD_BUTTON_BACK);
        else if (startClean_) padActions.push_back(PadAction::Play);
        if (!down) startClean_ = false;
        return;
    }
    if (!down) return;
    if (button == SDL_GAMEPAD_BUTTON_GUIDE || button == SDL_GAMEPAD_BUTTON_BACK) {
        startClean_ = false;
        return;
    }
    if (button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) padActions.push_back(PadAction::TabPrev);
    else if (button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) padActions.push_back(PadAction::TabNext);
    else {
        // By label: B / Circle goes back, Y / Triangle opens a game's settings (on Nintendo pads
        // those are the bottom and left buttons).
        const SDL_GamepadButtonLabel label = pad ? SDL_GetGamepadButtonLabel(pad, button) : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
        const bool unknown = label == SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
        if (label == SDL_GAMEPAD_BUTTON_LABEL_B || label == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE ||
            (unknown && button == SDL_GAMEPAD_BUTTON_EAST))
            padActions.push_back(PadAction::Back);
        else if (label == SDL_GAMEPAD_BUTTON_LABEL_Y || label == SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE ||
                 (unknown && button == SDL_GAMEPAD_BUTTON_NORTH))
            padActions.push_back(PadAction::GameSettings);
    }
}

// ---- main loop -----------------------------------------------------------------------------------

bool App::handleEvent(const SDL_Event& e) {
    pads_.handleEvent(e);
    switch (e.type) {
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: lastInputPad = true; break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (e.gaxis.value > 16384 || e.gaxis.value < -16384) lastInputPad = true;
            break;
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_FINGER_DOWN: lastInputPad = false; break;
        default: break;
    }
    if (padUi_) padButtonEvent(e);
    if (e.type == SDL_EVENT_QUIT) return false;
    if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window_)) return false;
    if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || e.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED ||
        e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED)
        updateDisplayInfo();
    if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
        std::string path = e.drop.data;
        ScanHit hit;
        if (identifyPath(catalog, path, hit) && !hit.gameId.empty()) {
            acceptHit(hit);
            const GameDef* g = catalog.game(hit.gameId);
            toast("Added " + (g ? g->title : hit.gameId));
        } else if (std::string kind = identifySystemFile(path); !kind.empty()) {
            library.setSystemFile(kind, path);
            library.save();
            toast("Registered " + kind);
        } else {
            std::error_code ec;
            if (fs::is_directory(utf8ToPath(path), ec)) scanFolder(path);
            else toast("Not a recognised Ridge Racer file: " + path, true);
        }
    }
    if (screen == Screen::Playing) {
        if (ui::playerHandleEvent(*this, e)) return true;
        if (!ui::playerMenuOpen() && host_.handleEvent(e)) return true;
    }
    ImGui_ImplSDL3_ProcessEvent(&e);
    return !quit_;
}

bool App::iterate() {
    pollRunning();
    pollJobs();
    handleDialogResults();
    handlePads();
    {
        // Verification results come back through lastScanHits as well.
        std::lock_guard<std::mutex> lock(scanMutex);
        for (auto& h : lastScanHits) {
            const LibraryEntry* e = library.find(h.gameId);
            if (e && e->files.path == h.files.path && !h.files.verified.empty() && e->files.verified != h.files.verified) {
                library.entry(h.gameId).files.verified = h.files.verified;
                library.save();
            }
        }
    }

    bool playing = screen == Screen::Playing && host_.running();
    if (playing && !host_.runFrame()) {
        exitPlayer();
        playing = false;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ui::draw(*this);
    ImGui::Render();

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    gl.Viewport(0, 0, w, h);
    gl.ClearColor(0.035f, 0.043f, 0.063f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT_);
    if (playing) ui::drawGameView(*this, w, h);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    auto savePng = [&](const std::string& path) {
        std::vector<uint8_t> pixels((size_t)w * h * 4);
        gl.ReadPixels(0, 0, w, h, GL_RGBA_, GL_UNSIGNED_BYTE_, pixels.data());
        SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
        if (s) {
            for (int y = 0; y < h; ++y)
                std::memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch, pixels.data() + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
            if (!SDL_SavePNG(s, path.c_str())) SDL_Log("screenshot failed: %s", SDL_GetError());
            SDL_DestroySurface(s);
        }
    };
    if (!screenshotPath_.empty()) {
        savePng(screenshotPath_);
        screenshotPath_.clear();
    }
    static int frameCount = 0;
    ++frameCount;
    const Job* setup = job("bundle");
    const bool setupReady = !g_opts.waitForSetup || !setup || setup->finished;
    if (!g_opts.screenshot.empty() && frameCount >= g_opts.frames && setupReady) {
        savePng(g_opts.screenshot);
        SDL_GL_SwapWindow(window_);
        return false;
    }
    SDL_GL_SwapWindow(window_);
    return !quit_;
}

}  // namespace rrm
