// PSXRecomp static recompilations:
//   rr1-recomp  Ridge Racer (USA)  RobGreenUK/Ridge-Racer-PS1-Recomp  (source only: a local build is linked)
//   r4-recomp   R4 (USA)           tetrisgm/RidgeRacerType4Recomp     (setup kit: upstream's wizard builds it)
//                                  omegakatana92/RidgeRacer4Recomp    (Linux AppImage)
//
// Every PSXRecomp runtime reads settings.toml beside its executable (CLI wins
// over it) and mods/state.toml for the per-title enhancement mods. Marathon
// changes only the keys it owns, passes --game/--disc/--no-launcher and keeps
// the memory cards in Marathon's own save folder (--memcard-dir), importing the
// build's existing cards once, so they survive rebuilds and updates.
//
// Ridge Racer's experimental native-scene mode ("redraw") is two processes:
// RidgeScenePreview draws the course natively and owns the window, while a
// hidden copy of the game (the "companion") runs the original simulation and
// audio and streams the scene over a local socket. Marathon starts the viewer,
// waits for its socket, starts the companion and ties their lifetimes together
// on a small supervisor thread (see SceneSession), like upstream's
// launcher/native_scene.py and launcher/windows/Launch.ps1.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#define TOML_EXCEPTIONS 0
#define TOML_ENABLE_FORMATTERS 1
#include "toml.hpp"

#include "backends/Adapters.h"
#include "core/ConfigFiles.h"
#include "core/Platform.h"
#include "core/Process.h"

namespace rrm {

namespace {

// ---- small helpers ------------------------------------------------------------------

bool dryRun() {
    const char* e = std::getenv("RRM_RECOMP_DRY_RUN");
    return e && *e && *e != '0';
}

std::string timeStamp() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

std::string joinCommand(const std::vector<std::string>& argv) {
    std::string out;
    for (auto& a : argv) {
        if (!out.empty()) out += ' ';
        bool plain = !a.empty() && a.find_first_of(" \t\"'\\") == std::string::npos;
        out += plain ? a : "\"" + a + "\"";
    }
    return out;
}

void appendLog(const std::string& logFile, const std::vector<std::string>& lines) {
    std::ofstream out(utf8ToPath(logFile), std::ios::app);
    for (auto& l : lines) out << "[marathon] " << l << "\n";
}

void logSpec(const ProcessSpec& spec, std::vector<std::string>& notes) {
    notes.push_back("command: " + joinCommand(spec.argv));
    if (!spec.workingDir.empty()) notes.push_back("cwd: " + spec.workingDir);
    for (auto& [k, v] : spec.env) notes.push_back("env: " + k + "=" + v);
}

LaunchResult spawnLogged(const ProcessSpec& spec, std::vector<std::string> notes) {
    LaunchResult r;
    r.mode = LaunchResult::Mode::ExternalProcess;
    r.commandLine = spec.argv;
    notes.insert(notes.begin(), "==== launch " + timeStamp() + (dryRun() ? " (dry run)" : ""));
    logSpec(spec, notes);
    appendLog(spec.logFile, notes);
    if (dryRun()) {
        r.ok = true;
        return r;
    }
    std::string error;
    r.pid = spawnProcess(spec, error);
    if (r.pid <= 0) {
        r.error = error.empty() ? "Could not start " + spec.argv[0] : error;
        appendLog(spec.logFile, {"failed: " + r.error});
        return r;
    }
    r.ok = true;
    return r;
}

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool isDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

std::string exeName(const std::string& base) {
#if defined(_WIN32)
    return base + ".exe";
#else
    return base;
#endif
}

// TOML basic string literal.
std::string tomlStr(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

std::string tomlBool(bool b) { return b ? "true" : "false"; }

// Adds the owner's execute bit to a program that lost it (archives and copies
// made without permissions). Returns false if the file still cannot be run.
bool ensureExecutable(const fs::path& p, std::vector<std::string>& notes) {
#if defined(_WIN32)
    (void)notes;
    return isFile(p);
#else
    std::string s = pathToUtf8(p);
    if (access(s.c_str(), X_OK) == 0) return true;
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_exec | fs::perms::group_exec, fs::perm_options::add, ec);
    bool ok = access(s.c_str(), X_OK) == 0;
    notes.push_back(std::string(ok ? "made executable: " : "NOT executable (chmod failed): ") + s);
    return ok;
#endif
}

// Reads a TOML document for inspection (nullopt when missing or invalid).
std::optional<toml::table> readToml(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    toml::parse_result r = toml::parse(ss.str());
    if (!r) return std::nullopt;
    return std::move(r).table();
}

bool writeTextAtomic(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".marathon-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }
    return !ec;
}

// Unified resolution -> output lines (for the native-scene renderer).
int targetLines(const std::string& res, int displayHeight) {
    if (res == "native") return 240;
    if (res.size() >= 2 && res.back() == 'x') return 240 * std::max(1, std::atoi(res.c_str()));
    if (res == "720p") return 720;
    if (res == "1080p") return 1080;
    if (res == "1440p") return 1440;
    if (res == "4k") return 2160;
    if (res == "5k") return 2880;
    if (res == "8k") return 4320;
    if (res == "display") return displayHeight > 0 ? displayHeight : 1080;
    return 1080;
}

// Unified resolution -> the internal_resolution presets of newer PSXRecomp runtimes.
std::string resolutionPreset(const std::string& res, int scale) {
    if (res == "native" || res == "720p" || res == "1080p" || res == "1440p" || res == "4k" || res == "5k" ||
        res == "8k" || res == "display")
        return res;
    // Multipliers: the smallest preset at least as sharp (720p = 3x, 1080p = 5x, 1440p = 6x, 4k = 9x).
    if (scale <= 1) return "native";
    if (scale <= 3) return "720p";
    if (scale <= 5) return "1080p";
    if (scale <= 6) return "1440p";
    if (scale <= 9) return "4k";
    return "5k";
}

// ---- mods/state.toml (format 2) ----------------------------------------------------------

struct ModChange {
    std::string package;   // "ridge.presentation"
    std::string feature;   // "view"
    bool enabled = false;
    std::vector<std::pair<std::string, std::string>> values;  // option id -> canonical text
};

// Highest version folder of a bundled/installed package ("" if absent).
std::string packageVersion(const fs::path& modsDir, const std::string& package) {
    std::string best;
    for (const char* sub : {"bundled", "installed"}) {
        std::error_code ec;
        fs::path dir = modsDir / sub / package;
        if (!isDir(dir)) continue;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_directory(ec)) continue;
            std::string v = pathToUtf8(e.path().filename());
            if (isFile(e.path() / "manifest.toml") && (best.empty() || v > best)) best = v;
        }
    }
    return best;
}

