// Rage Racer PC (khasinski/rage-racer-pc): native source port of Rage Racer.
// The official release stops at 16:9 (video.aspect 4:3 or 16:9) and has no fullscreen mode.
// params.ultrawide (default false): pass 21:9, 32:9 and the screen's shape through as
// video.aspect instead of stopping at 16:9, and set RAGE_FULLSCREEN=1 when fullscreen is on.
//
// Upstream publishes no licence (all rights reserved), so Marathon only ever
// downloads the official release archive on the user's machine.
//
// Configuration: the port reads rage-port.ini (beside the executable, or the
// copy in its user folder) and accepts `--set section.key=value` overrides on the
// command line. Marathon passes every setting it owns as --set, so the user's
// ini is never modified. Saves and the remembered disc live in the port's own
// user folder (~/Library/Application Support/Rage Racer, %APPDATA%\Rage Racer,
// $XDG_STATE_HOME/rage-racer), outside the install, so they survive updates.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <system_error>

#include "backends/Adapters.h"
#include "core/Platform.h"
#include "core/Process.h"

namespace rrm {

namespace {

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

LaunchResult spawnLogged(const ProcessSpec& spec, std::vector<std::string> notes) {
    LaunchResult r;
    r.mode = LaunchResult::Mode::ExternalProcess;
    r.commandLine = spec.argv;
    notes.insert(notes.begin(), "==== launch " + timeStamp() + (dryRun() ? " (dry run)" : ""));
    notes.push_back("command: " + joinCommand(spec.argv));
    if (!spec.workingDir.empty()) notes.push_back("cwd: " + spec.workingDir);
    for (auto& [k, v] : spec.env) notes.push_back("env: " + k + "=" + v);
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

// Formats a scale such as 4.5 without trailing zeros ("4.5", "3", "9.31").
std::string formatScale(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// The port renders at any fractional multiple of 320x240 (0.5..16), so the
// unified resolution maps to the exact line count instead of a rounded-up integer.
double internalScale(const std::string& res, int displayHeight) {
    double lines = 240;
    if (res == "native") lines = 240;
    else if (res.size() >= 2 && res.back() == 'x') lines = 240.0 * std::max(1, std::atoi(res.c_str()));
    else if (res == "720p") lines = 720;
    else if (res == "1080p") lines = 1080;
    else if (res == "1440p") lines = 1440;
    else if (res == "4k") lines = 2160;
    else if (res == "5k") lines = 2880;
    else if (res == "8k") lines = 4320;
    else if (res == "display") lines = displayHeight > 0 ? displayHeight : 1080;
    double scale = lines / 240.0;
    scale = std::round(scale * 100.0) / 100.0;
    return std::clamp(scale, 0.5, 16.0);
}

class RageRacerPc : public Backend {
public:
    using Backend::Backend;

    LaunchResult launch(const LaunchRequest& req) override {
        LaunchResult r;
        const std::string exe = req.library ? installedExecutable(*req.library) : std::string();
        if (exe.empty()) {
            r.error = def_.name + " is not installed.";
            return r;
        }
        if (!req.entry || !req.entry->hasFiles()) {
            r.error = "Add your Rage Racer disc image (.cue with its .bin tracks, Track 01 .bin or .chd) first.";
            return r;
        }
        const GameSettings& s = req.settings;
        const std::string logFile = pathToUtf8(paths().logs / (def_.id + ".log"));
        std::vector<std::string> sets;
        std::vector<std::string> notes;

        // Disc: the port imports its native assets from it at start-up.
        sets.push_back("disc.image=" + req.entry->files.path);

        // Redraw = the modern renderer (native geometry, depth buffer); off = classic PS1 polygons.
        sets.push_back(std::string("video.renderer=") + (s.redraw ? "modern" : "classic"));

        const double scale = internalScale(s.resolution, req.display.height);
        sets.push_back("video.internal_scale=" + formatScale(scale));

        const bool ultrawide = jget(def_.params, "ultrawide", false);
        if (s.aspect == "4:3" || s.aspect == "16:9") {
            sets.push_back("video.aspect=" + s.aspect);
        } else if (ultrawide && (s.aspect == "21:9" || s.aspect == "32:9")) {
            sets.push_back("video.aspect=" + s.aspect);
        } else if (ultrawide && s.aspect == "fit") {
            // The screen's own shape, as its pixel size.
            const double ratio = req.display.height > 0 ? double(req.display.width) / req.display.height : 16.0 / 9.0;
            if (ratio <= 4.0 / 3.0 + 0.01) sets.push_back("video.aspect=4:3");
            else if (ratio > 4.0) sets.push_back("video.aspect=4:1");
            else sets.push_back("video.aspect=" + std::to_string(req.display.width) + ":" + std::to_string(req.display.height));
        } else if (s.aspect == "fit" || s.aspect == "21:9" || s.aspect == "32:9") {
            // The official port offers 4:3 and 16:9: the one that fills more of the wanted shape
            // (cutoff about 1.54, between 3:2 and 16:10: 4:3 on 5:4, 4:3 and 3:2 screens, 16:9 from 16:10 up).
            const double wanted = aspectRatio(s.aspect, req.display.width, req.display.height);
            if (wanted < std::sqrt(4.0 / 3.0 * 16.0 / 9.0)) {
                sets.push_back("video.aspect=4:3");
            } else {
                sets.push_back("video.aspect=16:9");
                if (wanted > 16.0 / 9.0 + 0.01)
                    notes.push_back("the official Rage Racer PC stops at 16:9");
            }
        }
        // "auto": keep the ini's choice.

        const int fps = resolveFps(s);
        std::string fpsValue = fps == 0 ? "logic" : fps < 0 ? "vsync" : std::to_string(std::clamp(fps, 1, 1000));
        sets.push_back("video.fps=" + fpsValue);

        sets.push_back(std::string("video.texture_filter=") + (s.filter == "nearest" ? "nearest" : "linear"));
        sets.push_back(std::string("video.post=") + (s.antiAlias == "fxaa" ? "fxaa" : "none"));

        // Classic with nothing enhanced = the original framebuffer presentation ("fit" on a 4:3 screen too).
        const bool fourThree = std::find(sets.begin(), sets.end(), "video.aspect=4:3") != sets.end();
        const bool original = !s.redraw && scale == 1.0 && fps == 0 && (s.aspect == "auto" || fourThree);
        sets.push_back(std::string("video.classic_enhancements=") + (original ? "false" : "true"));

        // Hybrid ray tracing in the modern renderer: costly and experimental, so Marathon
        // defaults it off; settings.extra.rayTracing = off|shadows|reflections|full overrides.
        std::string rt = jget<std::string>(def_.params, "rayTracing", "off");
        std::string userRt = jget<std::string>(s.extra, "rayTracing", "");
        if (userRt == "off" || userRt == "shadows" || userRt == "reflections" || userRt == "full") rt = userRt;
        sets.push_back("modern.ray_tracing=" + rt);

        // The port moves stderr into its own log file; point it at Marathon's log.
        sets.push_back("diagnostics.log=" + logFile);
        // Advanced: settings.extra.set passes further port settings, as {"key": "value"} or "k=v;k=v".
        if (auto it = s.extra.find("set"); it != s.extra.end()) {
            if (it->is_object()) {
                for (auto& [k, v] : it->items())
                    if (v.is_string()) sets.push_back(k + "=" + v.get<std::string>());
            } else if (it->is_string()) {
                const std::string all = it->get<std::string>();
                for (size_t at = 0; at < all.size();) {
                    size_t end = all.find(';', at);
                    if (end == std::string::npos) end = all.size();
                    if (all.find('=', at) < end) sets.push_back(all.substr(at, end - at));
                    at = end + 1;
                }
            }
        }

        ProcessSpec spec;
        spec.argv.push_back(exe);
        // Advanced: settings.extra.scenario starts straight into a configured race (race-scenario.ini format).
        if (std::string scenario = jget<std::string>(s.extra, "scenario", ""); !scenario.empty()) {
            spec.argv.push_back("--scenario");
            spec.argv.push_back(scenario);
        }
        for (auto& kv : sets) {
            spec.argv.push_back("--set");
            spec.argv.push_back(kv);
        }
        spec.workingDir = installedRoot(*req.library);
        spec.logFile = logFile;
        if (s.fullscreen && ultrawide) spec.env["RAGE_FULLSCREEN"] = "1";  // params.ultrawide
        else if (s.fullscreen) notes.push_back("the official port has no fullscreen mode; use the window's green button");
        if (!s.vsync && fps < 0) notes.push_back("video.fps=vsync always follows the display refresh");
        return spawnLogged(spec, notes);
    }
};

}  // namespace

std::unique_ptr<Backend> makeRageRacerPc(const BackendDef& def) { return std::make_unique<RageRacerPc>(def); }

}  // namespace rrm
