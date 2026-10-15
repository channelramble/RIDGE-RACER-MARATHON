// Platform detection and well-known directories.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rrm {

namespace fs = std::filesystem;

enum class Os { MacOS, Windows, Linux, Android, iOS };

Os currentOs();
const char* osId(Os os);          // "macos", "windows", "linux", "android", "ios"
bool osFromId(const std::string& id, Os& out);
bool isDesktop(Os os);            // macOS, Windows, Linux: external processes allowed
inline bool isDesktop() { return isDesktop(currentOs()); }
const char* cpuArch();            // "arm64", "x86_64", ...

// Directories. All are created on first use.
struct Paths {
    fs::path data;       // per-user writable root (settings, library, saves)
    fs::path backends;   // installed external backends (desktop)
    fs::path cores;      // libretro cores (desktop/Android; iOS cores are bundled)
    fs::path system;     // BIOS / firmware supplied by the user (system/ps1, ps2, ps3)
    fs::path saves;      // per-game save roots
    fs::path config;     // generated backend configs
    fs::path cache;      // downloads, extracted temporaries
    fs::path logs;
    fs::path userFiles;  // where the user is told to put game files (iOS Documents etc.)
    fs::path resources;  // read-only app resources (catalog, fonts). May be empty on Android
                         // (resources come from APK assets via SDL_IOFromFile relative paths).
};

const Paths& paths();
void initPaths(const char* overrideDataDir = nullptr);

// Reads a resource file (catalog JSON, fonts) from the app bundle / APK assets.
bool readResource(const std::string& relative, std::string& out);

// Opens a URL or a folder in the system browser / file manager.
void openExternal(const std::string& urlOrPath);

// Android: hands a game to another installed app via an Intent. spec follows the
// LaunchResult::commandLine convention for ExternalApp:
//   {"android-intent", "<package>/<activity class>", "<action or empty>", "<file path or content URI>",
//    "extra:<key>=<value>", ...}
// Other platforms return false with an explanation. Implemented in PlatformAndroid.cpp on Android.
bool launchExternalApp(const std::vector<std::string>& spec, std::string& error);
// Android: true if a package is installed. Always false elsewhere.
bool isAppInstalled(const std::string& package);

std::string pathToUtf8(const fs::path& p);
fs::path utf8ToPath(const std::string& s);

}  // namespace rrm