bool applyModState(const fs::path& modsDir, const std::vector<ModChange>& changes, std::vector<std::string>& notes) {
    if (changes.empty()) return true;
    const fs::path stateFile = modsDir / "state.toml";
    toml::table doc;
    if (isFile(stateFile)) {
        auto parsed = readToml(stateFile);
        if (!parsed) {
            notes.push_back("mods/state.toml is not valid TOML; mod options left unchanged");
            return false;
        }
        doc = std::move(*parsed);
        if (auto fv = doc["format_version"].value<int64_t>(); fv && *fv != 2) {
            notes.push_back("mods/state.toml uses format " + std::to_string(*fv) +
                            "; open the game's own Mods page once to migrate it. Mod options left unchanged.");
            return false;
        }
    }
    doc.insert_or_assign("format_version", 2);
    auto arrayOf = [&](const char* key) -> toml::array* {
        if (!doc[key].is_array()) doc.insert_or_assign(key, toml::array{});
        return doc[key].as_array();
    };
    toml::array* packages = arrayOf("package");
    toml::array* features = arrayOf("feature");
    auto inlineStyle = [](toml::array* a) {
        for (auto& n : *a)
            if (auto* t = n.as_table()) return t->is_inline();
        return false;
    };
    const bool pkgInline = inlineStyle(packages), featInline = inlineStyle(features);
    bool changed = false;
    for (auto& c : changes) {
        std::string version = packageVersion(modsDir, c.package);
        if (version.empty()) {
            notes.push_back("mod " + c.package + " is not part of this build; " + c.feature + " not applied");
            continue;
        }
        for (auto it = packages->begin(); it != packages->end();) {
            auto* t = it->as_table();
            if (t && (*t)["id"].value_or(std::string()) == c.package) it = packages->erase(it);
            else ++it;
        }
        toml::table pkg;
        pkg.insert("id", c.package);
        pkg.insert("version", version);
        pkg.is_inline(pkgInline);
        packages->push_back(std::move(pkg));

        // Option values Marathon does not manage (e.g. R4's frame-rate "blend") are kept.
        toml::table values;
        for (auto it = features->begin(); it != features->end();) {
            auto* t = it->as_table();
            if (t && (*t)["package_id"].value_or(std::string()) == c.package &&
                (*t)["id"].value_or(std::string()) == c.feature) {
                if (auto* old = (*t)["values"].as_table()) values = *old;
                it = features->erase(it);
            } else {
                ++it;
            }
        }
        toml::table feat;
        feat.insert("package_id", c.package);
        feat.insert("id", c.feature);
        feat.insert("enabled", c.enabled);
        for (auto& [k, v] : c.values) values.insert_or_assign(k, v);
        values.is_inline(featInline);
        feat.insert("values", std::move(values));
        feat.is_inline(featInline);
        features->push_back(std::move(feat));

        std::string desc = c.package + "." + c.feature + " = " + (c.enabled ? "on" : "off");
        for (auto& [k, v] : c.values) desc += " " + k + "=" + v;
        notes.push_back("mod " + desc);
        changed = true;
    }
    if (!changed || dryRun()) return true;
    std::ostringstream out;
    out << "# Saved by Ridge Racer Marathon (mod options it manages); other entries are kept.\n" << doc << "\n";
    if (!writeTextAtomic(stateFile, out.str())) {
        notes.push_back("could not write " + pathToUtf8(stateFile));
        return false;
    }
    return true;
}

// ---- layout of an installed / linked build -----------------------------------------

enum class Flavor { Rr1, R4 };

struct Layout {
    fs::path root;         // install or linked folder
    fs::path exe;          // the game runtime (product)
    fs::path buildDir;     // folder of exe: settings.toml, mods/, bios/, lib/
    fs::path gameToml;
    fs::path setupHost;    // R4 kit: the zip-root program that runs upstream's setup wizard
    bool needsSetup = false;  // R4 kit not built yet
    bool appImage = false;    // R4 Linux AppImage (omegakatana92)
    fs::path viewer;          // RR1 native scene: RidgeScenePreview
    fs::path sceneAssets;     // RR1 native scene: native-scene/course.rrassets
    fs::path transportCheck;  // RR1 Windows: RidgeTransportCheck.exe
    std::vector<fs::path> legacySaves;  // folders whose card1/card2.mcd are imported once
    bool nativeSceneReady() const { return isFile(viewer) && isFile(sceneAssets); }
};

