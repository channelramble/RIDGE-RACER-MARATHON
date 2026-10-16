#include "core/Catalog.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace rrm {

namespace {

std::vector<Os> parseOsList(const json& j) {
    std::vector<Os> out;
    if (!j.is_array()) return out;
    for (auto& v : j) {
        Os os;
        if (v.is_string() && osFromId(v.get<std::string>(), os)) out.push_back(os);
    }
    return out;
}

KnownDump parseDump(const json& j) {
    KnownDump d;
    d.label = jget<std::string>(j, "label", "");
    d.file = jget<std::string>(j, "file", "");
    d.size = jget<uint64_t>(j, "size", 0);
    d.crc32 = jget<std::string>(j, "crc32", "");
    d.md5 = jget<std::string>(j, "md5", "");
    d.sha1 = jget<std::string>(j, "sha1", "");
    return d;
}

GameBackendOption parseOption(const json& j) {
    GameBackendOption o;
    o.backend = jget<std::string>(j, "backend", "");
    o.os = parseOsList(j.value("os", json::array()));
    o.priority = jget(j, "priority", 0);
    o.status = jget<std::string>(j, "status", "supported");
    o.note = jget<std::string>(j, "note", "");
    o.params = j.value("params", json::object());
    o.capabilities = j.value("capabilities", json::object());
    return o;
}

GameDef parseGame(const json& j) {
    GameDef g;
    g.id = jget<std::string>(j, "id", "");
    g.title = jget<std::string>(j, "title", g.id);
    g.system = jget<std::string>(j, "system", "");
    g.systemLabel = jget<std::string>(j, "systemLabel", g.system);
    g.year = jget(j, "year", 0);
    g.region = jget<std::string>(j, "region", "");
    g.serial = jget<std::string>(j, "serial", "");
    g.accent = jget<std::string>(j, "accent", "#F5C400");
    g.blurb = jget<std::string>(j, "blurb", "");
    g.mediaKind = jget<std::string>(j, "mediaKind", "");
    g.acceptedFormats = jget(j, "acceptedFormats", std::vector<std::string>{});
    g.requirements = jget(j, "requires", std::vector<std::string>{});
    g.fileHelp = jget<std::string>(j, "fileHelp", "");
    if (auto it = j.find("identify"); it != j.end() && it->is_object()) {
        auto& id = *it;
        g.identify.bootSerials = jget(id, "bootSerials", std::vector<std::string>{});
        g.identify.discIds = jget(id, "discIds", std::vector<std::string>{});
        g.identify.xboxTitleIds = jget(id, "xboxTitleIds", std::vector<std::string>{});
        g.identify.mameSet = jget<std::string>(id, "mameSet", "");
        g.identify.mameParents = jget(id, "mameParents", std::vector<std::string>{});
        g.identify.nameHints = jget(id, "nameHints", std::vector<std::string>{});
        if (auto d = id.find("dumps"); d != id.end() && d->is_array())
            for (auto& dj : *d) g.identify.dumps.push_back(parseDump(dj));
    }
    if (auto it = j.find("backends"); it != j.end() && it->is_array())
        for (auto& bj : *it) g.backends.push_back(parseOption(bj));
    return g;
}

BackendDef parseBackend(const json& j) {
    BackendDef b;
    b.id = jget<std::string>(j, "id", "");
    b.adapter = jget<std::string>(j, "adapter", b.id);
    b.name = jget<std::string>(j, "name", b.id);
    b.kind = jget<std::string>(j, "kind", "external");
    b.license = jget<std::string>(j, "license", "");
    b.homepage = jget<std::string>(j, "homepage", "");
    b.description = jget<std::string>(j, "description", "");
    b.core = jget<std::string>(j, "core", "");
    b.capabilities = j.value("capabilities", json::object());
    b.params = j.value("params", json::object());
    if (auto it = j.find("sources"); it != j.end() && it->is_object()) {
        for (auto& [platform, sj] : it->items()) {
            BackendSource s;
            s.platform = platform;
            s.kind = jget<std::string>(sj, "kind", "url");
            s.url = jget<std::string>(sj, "url", "");
            s.sha256 = jget<std::string>(sj, "sha256", "");
            s.repo = jget<std::string>(sj, "repo", "");
            s.assetPattern = jget<std::string>(sj, "asset", "");
            s.archive = jget<std::string>(sj, "archive", "");
            s.executable = jget<std::string>(sj, "executable", "");
            s.marker = jget<std::string>(sj, "marker", "");
            s.androidPackage = jget<std::string>(sj, "androidPackage", "");
            s.note = jget<std::string>(sj, "note", "");
            b.sources.push_back(std::move(s));
        }
    }
    return b;
}

}  // namespace

bool BackendSource::accepts(const std::string& exe) const {
    if (marker.empty()) return true;
    std::error_code ec;
    return fs::exists(utf8ToPath(exe).parent_path() / utf8ToPath(marker), ec);
}

const BackendSource* BackendDef::sourceFor(const std::string& platform) const {
    for (auto& s : sources)
        if (s.platform == platform) return &s;
    // "macos" matches any macOS arch entry such as "macos-universal".
    auto os = platform.substr(0, platform.find('-'));
    for (auto& s : sources)
        if (s.platform == os || s.platform == os + "-universal") return &s;
    return nullptr;
}

bool Catalog::load(std::string& error) {
    std::string games, backends, recomps;
    if (!readResource("catalog/games.json", games)) {
        error = "catalog/games.json not found in app resources";
        return false;
    }
    if (!readResource("catalog/backends.json", backends)) {
        error = "catalog/backends.json not found in app resources";
        return false;
    }
    readResource("catalog/recomps.json", recomps);  // optional second backend list
    if (!loadFromStrings(games, backends, error, recomps)) return false;
    loadExtraFiles(extraCatalogDir());
    return true;
}

