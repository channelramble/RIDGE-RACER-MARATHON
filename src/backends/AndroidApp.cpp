// Android only: hands a game to another installed emulator app (PS2, PS3) when no
// core can run it inside Marathon. Which apps are tried, and how each one is
// started, is catalog data (backend params "apps", in order of preference):
//   {"name": "NetherSX2", "package": "xyz.aethersx2.android",
//    "activity": "xyz.aethersx2.android.EmulationActivity", "action": "android.intent.action.MAIN",
//    "data": "", "extras": {"bootPath": "{path}"}}
// "{path}" is replaced by the game file (the platform layer turns it into a content URI).
// "data" is the intent data (default "{path}"; "" for apps that take the file as an extra).
// The intent spec format is documented in core/Platform.h (launchExternalApp).
#include "backends/Adapters.h"
#include "backends/EmuCommon.h"

namespace rrm {

namespace {

using namespace emu;

class AndroidApp : public Backend {
public:
    using Backend::Backend;

    InstallState installState(const Library&) const override {
        if (currentOs() != Os::Android) return InstallState::Unavailable;
        return firstInstalled().is_null() ? InstallState::NotInstalled : InstallState::UserProvided;
    }

    bool install(Library&, TaskProgress& progress) override {
        RunningGuard running(progress);
        std::string names;
        for (auto& a : apps()) {
            if (!names.empty()) names += ", ";
            names += jget<std::string>(a, "name", jget<std::string>(a, "package", ""));
        }
        progress.fail("Install one of these apps from its store or website, then come back: " + names + ".");
        return false;
    }

    LaunchResult launch(const LaunchRequest& req) override {
        const json app = firstInstalled();
        if (app.is_null()) return fail("None of the supported apps is installed on this device.");
        if (!req.entry || !req.entry->hasFiles()) return fail("Add your " + req.game->title + " files first.");
        const std::string path = gamePath(req);
        auto expand = [&](std::string v) {
            for (size_t at = v.find("{path}"); at != std::string::npos; at = v.find("{path}", at + path.size()))
                v.replace(at, 6, path);
            return v;
        };
        LaunchResult r;
        r.mode = LaunchResult::Mode::ExternalApp;
        r.commandLine = {"android-intent",
                         jget<std::string>(app, "package", "") + "/" + jget<std::string>(app, "activity", ""),
                         jget<std::string>(app, "action", ""), expand(jget<std::string>(app, "data", "{path}"))};
        if (auto ex = app.find("extras"); ex != app.end() && ex->is_object())
            for (auto& [k, v] : ex->items())
                if (v.is_string()) r.commandLine.push_back("extra:" + k + "=" + expand(v.get<std::string>()));
        r.ok = true;
        return r;
    }

private:
    json apps() const {
        json a = def_.params.value("apps", json::array());
        return a.is_array() ? a : json::array();
    }

    // The first catalog app that is installed, or null.
    json firstInstalled() const {
        for (auto& a : apps())
            if (isAppInstalled(jget<std::string>(a, "package", ""))) return a;
        return nullptr;
    }
};

}  // namespace

std::unique_ptr<Backend> makeAndroidApp(const BackendDef& def) { return std::make_unique<AndroidApp>(def); }

}  // namespace rrm