fs::path findGameToml(const fs::path& from, int levels) {
    fs::path p = from;
    for (int i = 0; i <= levels && !p.empty(); ++i) {
        if (isFile(p / "game.toml")) return p / "game.toml";
        if (p == p.parent_path()) break;
        p = p.parent_path();
    }
    return {};
}

std::optional<Layout> resolveLayout(Flavor flavor, const BackendInstall& bi) {
    if (bi.executable.empty()) return std::nullopt;
    Layout l;
    l.root = utf8ToPath(bi.root);
    fs::path exe = utf8ToPath(bi.executable);
    if (!isFile(exe)) return std::nullopt;
    if (flavor == Flavor::Rr1) {
        l.exe = exe;
        l.buildDir = exe.parent_path();
        l.gameToml = findGameToml(l.buildDir, 2);
        l.viewer = l.buildDir / exeName("RidgeScenePreview");
        l.sceneAssets = l.buildDir / "native-scene" / "course.rrassets";
        l.transportCheck = l.buildDir / "RidgeTransportCheck.exe";
        l.legacySaves = {l.root / "saves", l.buildDir / "saves"};
        return l;
    }
    std::string name = pathToUtf8(exe.filename());
    std::string lower = name;
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower.size() > 9 && lower.compare(lower.size() - 9, 9, ".appimage") == 0) {
        l.appImage = true;
        l.exe = exe;
        l.buildDir = exe.parent_path();
        return l;
    }
    // Setup kit: the zip-root exe is the wizard host; after "Generate & rebuild"
    // the playable product lives in build-release/ beside it.
    fs::path product = exe.parent_path() / "build-release" / exe.filename();
    const bool kit = isFile(exe.parent_path() / "psxrecomp" / "psxrecomp_cli.py") ||
                     isFile(exe.parent_path() / "README-SETUP.txt") || isDir(exe.parent_path() / "build-release");
    if (kit) {
        l.setupHost = exe;
        if (isFile(product)) {
            l.exe = product;
            l.buildDir = product.parent_path();
        } else {
            l.needsSetup = true;
            l.exe = exe;
            l.buildDir = exe.parent_path();
        }
        l.gameToml = isFile(l.buildDir / "game.toml") ? l.buildDir / "game.toml" : findGameToml(exe.parent_path(), 0);
        l.legacySaves = {l.buildDir / "saves", exe.parent_path() / "saves"};
        return l;
    }
    // A flat product folder (a packaged build: r4-runtime + settings.toml + mods/).
    l.exe = exe;
    l.buildDir = exe.parent_path();
    l.gameToml = findGameToml(l.buildDir, 1);
    l.legacySaves = {l.buildDir / "saves"};
    return l;
}

// ---- saves ----------------------------------------------------------------------------

fs::path saveDir(const LaunchRequest& req, const std::string& backendId) {
    return paths().saves / utf8ToPath(req.game ? req.game->id : std::string("unknown")) / utf8ToPath(backendId);
}

// The first time Marathon runs a linked build, its existing memory cards move
// with it (copied, never moved) so the player keeps their progress.
void importCards(const fs::path& dest, const std::vector<fs::path>& from, std::vector<std::string>& notes) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (isFile(dest / "card1.mcd") || isFile(dest / "card2.mcd")) return;
    for (auto& dir : from) {
        if (!isFile(dir / "card1.mcd") && !isFile(dir / "card2.mcd")) continue;
        for (const char* card : {"card1.mcd", "card2.mcd"}) {
            if (!isFile(dir / card)) continue;
            fs::copy_file(dir / card, dest / card, fs::copy_options::skip_existing, ec);
            notes.push_back(std::string(ec ? "could not import " : "imported memory card ") + pathToUtf8(dir / card));
        }
        return;
    }
}

// ---- native scene session (RR1 redraw) ----------------------------------------------------

struct SceneSession {
    ProcessSpec viewer, game;
    std::string socket;     // filled-in when the viewer is ready
    fs::path stage;         // temporary companion folder (removed afterwards); may be empty
    std::string logFile;
};

void removeStage(const fs::path& stage) {
    if (stage.empty()) return;
    std::error_code ec;
    fs::remove_all(stage, ec);
}

