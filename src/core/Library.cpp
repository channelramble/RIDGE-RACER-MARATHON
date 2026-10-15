#include "core/Library.h"

#include "core/Platform.h"

namespace rrm {

namespace {
json filesToJson(const GameFiles& f) {
    return json{{"path", f.path},         {"format", f.format}, {"detectedBy", f.detectedBy},
                {"verified", f.verified}, {"size", f.size},     {"addedAt", f.addedAt}};
}
GameFiles filesFromJson(const json& j) {
    GameFiles f;
    f.path = jget<std::string>(j, "path", "");
    f.format = jget<std::string>(j, "format", "");
    f.detectedBy = jget<std::string>(j, "detectedBy", "");
    f.verified = jget<std::string>(j, "verified", "");
    f.size = jget<uint64_t>(j, "size", 0);
    f.addedAt = jget<int64_t>(j, "addedAt", 0);
    return f;
}
fs::path libraryPath() { return paths().data / "library.json"; }
}  // namespace

bool Library::load() {
    json j;
    if (!readJsonFile(libraryPath(), j)) return false;
    entries_.clear();
    installs_.clear();
    systemFiles_.clear();
    if (auto it = j.find("games"); it != j.end() && it->is_object()) {
        for (auto& [id, g] : it->items()) {
            LibraryEntry e;
            e.gameId = id;
            if (g.contains("files")) e.files = filesFromJson(g["files"]);
            if (g.contains("settings")) e.settings = GameSettings::fromJson(g["settings"]);
            e.customSettings = jget(g, "customSettings", false);
            e.lastPlayed = jget<int64_t>(g, "lastPlayed", 0);
            e.playSeconds = jget<int64_t>(g, "playSeconds", 0);
            entries_[id] = std::move(e);
        }
    }
    if (auto it = j.find("installs"); it != j.end() && it->is_object()) {
        for (auto& [id, b] : it->items()) {
            BackendInstall bi;
            bi.backendId = id;
            bi.version = jget<std::string>(b, "version", "");
            bi.root = jget<std::string>(b, "root", "");
            bi.executable = jget<std::string>(b, "executable", "");
            bi.userProvided = jget(b, "userProvided", false);
            installs_[id] = bi;
        }
    }
    if (auto it = j.find("system"); it != j.end() && it->is_object()) {
        for (auto& [k, v] : it->items())
            if (v.is_string()) systemFiles_[k] = v.get<std::string>();
    }
    scanFolders = jget(j, "scanFolders", std::vector<std::string>{});
    return true;
}

bool Library::save() const {
    json games = json::object();
    for (auto& [id, e] : entries_) {
        games[id] = json{{"files", filesToJson(e.files)},
                         {"settings", e.settings.toJson()},
                         {"customSettings", e.customSettings},
                         {"lastPlayed", e.lastPlayed},
                         {"playSeconds", e.playSeconds}};
    }
    json installs = json::object();
    for (auto& [id, b] : installs_) {
        installs[id] = json{{"version", b.version},
                            {"root", b.root},
                            {"executable", b.executable},
                            {"userProvided", b.userProvided}};
    }
    json system = json::object();
    for (auto& [k, v] : systemFiles_) system[k] = v;
    json j{{"version", 1}, {"games", games}, {"installs", installs}, {"system", system}, {"scanFolders", scanFolders}};
    return writeJsonFile(libraryPath(), j);
}

LibraryEntry& Library::entry(const std::string& gameId) {
    auto& e = entries_[gameId];
    e.gameId = gameId;
    return e;
}

const LibraryEntry* Library::find(const std::string& gameId) const {
    auto it = entries_.find(gameId);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<std::string> Library::gameIds() const {
    std::vector<std::string> ids;
    for (auto& [id, _] : entries_) ids.push_back(id);
    return ids;
}

void Library::setInstall(const BackendInstall& install) { installs_[install.backendId] = install; }

const BackendInstall* Library::install(const std::string& backendId) const {
    auto it = installs_.find(backendId);
    return it == installs_.end() ? nullptr : &it->second;
}

void Library::removeInstall(const std::string& backendId) { installs_.erase(backendId); }

std::string Library::systemFile(const std::string& key) const {
    auto it = systemFiles_.find(key);
    return it == systemFiles_.end() ? std::string() : it->second;
}

void Library::setSystemFile(const std::string& key, const std::string& path) {
    if (path.empty()) systemFiles_.erase(key);
    else systemFiles_[key] = path;
}

}  // namespace rrm
