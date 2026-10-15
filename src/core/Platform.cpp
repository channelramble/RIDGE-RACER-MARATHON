#include "core/Platform.h"

#include <SDL3/SDL.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <cstdlib>
#include <system_error>

namespace rrm {

Os currentOs() {
#if defined(__ANDROID__)
    return Os::Android;
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    return Os::iOS;
#else
    return Os::MacOS;
#endif
#elif defined(_WIN32)
    return Os::Windows;
#else
    return Os::Linux;
#endif
}

const char* osId(Os os) {
    switch (os) {
        case Os::MacOS: return "macos";
        case Os::Windows: return "windows";
        case Os::Linux: return "linux";
        case Os::Android: return "android";
        case Os::iOS: return "ios";
    }
    return "unknown";
}

bool osFromId(const std::string& id, Os& out) {
    for (Os os : {Os::MacOS, Os::Windows, Os::Linux, Os::Android, Os::iOS}) {
        if (id == osId(os)) {
            out = os;
            return true;
        }
    }
    return false;
}

bool isDesktop(Os os) { return os == Os::MacOS || os == Os::Windows || os == Os::Linux; }

const char* cpuArch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__arm__) || defined(_M_ARM)
    return "armv7";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

namespace {
Paths g_paths;
bool g_init = false;

fs::path ensureDir(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}
}  // namespace

std::string pathToUtf8(const fs::path& p) {
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

fs::path utf8ToPath(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

void initPaths(const char* overrideDataDir) {
    fs::path data;
    if (overrideDataDir && *overrideDataDir) {
        data = utf8ToPath(overrideDataDir);
    } else if (const char* env = std::getenv("RRM_DATA_DIR"); env && *env) {
        data = utf8ToPath(env);
    } else if (char* pref = SDL_GetPrefPath("RidgeRacerMarathon", "Marathon")) {
        data = utf8ToPath(pref);
        SDL_free(pref);
    } else {
        data = fs::current_path() / "marathon-data";
    }
    g_paths.data = ensureDir(data);
    g_paths.backends = ensureDir(data / "backends");
    g_paths.cores = ensureDir(data / "cores");
    g_paths.system = ensureDir(data / "system");
    g_paths.saves = ensureDir(data / "saves");
    g_paths.config = ensureDir(data / "config");
    g_paths.cache = ensureDir(data / "cache");
    g_paths.logs = ensureDir(data / "logs");

#if defined(__APPLE__)
#if TARGET_OS_IPHONE
    // iOS: Documents is visible in the Files app (UIFileSharingEnabled).
    if (const char* home = std::getenv("HOME")) g_paths.userFiles = ensureDir(fs::path(home) / "Documents" / "Games");
#endif
#endif
#if defined(__ANDROID__)
    // Android: app-specific external storage is reachable over USB without extra permissions.
    if (const char* ext = SDL_GetAndroidExternalStoragePath()) g_paths.userFiles = ensureDir(fs::path(ext) / "Games");
#endif
    if (g_paths.userFiles.empty()) g_paths.userFiles = ensureDir(data / "games");

    if (const char* base = SDL_GetBasePath()) {
        fs::path b = utf8ToPath(base);
        // macOS/iOS bundles: SDL returns .../Contents/Resources/ (mac) or the .app (iOS).
        if (fs::exists(b / "catalog" / "games.json")) g_paths.resources = b;
        else if (fs::exists(b / "Resources" / "catalog" / "games.json")) g_paths.resources = b / "Resources";
        else if (fs::exists(b / ".." / "Resources" / "catalog" / "games.json")) g_paths.resources = b / ".." / "Resources";
        else if (fs::exists(b / ".." / "share" / "ridge-racer-marathon" / "catalog" / "games.json"))
            g_paths.resources = b / ".." / "share" / "ridge-racer-marathon";
    }
    if (const char* env = std::getenv("RRM_RESOURCES"); env && *env) g_paths.resources = utf8ToPath(env);
    g_init = true;
}

const Paths& paths() {
    if (!g_init) initPaths();
    return g_paths;
}

bool readResource(const std::string& relative, std::string& out) {
    std::string full = paths().resources.empty() ? relative : pathToUtf8(paths().resources / utf8ToPath(relative));
    size_t size = 0;
    // SDL_LoadFile reads APK assets on Android when given a relative path.
    void* data = SDL_LoadFile(full.c_str(), &size);
    if (!data) return false;
    out.assign(static_cast<const char*>(data), size);
    SDL_free(data);
    return true;
}

#if !defined(__ANDROID__)
bool launchExternalApp(const std::vector<std::string>&, std::string& error) {
    error = "Handing games to other apps is only supported on Android.";
    return false;
}

bool isAppInstalled(const std::string&) { return false; }
#endif

void openExternal(const std::string& urlOrPath) {
    std::string url = urlOrPath;
    if (url.find("://") == std::string::npos) url = "file://" + url;
    SDL_OpenURL(url.c_str());
}

}  // namespace rrm