bool Catalog::loadFromStrings(const std::string& gamesJson, const std::string& backendsJson, std::string& error,
                              const std::string& recompsJson) {
    try {
        json gj = json::parse(gamesJson, nullptr, true, true);
        json bj = json::parse(backendsJson, nullptr, true, true);
        games_.clear();
        backends_.clear();
        warnings_.clear();
        for (auto& g : gj.at("games")) games_.push_back(parseGame(g));
        for (auto& b : bj.at("backends")) backends_.push_back(parseBackend(b));
        if (!recompsJson.empty()) {
            json rj = json::parse(recompsJson, nullptr, true, true);
            for (auto& b : rj.at("backends")) backends_.push_back(parseBackend(b));
        }
    } catch (const std::exception& e) {
        error = std::string("catalog parse error: ") + e.what();
        return false;
    }
    for (auto& g : games_)
        for (auto& o : g.backends)
            if (!backend(o.backend)) {
                error = "game " + g.id + " references unknown backend " + o.backend;
                return false;
            }
    return true;
}

void Catalog::loadExtraFiles(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = pathToUtf8(it->path().filename());
        // Hidden files include the "._name.json" copies macOS leaves on some drives.
        std::error_code fileEc;
        if (name.empty() || name[0] == '.' || it->path().extension() != ".json" || !it->is_regular_file(fileEc)) continue;
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        std::string error;
        std::ifstream in(f, std::ios::binary);
        std::stringstream text;
        if (in) text << in.rdbuf();
        if (!in) error = "cannot be read";
        else if (mergeExtra(text.str(), error)) continue;
        warnings_.push_back(pathToUtf8(f) + ": " + error);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[catalog] skipped %s", warnings_.back().c_str());
    }
}

bool Catalog::mergeExtra(const std::string& extraJson, std::string& error) {
    // Everything is parsed and checked first, so a bad file leaves the catalog as it was.
    std::vector<BackendDef> added;
    std::vector<std::pair<GameDef*, GameBackendOption>> options;
    try {
        json j = json::parse(extraJson, nullptr, true, true);
        if (!j.is_object()) throw std::runtime_error("not a JSON object");
        if (auto it = j.find("schema"); it != j.end() && !it->is_number_integer())
            throw std::runtime_error("\"schema\" is not a number");
        if (const int schema = jget(j, "schema", 1); schema != 1)
            throw std::runtime_error("schema " + std::to_string(schema) + " is not supported (only 1)");
        if (auto it = j.find("backends"); it != j.end()) {
            if (!it->is_array()) throw std::runtime_error("\"backends\" is not a list");
            for (auto& bj : *it) {
                if (!bj.is_object()) throw std::runtime_error("a backend is not an object");
                if (auto s = bj.find("sources"); s != bj.end() && !s->is_object())
                    throw std::runtime_error("\"sources\" of a backend is not an object");
                added.push_back(parseBackend(bj));
                if (added.back().id.empty()) throw std::runtime_error("a backend has no \"id\"");
            }
        }
        if (auto it = j.find("gameOptions"); it != j.end()) {
            if (!it->is_object()) throw std::runtime_error("\"gameOptions\" is not an object");
            for (auto& [gameId, list] : it->items()) {
                auto g = std::find_if(games_.begin(), games_.end(), [&](const GameDef& d) { return d.id == gameId; });
                if (g == games_.end()) throw std::runtime_error("unknown game " + gameId);
                if (!list.is_array()) throw std::runtime_error("the options for " + gameId + " are not a list");
                for (auto& oj : list) {
                    if (!oj.is_object()) throw std::runtime_error("an option for " + gameId + " is not an object");
                    if (auto os = oj.find("os"); os != oj.end() && !os->is_array())
                        throw std::runtime_error("\"os\" of an option for " + gameId + " is not a list");
                    options.emplace_back(&*g, parseOption(oj));
                }
            }
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    for (auto& [g, o] : options) {
        const bool known = backend(o.backend) ||
                           std::any_of(added.begin(), added.end(), [&](const BackendDef& b) { return b.id == o.backend; });
        if (!known) {
            error = "an option for " + g->id + " uses unknown backend \"" + o.backend + "\"";
            return false;
        }
    }
    for (auto& b : added) {
        auto it = std::find_if(backends_.begin(), backends_.end(), [&](const BackendDef& d) { return d.id == b.id; });
        if (it != backends_.end()) *it = std::move(b);
        else backends_.push_back(std::move(b));
    }
    for (auto& [g, o] : options) g->backends.push_back(std::move(o));
    return true;
}

const GameDef* Catalog::game(const std::string& id) const {
    for (auto& g : games_)
        if (g.id == id) return &g;
    return nullptr;
}

const BackendDef* Catalog::backend(const std::string& id) const {
    for (auto& b : backends_)
        if (b.id == id) return &b;
    return nullptr;
}

std::vector<const GameBackendOption*> Catalog::optionsFor(const GameDef& g, Os os) const {
    std::vector<const GameBackendOption*> out;
    for (auto& o : g.backends)
        if (std::find(o.os.begin(), o.os.end(), os) != o.os.end()) out.push_back(&o);
    std::stable_sort(out.begin(), out.end(),
                     [](const GameBackendOption* a, const GameBackendOption* b) { return a->priority > b->priority; });
    return out;
}

std::string currentPlatformKey() {
    return std::string(osId(currentOs())) + "-" + cpuArch();
}

fs::path extraCatalogDir() {
    return paths().data / "catalog";
}

}  // namespace rrm