// Runs on a detached thread: waits for the viewer's socket, starts the hidden
// companion, then ends whichever process is still running when the other exits.
void superviseScene(std::shared_ptr<SceneSession> s, int64_t viewerPid) {
    using namespace std::chrono_literals;
    auto log = [&](const std::string& line) { appendLog(s->logFile, {line}); };
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    std::error_code ec;
    while (!fs::exists(utf8ToPath(s->socket), ec)) {
        if (!processRunning(viewerPid)) {
            log("native renderer exited before it was ready (see above)");
            removeStage(s->stage);
            return;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            log("native renderer did not open its scene socket within 15 s; stopping it");
            terminateProcess(viewerPid);
            removeStage(s->stage);
            return;
        }
        std::this_thread::sleep_for(20ms);
    }
    std::string error;
    int64_t gamePid = spawnProcess(s->game, error);
    if (gamePid <= 0) {
        log("could not start the game companion: " + error);
        terminateProcess(viewerPid);
        removeStage(s->stage);
        return;
    }
    log("game companion started (pid " + std::to_string(gamePid) + ")");
    int code = 0;
    while (true) {
        if (!processRunning(viewerPid, &code)) {
            log("native renderer closed (" + std::to_string(code) + "); stopping the game companion");
            terminateProcess(gamePid);
            break;
        }
        if (!processRunning(gamePid, &code)) {
            log("game companion exited (" + std::to_string(code) + "); closing the native renderer");
            terminateProcess(viewerPid);
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    for (int i = 0; i < 60 && (processRunning(viewerPid) || processRunning(gamePid)); ++i)
        std::this_thread::sleep_for(100ms);
    removeStage(s->stage);
}

#if !defined(_WIN32)
// Short private folder for the companion copy and the socket (sockaddr_un paths
// are limited to ~104 bytes, so it lives directly under /tmp like upstream's).
fs::path makeStageDir(std::string& error) {
    char tmpl[] = "/tmp/rrm-scene-XXXXXX";
    if (!mkdtemp(tmpl)) {
        error = "cannot create a temporary folder in /tmp";
        return {};
    }
    return fs::path(tmpl);
}
#endif

// ---- the backend ----------------------------------------------------------------------------

class PsxRecomp : public Backend {
public:
    explicit PsxRecomp(const BackendDef& def)
        : Backend(def), flavor_(def.adapter == "rr1-recomp" || jget<std::string>(def.params, "flavor", "") == "rr1"
                                    ? Flavor::Rr1
                                    : Flavor::R4) {}

    bool linkExisting(Library& lib, const std::string& path, std::string& error) override {
        std::error_code ec;
        fs::path p = fs::absolute(utf8ToPath(path), ec).lexically_normal();
        if (!fs::exists(p, ec)) {
            error = "Not found: " + path;
            return false;
        }
        fs::path exe, root;
        if (flavor_ == Flavor::Rr1) {
            if (!findRr1(p, exe, root)) {
                error = "No Ridge Racer recomp build found in " + path +
                        ". Choose the folder that contains game.toml and build-macos/RidgeRacer_Recompiled "
                        "(Windows: the dist folder with RidgeRacer_Recompiled.exe).";
                return false;
            }
        } else {
            if (!findR4(p, exe, root)) {
                error = "No R4 recomp found in " + path + ". Choose the folder that contains " + exeName("r4-runtime") +
                        " (or the AppImage).";
                return false;
            }
        }
        std::vector<std::string> notes;
        ensureExecutable(exe, notes);
        BackendInstall bi;
        bi.backendId = def_.id;
        bi.version = "user";
        bi.root = pathToUtf8(root);
        bi.executable = pathToUtf8(exe);
        bi.userProvided = true;
        lib.setInstall(bi);
        lib.save();
        if (!notes.empty()) appendLog(pathToUtf8(paths().logs / (def_.id + ".log")), notes);
        return true;
    }

    std::vector<std::string> missingRequirements(const LaunchRequest& req) const override {
        std::vector<std::string> out = Backend::missingRequirements(req);
        const BackendInstall* bi = req.library ? req.library->install(def_.id) : nullptr;
        if (!bi || installedExecutable(*req.library).empty()) {
            if (flavor_ == Flavor::Rr1)
                out.push_back("Link your local Ridge Racer recomp build (upstream publishes source only; the game "
                              "code is generated from your own disc). See the backend notes for the build steps.");
            return out;
        }
        auto l = resolveLayout(flavor_, *bi);
        if (!l) return out;
        if (flavor_ == Flavor::R4 && l->needsSetup)
            out.push_back("Note: the first start opens R4's own setup wizard. It downloads its build tools (about "
                          "100-200 MB), checks your disc and compiles the game from it (10-20 minutes, once). "
                          "Pick your R4 .cue when it asks; Marathon starts the finished build directly afterwards.");
        if (flavor_ == Flavor::Rr1 && req.settings.redraw && !l->nativeSceneReady())
            out.push_back("Note: native-scene redraw is not prepared in this build (run scripts/prepare-native-scene.sh "
                          "in it). Marathon starts the original renderer instead.");
        return out;
    }

    LaunchResult launch(const LaunchRequest& req) override {
        LaunchResult r;
        const BackendInstall* bi = req.library ? req.library->install(def_.id) : nullptr;
        auto layout = bi ? resolveLayout(flavor_, *bi) : std::nullopt;
        if (!layout) {
            r.error = flavor_ == Flavor::Rr1 ? "Link your local Ridge Racer recomp build first."
                                             : def_.name + " is not installed.";
            return r;
        }
        if (!req.entry || !req.entry->hasFiles()) {
            r.error = "Add your " + (req.game ? req.game->title : std::string("game")) + " disc (.cue) first.";
            return r;
        }
        const Layout& l = *layout;
        std::vector<std::string> notes;
        const std::string logFile = pathToUtf8(paths().logs / (def_.id + ".log"));
        if (!ensureExecutable(l.exe, notes)) {
            appendLog(logFile, notes);
            r.error = pathToUtf8(l.exe) + " cannot be run (it is not marked executable). Run: chmod +x \"" +
                      pathToUtf8(l.exe) + "\"";
            return r;
        }
        const std::string disc = req.entry->files.path;

        if (flavor_ == Flavor::R4 && l.needsSetup) return launchSetupWizard(l, disc, logFile, notes);
        if (flavor_ == Flavor::R4 && l.appImage) return launchAppImage(req, l, disc, logFile, notes);

        const fs::path cards = saveDir(req, def_.id);
        importCards(cards, l.legacySaves, notes);

        const bool scene = flavor_ == Flavor::Rr1 && req.settings.redraw && l.nativeSceneReady();
        if (flavor_ == Flavor::Rr1 && req.settings.redraw && !scene)
            notes.push_back("redraw requested but the native scene is not prepared in this build: original renderer");
        writeSettings(req, l, scene, notes);
        writeMods(req, l, scene, notes);

        ProcessSpec game;
        game.argv = {pathToUtf8(l.exe)};
        if (!l.gameToml.empty()) game.argv.insert(game.argv.end(), {"--game", pathToUtf8(l.gameToml)});
        game.argv.insert(game.argv.end(), {"--disc", disc, "--no-launcher", "--memcard-dir", pathToUtf8(cards)});
        game.workingDir = pathToUtf8(flavor_ == Flavor::Rr1 ? l.root : l.buildDir);
        game.logFile = logFile;
        game.env["PSX_FAST_FORWARD_SPEED"] = turboValue(req.settings);
        if (flavor_ == Flavor::Rr1) {
            game.env["RIDGERACERRECOMP_BUILD_DIR"] = pathToUtf8(l.buildDir);
#if defined(_WIN32)
            game.env["SDL_RENDER_DRIVER"] = "opengl";  // as upstream's Launch.ps1
#endif
        }

        if (scene) return launchNativeScene(req, l, game, notes);
        notes.push_back("turbo: hold Tab (keyboard) or Select+L1 (pad) to fast-forward at " +
                        game.env["PSX_FAST_FORWARD_SPEED"] + "x");
        return spawnLogged(game, notes);
    }

private:
    Flavor flavor_;

    static std::string turboValue(const GameSettings& s) {
        if (s.turbo <= 0) return "max";
        int mult = (int)std::lround(s.turbo / 100.0);
        return std::to_string(std::clamp(mult, 2, 16));
    }

    static bool findRr1(const fs::path& p, fs::path& exe, fs::path& root) {
        std::error_code ec;
        auto accept = [&](const fs::path& candidate) {
            if (!isFile(candidate)) return false;
            fs::path gt = findGameToml(candidate.parent_path(), 2);
            if (gt.empty()) return false;
            exe = candidate;
            root = gt.parent_path();
            return true;
        };
        if (!fs::is_directory(p, ec)) {
            // The runtime itself, or something inside the build (Play.command, the service-menu app).
            std::string name = pathToUtf8(p.filename());
            if (name.rfind("RidgeRacer_Recompiled", 0) == 0) return accept(p);
            for (fs::path d = p.parent_path(); !d.empty() && d != d.parent_path(); d = d.parent_path())
                if (findRr1(d, exe, root)) return true;
            return false;
        }
        for (const fs::path& c : {p / "build-macos" / "RidgeRacer_Recompiled", p / "RidgeRacer_Recompiled.exe",
                                  p / "RidgeRacer_Recompiled", p / "dist" / "macos" / "build-macos" / "RidgeRacer_Recompiled",
                                  p / "dist" / "windows" / "RidgeRacer_Recompiled.exe",
                                  p / "build-windows" / "RidgeRacer_Recompiled.exe"})
            if (accept(c)) return true;
        // A .app bundle or build-macos folder was chosen: look upwards.
        if (pathToUtf8(p.filename()) == "build-macos" || p.extension() == ".app")
            for (fs::path d = p.parent_path(); !d.empty() && d != d.parent_path(); d = d.parent_path())
                if (isFile(d / "game.toml")) return findRr1(d, exe, root);
        return false;
    }

    static bool findR4(const fs::path& p, fs::path& exe, fs::path& root) {
        std::error_code ec;
        if (!fs::is_directory(p, ec)) {
            exe = p;
            root = p.parent_path();
            // build-release/r4-runtime chosen inside a kit: link the kit root.
            if (pathToUtf8(root.filename()) == "build-release" && isFile(root.parent_path() / p.filename())) {
                root = root.parent_path();
                exe = root / p.filename();
            }
            return true;
        }
        for (const fs::path& c : {p / exeName("r4-runtime"), p / "r4-runtime.exe", p / "r4-runtime"}) {
            if (isFile(c)) {
                exe = c;
                root = p;
                return true;
            }
        }
        for (auto& e : fs::directory_iterator(p, ec)) {
            std::string n = pathToUtf8(e.path().filename());
            if (e.is_regular_file(ec) && n.size() > 9 && n.find(".AppImage") != std::string::npos) {
                exe = e.path();
                root = p;
                return true;
            }
        }
        return false;
    }

    // settings.toml beside the runtime: only the keys Marathon owns.
    void writeSettings(const LaunchRequest& req, const Layout& l, bool scene, std::vector<std::string>& notes) const {
        const GameSettings& s = req.settings;
        const fs::path file = l.buildDir / "settings.toml";
        auto current = readToml(file);
        std::vector<std::pair<std::string, std::string>> kv;
        const int nativeHeight = jget(req.option ? req.option->params : json::object(), "nativeHeight", 240);
        const int scale = resolveScale(s.resolution, nativeHeight, req.display.height, 32);
        kv.emplace_back("video.supersampling", std::to_string(std::clamp(scale, 1, 4)));
        if (flavor_ == Flavor::R4) kv.emplace_back("video.internal_resolution", tomlStr(resolutionPreset(s.resolution, scale)));
        kv.emplace_back("video.texture_filtering", tomlStr(s.filter == "nearest" ? "nearest" : "bilinear"));
        int fullscreen = s.fullscreen ? 1 : 0;
        if (s.fullscreen && current) {
            // Keep the user's exclusive-fullscreen choice (2) when fullscreen is wanted.
            if ((*current)["video"]["fullscreen"].value_or<int64_t>(0) == 2) fullscreen = 2;
        }
        kv.emplace_back("video.fullscreen", std::to_string(fullscreen));
        kv.emplace_back("video.vsync", tomlStr(s.vsync ? "on" : "off"));
        kv.emplace_back("video.perspective_texturing", tomlBool(s.precision));
        kv.emplace_back("video.fast_boot", tomlBool(s.fastBoot));
        // Presentation interpolation is mod-owned in these titles; the generic switch stays off.
        kv.emplace_back("video.frame_interpolation", "false");
        kv.emplace_back("video.frame_interpolation_fps", "0");
        if (flavor_ == Flavor::Rr1) {
            // RR1's service menu validates these; keep them in its accepted ranges.
            kv.emplace_back("video.renderer", tomlStr("opengl"));
            kv.emplace_back("video.aspect_ratio", tomlStr("4:3"));  // widescreen is the ridge.presentation mod
            kv.emplace_back("video.adaptive_view", "false");
            int64_t width = current ? (*current)["video"]["window_width"].value_or<int64_t>(0) : 0;
            if (width < 960 || width > 3840) kv.emplace_back("video.window_width", "1280");
            const bool wide = sceneAspect(req, current) == "16:9";
            const int lines = std::clamp(targetLines(s.resolution, req.display.height), 240, 4320);
            const int a = wide ? 16 : 4, b = wide ? 9 : 3;
            int units = std::max((240 + b - 1) / b, std::min({7680 / a, 4320 / b, (int)std::lround((double)lines / b)}));
            kv.emplace_back("ridge_native_scene.nativeScene", tomlBool(scene));
            kv.emplace_back("ridge_native_scene.nativeWidth", std::to_string(units * a));
            kv.emplace_back("ridge_native_scene.nativeHeight", std::to_string(units * b));
            kv.emplace_back("ridge_native_scene.nativeFps", std::to_string(sceneFps(s)));
            kv.emplace_back("ridge_native_scene.aspect", tomlStr(wide ? "16:9" : "4:3"));
            kv.emplace_back("ridge_native_scene.perspective", tomlBool(s.precision));
        }
        std::string error;
        if (!dryRun() && !cfg::tomlSet(pathToUtf8(file), kv, &error)) {
            notes.push_back("settings.toml not updated: " + error);
            return;
        }
        std::string summary;
        for (auto& [k, v] : kv) summary += (summary.empty() ? "" : ", ") + k + "=" + v;
        notes.push_back("settings.toml: " + summary);
    }

    // "auto" keeps the aspect the build is currently set to.
    static std::string sceneAspect(const LaunchRequest& req, const std::optional<toml::table>& current) {
        const std::string& a = req.settings.aspect;
        if (a == "4:3") return "4:3";
        if (a == "16:9" || a == "fit" || a == "21:9") return "16:9";
        if (current) {
            std::string saved = (*current)["ridge_native_scene"]["aspect"].value_or(std::string());
            if (saved == "4:3" || saved == "16:9") return saved;
        }
        return "16:9";
    }

    // Native-scene frames per second: 0 = the display's refresh rate, else 30..360.
    static int sceneFps(const GameSettings& s) {
        int fps = resolveFps(s);
        if (fps < 0) return 0;
        if (fps == 0) return 60;
        return std::clamp(fps, 30, 360);
    }

    // RR1: widescreen and frame blending are the ridge.presentation mod.
    // R4: the R4 Frame Rate / Custom Renderer (widescreen) mods and PGXP.
    void writeMods(const LaunchRequest& req, const Layout& l, bool scene, std::vector<std::string>& notes) const {
        const GameSettings& s = req.settings;
        const fs::path mods = l.buildDir / "mods";
        std::vector<ModChange> changes;
        const int fps = resolveFps(s);
        if (flavor_ == Flavor::Rr1) {
            if (!scene && (s.aspect == "21:9" || s.aspect == "32:9"))
                notes.push_back("the original renderer draws at most 16:9 (" + s.aspect + " requested); "
                                "turn on Experimental redraw for ultrawide");
            if (s.aspect != "auto") {
                ModChange view{"ridge.presentation", "view", s.aspect != "4:3", {}};
                view.values.emplace_back("mode", s.aspect == "fit" ? "adaptive" : "wide");
                changes.push_back(view);
            }
            // Temporal blending at a presentation target (classic renderer only; the native
            // scene draws real frames instead, so the mod stays off there).
            static const int targets[] = {60, 90, 120, 144, 165, 240};
            int target = 0;
            if (fps > 0) {
                target = targets[0];
                for (int t : targets)
                    if (std::abs(t - fps) < std::abs(target - fps)) target = t;
            }
            ModChange motion{"ridge.presentation", "motion", !scene && fps != 0, {}};
            motion.values.emplace_back("target", std::to_string(fps < 0 ? 0 : target));
            changes.push_back(motion);
        } else {
            // R4 Frame Rate: display | 60 | 100 | 120 | 200 | 240 | 300 | custom (+ custom_fps 60..1000).
            ModChange rate{"r4.enhancement.frame-rate", "frame-rate", fps != 0, {}};
            std::string value = "display";
            if (fps > 0) {
                static const int presets[] = {60, 100, 120, 200, 240, 300};
                value = "custom";
                for (int p : presets)
                    if (p == fps) value = std::to_string(p);
            }
            rate.values.emplace_back("rate", value);
            if (value == "custom") rate.values.emplace_back("custom_fps", std::to_string(std::clamp(fps, 60, 1000)));
            rate.values.emplace_back("method", s.fpsMethod == "blend" ? "blend" : "interpolate");
            changes.push_back(rate);
            if (s.aspect != "auto") {
                ModChange wide{"r4.enhancement.widescreen", "widescreen", s.aspect != "4:3", {}};
                const bool ultra = s.aspect == "21:9" || s.aspect == "32:9";
                wide.values.emplace_back("aspect", s.aspect == "fit" ? "Fit" : ultra ? s.aspect : "16:9");
                changes.push_back(wide);
            }
            changes.push_back(ModChange{"psx.enhancement.pgxp", "pgxp", s.precision, {}});
        }
        applyModState(mods, changes, notes);
    }

    LaunchResult launchSetupWizard(const Layout& l, const std::string& disc, const std::string& logFile,
                                   std::vector<std::string>& notes) const {
        ProcessSpec spec;
        spec.argv = {pathToUtf8(l.setupHost), "--disc", disc};
        spec.workingDir = pathToUtf8(l.setupHost.parent_path());
        spec.logFile = logFile;
        notes.push_back("R4 is not built yet: opening upstream's setup wizard (build tools download, then "
                        "Generate & rebuild from your disc, 10-20 minutes). Launch again from Marathon afterwards.");
        return spawnLogged(spec, notes);
    }

    // omegakatana92's AppImage: an older runtime. Settings and cards go through settings.toml.
    LaunchResult launchAppImage(const LaunchRequest& req, const Layout& l, const std::string& disc,
                                const std::string& logFile, std::vector<std::string>& notes) const {
        const GameSettings& s = req.settings;
        const fs::path cards = saveDir(req, def_.id);
        importCards(cards, {l.buildDir / "saves"}, notes);
        const int nativeHeight = jget(req.option ? req.option->params : json::object(), "nativeHeight", 240);
        const int scale = resolveScale(s.resolution, nativeHeight, req.display.height, 4);
        const int fps = resolveFps(s);
        std::vector<std::pair<std::string, std::string>> kv = {
            {"video.supersampling", std::to_string(std::clamp(scale, 1, 4))},
            {"video.texture_filtering", tomlStr(s.filter == "nearest" ? "nearest" : "bilinear")},
            {"video.fullscreen", s.fullscreen ? "1" : "0"},
            {"video.vsync", tomlStr(s.vsync ? "on" : "off")},
            {"video.perspective_texturing", tomlBool(s.precision)},
            {"video.fast_boot", tomlBool(s.fastBoot)},
            // This runtime still offers the generic presentation blending.
            {"video.frame_interpolation", tomlBool(fps != 0)},
            {"video.frame_interpolation_fps", std::to_string(fps <= 0 ? 0 : std::max(fps, 90))},
            {"memcard.dir", tomlStr(pathToUtf8(cards))},
            {"memcard.card1", tomlStr(pathToUtf8(cards / "card1.mcd"))},
            {"memcard.card2", tomlStr(pathToUtf8(cards / "card2.mcd"))},
        };
        std::string error;
        if (!dryRun() && !cfg::tomlSet(pathToUtf8(l.buildDir / "settings.toml"), kv, &error))
            notes.push_back("settings.toml not updated: " + error);
        else
            notes.push_back("settings.toml beside the AppImage updated (resolution, filtering, window, cards)");
        ProcessSpec spec;
        spec.argv = {pathToUtf8(l.exe), "--disc", disc};
        // Older runtimes resolve the cue's relative track names against the working directory.
        spec.workingDir = pathToUtf8(utf8ToPath(disc).parent_path());
        spec.logFile = logFile;
        spec.env["PSX_FAST_FORWARD_SPEED"] = turboValue(s);
        return spawnLogged(spec, notes);
    }

    LaunchResult launchNativeScene(const LaunchRequest& req, const Layout& l, ProcessSpec game,
                                   std::vector<std::string>& notes) {
        const GameSettings& s = req.settings;
        auto session = std::make_shared<SceneSession>();
        session->logFile = game.logFile;
        std::vector<std::string> viewerNotes;
        ensureExecutable(l.viewer, viewerNotes);
        notes.insert(notes.end(), viewerNotes.begin(), viewerNotes.end());

        // Viewer geometry from settings.toml's ridge_native_scene (just written).
        auto settings = readToml(l.buildDir / "settings.toml");
        int width = settings ? (int)(*settings)["ridge_native_scene"]["nativeWidth"].value_or<int64_t>(1920) : 1920;
        int height = settings ? (int)(*settings)["ridge_native_scene"]["nativeHeight"].value_or<int64_t>(1080) : 1080;
        // Ultrawide: the renderer's focal length follows the image height, so a wider image shows
        // more of the course at the sides. RR1's own service menu only accepts 4:3 and 16:9, so
        // wider shapes go to the renderer directly and never into settings.toml.
        if (const double ratio = aspectRatio(s.aspect, req.display.width, req.display.height); ratio > 16.0 / 9.0 + 0.01) {
            width = static_cast<int>(std::lround(height * ratio)) & ~1;
            if (width > 7680) {  // the renderer's limit: keep the shape, lower the height
                width = 7680;
                height = static_cast<int>(std::lround(7680 / ratio)) & ~1;
            }
        }
        const bool fullCourse = jget(s.extra, "nativeFullCourse", true);
        const int carDistance = std::clamp(jget(s.extra, "nativeCarDistance", 0), 0, 5);
        std::string error;

#if defined(_WIN32)
        // Windows package (flat): the companion shares settings.toml, written for a small
        // hidden window exactly like upstream's Launch.ps1 does for the enhanced renderer.
        std::vector<std::pair<std::string, std::string>> companion = {
            {"video.fullscreen", "0"}, {"video.window_width", "640"}, {"video.supersampling", "1"},
            {"video.frame_interpolation", "false"}, {"video.frame_interpolation_fps", "0"}};
        if (!dryRun()) cfg::tomlSet(pathToUtf8(l.buildDir / "settings.toml"), companion, &error);
        static std::atomic<int> sessions{0};
        std::error_code ec;
        session->stage = fs::temp_directory_path(ec) /
                         utf8ToPath("ridge-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                    "-" + std::to_string(++sessions));
        if (!dryRun()) fs::create_directories(session->stage, ec);
        session->socket = pathToUtf8(session->stage / "scene");
        if (isFile(l.transportCheck) && !dryRun()) {
            ProcessSpec check;
            check.argv = {pathToUtf8(l.transportCheck), pathToUtf8(session->stage / "check")};
            check.workingDir = pathToUtf8(l.buildDir);
            int rc = runProcess(check);
            notes.push_back("scene transport check exited " + std::to_string(rc));
        }
#else
        if (!dryRun()) {
            session->stage = makeStageDir(error);
            if (session->stage.empty()) {
                LaunchResult r;
                r.error = error;
                return r;
            }
        } else {
            session->stage = "/tmp/rrm-scene-DRYRUN";
        }
        session->socket = pathToUtf8(session->stage / "scene");
        // The companion is a real copy of the runtime (it resolves settings beside its own
        // executable, through symlinks) with a private settings.toml for a small hidden window.
        if (!dryRun()) {
            std::error_code ec;
            const fs::path copy = session->stage / l.exe.filename();
            fs::copy_file(l.exe, copy, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                removeStage(session->stage);
                LaunchResult r;
                r.error = "Could not prepare the native-scene companion: " + ec.message();
                return r;
            }
            fs::permissions(copy, fs::perms::owner_all, fs::perm_options::add, ec);
            for (const char* name : {"assets", "bios", "lib"})
                if (fs::exists(l.buildDir / name)) fs::create_directory_symlink(l.buildDir / name, session->stage / name, ec);
            if (isDir(l.buildDir / "mods"))
                fs::copy(l.buildDir / "mods", session->stage / "mods", fs::copy_options::recursive, ec);
            fs::copy_file(l.buildDir / "settings.toml", session->stage / "settings.toml",
                          fs::copy_options::overwrite_existing, ec);
            cfg::tomlSet(pathToUtf8(session->stage / "settings.toml"),
                         {{"video.fullscreen", "0"},
                          {"video.window_width", "640"},
                          {"video.supersampling", "1"},
                          {"video.frame_interpolation", "false"},
                          {"video.frame_interpolation_fps", "0"}},
                         &error);
            game.argv[0] = pathToUtf8(copy);
        } else {
            game.argv[0] = pathToUtf8(session->stage / l.exe.filename());
        }
#endif
        std::map<std::string, std::string> env = {
            {"RIDGE_SCENE_SOCKET", session->socket},
            {"RIDGE_SCENE_INPUT", "1"},
            {"RIDGE_NATIVE_SCENE", "1"},
            {"RIDGE_SCENE_FULL_COURSE", fullCourse ? "1" : "0"},
            {"RIDGE_SCENE_CAR_DISTANCE", std::to_string(carDistance)},
            {"SDL_RENDER_DRIVER", "opengl"},
            {"PSX_HIDDEN_COMPANION", "1"},
        };
        for (auto& [k, v] : env) game.env[k] = v;

        ProcessSpec viewer;
        viewer.argv = {pathToUtf8(l.viewer), "--live", session->socket, "--assets", pathToUtf8(l.sceneAssets),
                       "--game-camera", "--control", "--fps", std::to_string(sceneFps(s)), "--vsync",
                       s.vsync ? "on" : "off", "--width", std::to_string(width), "--height", std::to_string(height)};
        if (s.fullscreen) viewer.argv.push_back("--fullscreen");
        if (!s.precision) viewer.argv.push_back("--affine-textures");
        if (!fullCourse) viewer.argv.push_back("--legacy-distance");
        // Diagnostics: settings.extra.sceneShot = "<file>.bmp" saves one frame after sceneShotAt seconds.
        if (std::string shot = jget<std::string>(s.extra, "sceneShot", ""); !shot.empty()) {
            viewer.argv.insert(viewer.argv.end(), {"--shot", shot, "--at", std::to_string(jget(s.extra, "sceneShotAt", 30))});
        }
        viewer.workingDir = game.workingDir;
        viewer.logFile = game.logFile;
        viewer.env = env;
        viewer.env["RIDGERACERRECOMP_BUILD_DIR"] = game.env["RIDGERACERRECOMP_BUILD_DIR"];
        session->viewer = viewer;
        session->game = game;

        notes.push_back("native-scene redraw: " + std::to_string(width) + "x" + std::to_string(height) + " at " +
                        (sceneFps(s) == 0 ? std::string("display rate") : std::to_string(sceneFps(s)) + " fps"));
        std::vector<std::string> companionNotes = {"companion (started once the renderer is ready):"};
        logSpec(game, companionNotes);
        notes.insert(notes.end(), companionNotes.begin(), companionNotes.end());

        LaunchResult r = spawnLogged(viewer, notes);
        if (!r.ok || dryRun()) {
            if (!r.ok) removeStage(session->stage);
            return r;
        }
        std::thread(superviseScene, session, r.pid).detach();
        return r;
    }
};

}  // namespace

std::unique_ptr<Backend> makePsxRecomp(const BackendDef& def) { return std::make_unique<PsxRecomp>(def); }

}  // namespace rrm
