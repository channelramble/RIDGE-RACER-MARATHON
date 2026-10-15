#include "core/Scanner.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <system_error>

#include "core/Platform.h"
#include "core/RomSet.h"
#include "core/formats/Formats.h"

namespace rrm {

namespace {

constexpr size_t kMaxWalkEntries = 500000;           // bounds a scan of a huge tree
constexpr uint64_t kMaxRomsetScanBytes = 512ull << 20;  // CRC budget per folder during a scan
constexpr size_t kMaxFolderSizeEntries = 200000;

using StringList = std::vector<std::string> IdentifySpec::*;

std::string lowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Upper-case alphanumerics only: "SCUS-943.00", "scus_943.00" and "SCUS94300" compare equal.
std::string normId(std::string_view s) {
    std::string o;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) o += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

std::string extOf(const fs::path& p) { return lowerStr(pathToUtf8(p.extension())); }

uint64_t fileSizeOf(const fs::path& p) {
    std::error_code ec;
    const auto s = fs::file_size(p, ec);
    return ec ? 0 : static_cast<uint64_t>(s);
}

std::string joinNames(const std::vector<std::string>& names, size_t maxShown) {
    std::string s;
    for (size_t i = 0; i < names.size() && i < maxShown; ++i) s += (i ? ", " : "") + names[i];
    if (names.size() > maxShown) s += ", ...";
    return s;
}

bool isCancelled(const std::atomic<bool>* c) { return c && c->load(std::memory_order_relaxed); }
bool isCancelled(const ScanProgress* p) { return p && p->cancel.load(std::memory_order_relaxed); }

fs::path childCI(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    const std::string want = lowerStr(name);
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (int budget = 20000; !ec && it != end && budget-- > 0; it.increment(ec))
        if (lowerStr(pathToUtf8(it->path().filename())) == want) return it->path();
    return {};
}

uint64_t folderSize(const fs::path& dir) {
    uint64_t total = 0;
    size_t n = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && ++n < kMaxFolderSizeEntries; it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec)) total += it->file_size(fec);
    }
    return total;
}

// ---- catalog lookups -------------------------------------------------------------------

const GameDef* findById(const Catalog& cat, StringList field, const std::string& id) {
    const std::string n = normId(id);
    if (n.empty()) return nullptr;
    for (const auto& g : cat.games())
        for (const auto& s : g.identify.*field)
            if (normId(s) == n) return &g;
    return nullptr;
}

// Longest matching hint wins ("ridge racer type 4" beats a shorter hint).
const GameDef* findByNameHint(const Catalog& cat, const std::string& fileName) {
    const std::string name = lowerStr(fileName);
    const GameDef* best = nullptr;
    size_t bestLen = 0;
    for (const auto& g : cat.games())
        for (const auto& h : g.identify.nameHints) {
            const std::string hint = lowerStr(h);
            if (!hint.empty() && hint.size() > bestLen && name.find(hint) != std::string::npos) {
                best = &g;
                bestLen = hint.size();
            }
        }
    return best;
}

bool isArcade(const GameDef& g) {
    return !g.identify.dumps.empty() && (g.mediaKind == "romset" || !g.identify.mameSet.empty());
}

void setHit(ScanHit& h, const GameDef& g, const std::string& path, const std::string& format,
            const std::string& detectedBy, uint64_t size) {
    h.gameId = g.id;
    h.files.path = path;
    h.files.format = format;
    h.files.detectedBy = detectedBy;
    h.files.size = size;
}

// ---- discs -------------------------------------------------------------------------------

struct DiscProbe {
    std::string kind;  // "boot" (SYSTEM.CNF), "disc id" (PSP), "title id" (PS3)
    std::string id;
};

const fmt::IsoEntry* findEntry(const std::vector<fmt::IsoEntry>& list, const char* name) {
    for (const auto& e : list)
        if (lowerStr(e.name) == lowerStr(name)) return &e;
    return nullptr;
}

// Reads a handful of sectors: PVD, root directory and SYSTEM.CNF or PARAM.SFO.
bool probeIso(fmt::SectorReader& r, DiscProbe& out) {
    fmt::Iso9660 iso(r);
    std::vector<fmt::IsoEntry> root;
    if (!iso.mount() || !iso.list("/", root)) return false;
    std::string data;
    if (auto* e = findEntry(root, "SYSTEM.CNF"); e && !e->directory && iso.readFile(*e, data, 64 * 1024)) {
        out.id = fmt::bootSerialFromSystemCnf(data);
        if (!out.id.empty()) {
            out.kind = "boot";
            return true;
        }
    }
    auto sfoValue = [&](const char* dir, const char* key) -> std::string {
        fmt::IsoEntry sfo;
        std::vector<std::pair<std::string, std::string>> kv;
        if (iso.find(std::string("/") + dir + "/PARAM.SFO", sfo) && !sfo.directory &&
            iso.readFile(sfo, data, 1 << 20) && fmt::parseSfo(data, kv))
            return fmt::sfoValue(kv, key);
        return {};
    };
    if (auto* e = findEntry(root, "PSP_GAME"); e && e->directory) {
        out.id = sfoValue("PSP_GAME", "DISC_ID");
        if (!out.id.empty()) {
            out.kind = "disc id";
            return true;
        }
    }
    // UMD_DATA.BIN starts with "UCES-00422|...".
    if (auto* e = findEntry(root, "UMD_DATA.BIN"); e && !e->directory && iso.readFile(*e, data, 4096)) {
        out.id = normId(data.substr(0, data.find('|')));
        if (out.id.size() == 9) {
            out.kind = "disc id";
            return true;
        }
    }
    if (auto* e = findEntry(root, "PS3_GAME"); e && e->directory) {
        out.id = sfoValue("PS3_GAME", "TITLE_ID");
        if (!out.id.empty()) {
            out.kind = "title id";
            return true;
        }
    }
    out = DiscProbe{};
    return false;
}

const GameDef* gameForProbe(const Catalog& cat, const DiscProbe& p) {
    if (p.kind == "boot") return findById(cat, &IdentifySpec::bootSerials, p.id);
    if (p.kind == "disc id" || p.kind == "title id") return findById(cat, &IdentifySpec::discIds, p.id);
    return nullptr;
}

bool identifyCue(const Catalog& cat, const std::string& path, ScanHit& out) {
    out.files.path = path;
    out.files.format = "cue";
    fmt::CueSheet cue;
    std::string err;
    if (!fmt::parseCue(path, cue, &err)) {
        out.message = "invalid cue sheet: " + err;
        return false;
    }
    std::set<std::string> seen;
    for (const auto& t : cue.tracks)
        if (seen.insert(t.file).second) out.files.size += fileSizeOf(utf8ToPath(t.file));
    std::string warning;
    if (!cue.missingFiles.empty()) {
        std::vector<std::string> names;
        for (const auto& m : cue.missingFiles) names.push_back(pathToUtf8(utf8ToPath(m).filename()));
        warning = "missing " + std::to_string(names.size()) + " track file(s): " + joinNames(names, 3);
    }
    DiscProbe probe;
    auto reader = fmt::SectorReader::open(path);
    if (reader && probeIso(*reader, probe)) {
        out.serial = probe.id;
        const GameDef* g = gameForProbe(cat, probe);
        if (!g) {
            out.message = "not a catalog game (" + probe.id + ")";
            return false;
        }
        setHit(out, *g, path, "cue", probe.kind + " " + probe.id, out.files.size);
        out.message = warning;
        return true;
    }
    // The data track is missing: still report the sheet by name so the user sees why it fails.
    if (!cue.missingFiles.empty()) {
        if (const GameDef* g = findByNameHint(cat, pathToUtf8(utf8ToPath(path).filename()))) {
            setHit(out, *g, path, "cue", "file name", out.files.size);
            out.message = warning;
            return true;
        }
    }
    out.message = warning.empty() ? "no PlayStation data track" : warning;
    return false;
}

bool identifyImage(const Catalog& cat, const std::string& path, const std::string& ext, ScanHit& out) {
    const std::string format = ext.substr(1);
    out.files.path = path;
    out.files.format = format;
    out.files.size = fileSizeOf(utf8ToPath(path));
    DiscProbe probe;
    if (auto reader = fmt::SectorReader::open(path); reader && probeIso(*reader, probe)) {
        out.serial = probe.id;
        if (const GameDef* g = gameForProbe(cat, probe)) {
            setHit(out, *g, path, format, probe.kind + " " + probe.id, out.files.size);
            return true;
        }
        out.message = "not a catalog game (" + probe.id + ")";
        return false;
    }
    if (ext == ".iso") {  // Xbox 360 discs also carry a small ISO9660 video partition, so check last
        std::string titleId, mediaId;
        if (fmt::xbox360TitleId(path, titleId, &mediaId)) {
            out.serial = titleId;
            if (const GameDef* g = findById(cat, &IdentifySpec::xboxTitleIds, titleId)) {
                setHit(out, *g, path, format, "title id " + titleId, out.files.size);
                return true;
            }
            out.message = "not a catalog game (Xbox 360 title " + titleId + ")";
        }
    }
    return false;
}

// .chd/.cso/.pbp are compressed containers we do not parse: file (or folder) name only.
bool identifyByName(const Catalog& cat, const std::string& path, const std::string& ext, ScanHit& out) {
    const fs::path p = utf8ToPath(path);
    const std::string format = ext.substr(1);
    out.files.path = path;
    out.files.format = format;
    out.files.size = fileSizeOf(p);
    const GameDef* g = findByNameHint(cat, pathToUtf8(p.filename()));
    if (!g) g = findByNameHint(cat, pathToUtf8(p.parent_path().filename()));
    if (!g) return false;
    setHit(out, *g, path, format, "file name", out.files.size);
    const auto& acc = g->acceptedFormats;
    if (!acc.empty() && std::find(acc.begin(), acc.end(), format) == acc.end())
        out.message = format + " is not a supported format for " + g->title;
    return true;
}

// ---- PS3 / Xbox 360 game folders -------------------------------------------------------------

bool identifyPs3Folder(const Catalog& cat, const fs::path& dir, ScanHit& out) {
    fs::path gameDir = dir, sfo;
    std::error_code ec;
    if (lowerStr(pathToUtf8(dir.filename())) == "ps3_game") {
        sfo = childCI(dir, "PARAM.SFO");
        gameDir = dir.parent_path();
    } else if (const fs::path ps3 = childCI(dir, "PS3_GAME"); !ps3.empty() && fs::is_directory(ps3, ec)) {
        sfo = childCI(ps3, "PARAM.SFO");
    }
    if (sfo.empty() || fileSizeOf(sfo) > (1u << 20)) return false;
    std::ifstream in(sfo, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::pair<std::string, std::string>> kv;
    if (!fmt::parseSfo(data, kv)) return false;
    const std::string id = fmt::sfoValue(kv, "TITLE_ID");
    if (id.empty()) return false;
    out.serial = id;
    const GameDef* g = findById(cat, &IdentifySpec::discIds, id);
    if (!g) {
        out.message = "not a catalog game (PS3 " + id + ")";
        return false;
    }
    setHit(out, *g, pathToUtf8(gameDir), "ps3-folder", "title id " + id, folderSize(gameDir));
    return true;
}

bool identifyXexFolder(const Catalog& cat, const fs::path& dir, ScanHit& out) {
    if (childCI(dir, "default.xex").empty()) return false;
    std::string titleId, mediaId;
    if (!fmt::xbox360TitleId(pathToUtf8(dir), titleId, &mediaId)) return false;
    out.serial = titleId;
    const GameDef* g = findById(cat, &IdentifySpec::xboxTitleIds, titleId);
    if (!g) {
        out.message = "not a catalog game (Xbox 360 title " + titleId + ")";
        return false;
    }
    setHit(out, *g, pathToUtf8(dir), "xex-folder", "title id " + titleId, folderSize(dir));
    return true;
}

// ---- MAME romsets -------------------------------------------------------------------------------

struct RomMatch {
    const GameDef* game = nullptr;
    size_t entries = 0, entriesFound = 0;    // catalog entries (duplicate CRCs included)
    size_t distinct = 0, distinctFound = 0;  // distinct CRCs
    std::vector<std::string> missing;        // catalog names without a source file
    bool identified() const { return game && distinct && distinctFound * 10 >= distinct * 9; }
    double ratio() const { return distinct ? double(distinctFound) / double(distinct) : 0.0; }
};

RomMatch matchRoms(const GameDef& g, const std::vector<RomFile>& files) {
    RomMatch m;
    m.game = &g;
    std::set<std::pair<uint32_t, uint64_t>> have;
    std::set<uint32_t> haveCrc;
    for (const auto& f : files)
        if (f.hasCrc) {
            have.insert({f.crc32, f.size});
            haveCrc.insert(f.crc32);
        }
    std::set<uint32_t> distinct, found;
    for (const auto& d : g.identify.dumps) {
        uint32_t crc = 0;
        if (!parseCrc32(d.crc32, crc)) continue;
        ++m.entries;
        distinct.insert(crc);
        if (d.size ? have.count({crc, d.size}) > 0 : haveCrc.count(crc) > 0) {
            ++m.entriesFound;
            found.insert(crc);
        } else {
            m.missing.push_back(d.file);
        }
    }
    m.distinct = distinct.size();
    m.distinctFound = found.size();
    return m;
}

// Before reading anything: are there enough files with catalog sizes to reach 90%?
bool plausibleRomset(const GameDef& g, const std::vector<RomFile>& files) {
    std::set<uint64_t> sizes;
    std::set<uint32_t> distinct;
    for (const auto& d : g.identify.dumps) {
        uint32_t crc = 0;
        if (!parseCrc32(d.crc32, crc)) continue;
        distinct.insert(crc);
        sizes.insert(d.size);
    }
    size_t candidates = 0;
    for (const auto& f : files)
        if (sizes.count(f.size) || sizes.count(0)) ++candidates;
    return !distinct.empty() && candidates * 10 >= distinct.size() * 9;
}

bool identifyRomSource(const Catalog& cat, const std::string& path, std::vector<RomFile>& files, bool isZip,
                       uint64_t totalSize, ScanHit& out, const ScanProgress* progress) {
    if (!isZip) {
        std::set<uint64_t> sizes;
        for (const auto& g : cat.games())
            if (isArcade(g) && plausibleRomset(g, files))
                for (const auto& d : g.identify.dumps) sizes.insert(d.size);
        if (sizes.empty()) return false;
        uint64_t budget = kMaxRomsetScanBytes;
        computeRomCrcs(
            files,
            [&](const RomFile& f) {
                if (!(sizes.count(f.size) || sizes.count(0)) || f.size > budget) return false;
                budget -= f.size;
                return true;
            },
            progress ? &progress->cancel : nullptr);
    }
    RomMatch best;
    for (const auto& g : cat.games()) {
        if (!isArcade(g)) continue;
        RomMatch m = matchRoms(g, files);
        if (m.identified() && m.ratio() > best.ratio()) best = std::move(m);
    }
    if (!best.game) return false;
    setHit(out, *best.game, path, isZip ? "romset-zip" : "romset-dir",
           "mame crc " + std::to_string(best.entriesFound) + "/" + std::to_string(best.entries), totalSize);
    const auto& dumps = best.game->identify.dumps;
    out.files.verified = best.distinctFound == best.distinct ? "match:" + dumps.front().label : "partial";
    if (!best.missing.empty()) out.message = "missing " + std::to_string(best.missing.size()) + ": " + joinNames(best.missing, 6);
    return true;
}

bool identifyRomZip(const Catalog& cat, const std::string& path, ScanHit& out) {
    std::vector<RomFile> files;
    if (!listRomSource(path, files)) return false;
    return identifyRomSource(cat, path, files, true, fileSizeOf(utf8ToPath(path)), out, nullptr);
}

// ---- folder walk -----------------------------------------------------------------------------

struct WalkFile {
    fs::path path;
    std::string name, ext;
    uint64_t size = 0;
};
struct WalkDir {
    fs::path path;
    std::vector<WalkFile> files;
    bool ps3 = false;  // has PS3_GAME/PARAM.SFO
    bool xex = false;  // has default.xex
};

bool skipDir(const std::string& lname) {
    auto endsWith = [&](std::string_view s) {
        return lname.size() >= s.size() && lname.compare(lname.size() - s.size(), s.size(), s) == 0;
    };
    return endsWith(".app") || lname == "node_modules" || lname == "__macosx" || lname == "$recycle.bin" ||
           lname == "system volume information";
}

std::vector<WalkDir> walk(const fs::path& root, int maxDepth, ScanProgress* progress) {
    std::vector<WalkDir> dirs;
    std::set<std::string> visited;  // canonical paths: symlink loops and aliases are entered once
    std::vector<std::pair<fs::path, int>> stack{{root, 0}};
    size_t entries = 0;
    while (!stack.empty() && entries < kMaxWalkEntries && !isCancelled(progress)) {
        auto [dir, depth] = std::move(stack.back());
        stack.pop_back();
        std::error_code ec;
        const fs::path canon = fs::canonical(dir, ec);
        if (!visited.insert(pathToUtf8(ec ? dir : canon)).second) continue;

        WalkDir wd;
        wd.path = dir;
        std::vector<fs::path> subdirs;
        fs::path ps3Game;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && ++entries < kMaxWalkEntries; it.increment(ec)) {
            const fs::path& p = it->path();
            const std::string name = pathToUtf8(p.filename());
            if (name.empty() || name[0] == '.') continue;  // hidden entries, AppleDouble "._x" files
            const std::string lname = lowerStr(name);
            std::error_code fec;
            if (it->is_directory(fec)) {
                if (lname == "ps3_game") ps3Game = p;
                if (depth < maxDepth && !skipDir(lname)) subdirs.push_back(p);
            } else if (it->is_regular_file(fec)) {
                WalkFile f;
                f.path = p;
                f.name = name;
                f.ext = lowerStr(pathToUtf8(p.extension()));
                f.size = it->file_size(fec);
                if (fec) continue;
                if (lname == "default.xex") wd.xex = true;
                wd.files.push_back(std::move(f));
                if (progress) progress->filesSeen.fetch_add(1, std::memory_order_relaxed);
            }
        }
        wd.ps3 = !ps3Game.empty() && !childCI(ps3Game, "PARAM.SFO").empty();
        // PS3 and extracted Xbox 360 games are units: their thousands of data files are not scanned.
        if (wd.ps3 || wd.xex) subdirs.clear();
        std::sort(wd.files.begin(), wd.files.end(), [](const WalkFile& a, const WalkFile& b) { return a.name < b.name; });
        std::sort(subdirs.rbegin(), subdirs.rend());  // popped in name order
        for (auto& s : subdirs) stack.emplace_back(std::move(s), depth + 1);
        dirs.push_back(std::move(wd));
    }
    return dirs;
}

void setCurrent(ScanProgress* progress, const fs::path& p) {
    if (!progress) return;
    std::string s = pathToUtf8(p);
    if (s.size() > 1000) s.resize(1000);
    progress->current.assign(s);  // capacity is reserved up front: no reallocation while the UI reads
}

// ---- verification ------------------------------------------------------------------------------

bool sameCrc(const std::string& catalogHex, const std::string& ours) {
    uint32_t a = 0, b = 0;
    return parseCrc32(catalogHex, a) && parseCrc32(ours, b) && a == b;
}

const KnownDump* matchDump(const GameDef& g, const fmt::Digests& d) {
    for (const auto& k : g.identify.dumps) {
        if (k.crc32.empty() && k.md5.empty() && k.sha1.empty()) continue;
        if (k.size && k.size != d.size) continue;
        if (!k.crc32.empty() && !sameCrc(k.crc32, d.crc32)) continue;
        if (!k.md5.empty() && lowerStr(k.md5) != d.md5) continue;
        if (!k.sha1.empty() && lowerStr(k.sha1) != d.sha1) continue;
        return &k;
    }
    return nullptr;
}

void verifyRomSet(const GameDef& g, ScanHit& hit, std::atomic<bool>* cancel,
                  const std::function<void(uint64_t, uint64_t)>& onProgress) {
    std::vector<RomFile> files;
    std::string err;
    if (!listRomSource(hit.files.path, files, &err)) {
        hit.message = err;
        return;
    }
    const auto& dumps = g.identify.dumps;
    std::set<uint64_t> sizes;
    for (const auto& d : dumps) sizes.insert(d.size);
    if (!computeRomCrcs(files, [&](const RomFile& f) { return sizes.count(f.size) || sizes.count(0); }, cancel)) {
        hit.message = "verification cancelled";
        return;
    }
    // One check per distinct CRC; duplicates in the set share the same data.
    std::map<uint32_t, const KnownDump*> wanted;
    for (const auto& d : dumps) {
        uint32_t crc = 0;
        if (parseCrc32(d.crc32, crc)) wanted.emplace(crc, &d);
    }
    std::vector<std::pair<const KnownDump*, const RomFile*>> todo;
    std::vector<std::string> missing, bad;
    uint64_t total = 0, done = 0;
    for (const auto& entry : wanted) {
        const uint32_t crc = entry.first;
        const KnownDump* d = entry.second;
        auto it = std::find_if(files.begin(), files.end(), [&](const RomFile& f) {
            return f.hasCrc && f.crc32 == crc && (d->size == 0 || f.size == d->size);
        });
        if (it == files.end()) {
            missing.push_back(d->file);
            continue;
        }
        todo.emplace_back(d, &*it);
        total += it->size;
    }
    for (const auto& [d, f] : todo) {
        if (isCancelled(cancel)) {
            hit.message = "verification cancelled";
            return;
        }
        bool ok = false;
        if (!f->path.empty()) {
            fmt::Digests dg;
            const uint64_t base = done;
            auto progress = [&](uint64_t n, uint64_t) {
                if (onProgress) onProgress(base + n, total);
            };
            if (!fmt::hashFile(f->path, dg, cancel, progress)) {
                if (isCancelled(cancel)) {
                    hit.message = "verification cancelled";
                    return;
                }
            } else {
                ok = (d->size == 0 || dg.size == d->size) && sameCrc(d->crc32, dg.crc32) &&
                     (d->sha1.empty() || lowerStr(d->sha1) == dg.sha1) && (d->md5.empty() || lowerStr(d->md5) == dg.md5);
            }
        } else {
            std::string bytes;  // readRomFile inflates the entry and checks its CRC32
            ok = readRomFile(hit.files.path, *f, bytes) && (d->size == 0 || bytes.size() == d->size);
        }
        if (!ok) bad.push_back(d->file);
        done += f->size;
        if (onProgress) onProgress(done, total);
    }
    if (!bad.empty()) {
        hit.files.verified = "mismatch";
        hit.message = "bad ROM data: " + joinNames(bad, 6);
    } else if (!missing.empty()) {
        hit.files.verified = "partial";
        hit.message = "missing " + std::to_string(missing.size()) + ": " + joinNames(missing, 6);
    } else {
        hit.files.verified = "match:" + dumps.front().label;
        hit.message.clear();
    }
}

}  // namespace

bool identifyPath(const Catalog& catalog, const std::string& path, ScanHit& out) {
    out = ScanHit{};
    try {
        const fs::path p = utf8ToPath(path);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            if (identifyPs3Folder(catalog, p, out) || identifyXexFolder(catalog, p, out)) return true;
            std::vector<RomFile> files;
            if (listRomSource(path, files)) {
                uint64_t total = 0;
                for (const auto& f : files) total += f.size;
                ScanHit rom;
                if (identifyRomSource(catalog, path, files, false, total, rom, nullptr)) {
                    out = std::move(rom);
                    return true;
                }
            }
            // Not a game container itself (e.g. a folder holding a cue sheet): look one level down.
            auto hits = scanFolder(catalog, path, nullptr, 1);
            if (!hits.empty()) {
                out = std::move(hits.front());
                return true;
            }
            return false;
        }
        if (!fs::is_regular_file(p, ec)) {
            out.message = "file not found";
            return false;
        }
        const std::string ext = extOf(p);
        const std::string name = lowerStr(pathToUtf8(p.filename()));
        // Files that stand for their game folder.
        if (name == "param.sfo") return identifyPs3Folder(catalog, p.parent_path(), out);
        if (name == "eboot.bin") return identifyPs3Folder(catalog, p.parent_path().parent_path(), out);
        if (ext == ".xex") return identifyXexFolder(catalog, p.parent_path(), out);
        if (ext == ".cue") return identifyCue(catalog, path, out);
        if (ext == ".iso" || ext == ".bin" || ext == ".img") return identifyImage(catalog, path, ext, out);
        if (ext == ".chd" || ext == ".cso" || ext == ".pbp") return identifyByName(catalog, path, ext, out);
        if (ext == ".zip") return identifyRomZip(catalog, path, out);
    } catch (...) {
        out.gameId.clear();
    }
    return false;
}

std::vector<ScanHit> scanFolder(const Catalog& catalog, const std::string& folder, ScanProgress* progress,
                                int maxDepth) {
    std::vector<ScanHit> hits;
    try {
        if (progress && progress->current.capacity() < 1024) progress->current.reserve(1024);
        std::error_code ec;
        fs::path root = utf8ToPath(folder);
        if (fs::path abs = fs::absolute(root, ec); !ec) root = abs.lexically_normal();
        if (!fs::is_directory(root, ec)) {
            ScanHit h;
            if (identifyPath(catalog, folder, h)) hits.push_back(std::move(h));
            return hits;
        }
        const std::vector<WalkDir> dirs = walk(root, std::max(0, maxDepth), progress);

        // Cue sheets claim their track files so the BIN tracks are not reported again.
        std::set<std::string> claimed;
        for (const auto& d : dirs)
            for (const auto& f : d.files) {
                fmt::CueSheet cue;
                if (f.ext == ".cue" && fmt::parseCue(pathToUtf8(f.path), cue))
                    for (const auto& t : cue.tracks) claimed.insert(lowerStr(t.file));
            }

        auto add = [&](ScanHit& h) {
            hits.push_back(std::move(h));
            if (progress) progress->hits.fetch_add(1, std::memory_order_relaxed);
        };
        for (const auto& d : dirs) {
            if (isCancelled(progress)) break;
            setCurrent(progress, d.path);
            ScanHit h;
            if (d.ps3 && identifyPs3Folder(catalog, d.path, h)) add(h);
            if (d.xex) {
                if (ScanHit x; identifyXexFolder(catalog, d.path, x)) add(x);
                continue;  // an extracted Xbox 360 game: its files are data, not separate media
            }
            uint64_t dirBytes = 0;
            std::vector<RomFile> roms;
            for (const auto& f : d.files) {
                if (isCancelled(progress)) break;
                dirBytes += f.size;
                RomFile r;
                r.name = f.name;
                r.path = pathToUtf8(f.path);
                r.size = f.size;
                roms.push_back(r);

                const std::string p = r.path;
                ScanHit fh;
                bool found = false;
                if (f.ext == ".cue") found = identifyCue(catalog, p, fh);
                else if (f.ext == ".iso" || f.ext == ".bin" || f.ext == ".img")
                    found = !claimed.count(lowerStr(p)) && identifyImage(catalog, p, f.ext, fh);
                else if (f.ext == ".chd" || f.ext == ".cso" || f.ext == ".pbp")
                    found = identifyByName(catalog, p, f.ext, fh);
                else if (f.ext == ".zip")
                    found = identifyRomZip(catalog, p, fh);
                if (found) add(fh);
            }
            // Loose ROM files directly inside this folder form one romset.
            ScanHit rh;
            if (!roms.empty() && identifyRomSource(catalog, pathToUtf8(d.path), roms, false, dirBytes, rh, progress))
                add(rh);
        }

        std::map<std::string, size_t> order;
        for (size_t i = 0; i < catalog.games().size(); ++i) order[catalog.games()[i].id] = i;
        std::stable_sort(hits.begin(), hits.end(), [&](const ScanHit& a, const ScanHit& b) {
            const size_t oa = order.count(a.gameId) ? order[a.gameId] : SIZE_MAX;
            const size_t ob = order.count(b.gameId) ? order[b.gameId] : SIZE_MAX;
            return oa != ob ? oa < ob : a.files.path < b.files.path;
        });
    } catch (...) {
    }
    return hits;
}

void verifyHit(const Catalog& catalog, ScanHit& hit, std::atomic<bool>* cancel,
               std::function<void(uint64_t done, uint64_t total)> onProgress) {
    try {
        const GameDef* g = catalog.game(hit.gameId);
        if (!g) {
            hit.message = "unknown game";
            return;
        }
        const std::string& format = hit.files.format;
        if (format == "romset-dir" || format == "romset-zip") {
            verifyRomSet(*g, hit, cancel, onProgress);
            return;
        }
        std::string file;
        if (format == "cue") {
            // Catalog hashes describe track 1 (the data track) of the disc.
            fmt::CueSheet cue;
            std::string err;
            if (!fmt::parseCue(hit.files.path, cue, &err)) {
                hit.message = err;
                return;
            }
            for (const auto& t : cue.tracks)
                if (t.mode != "AUDIO") {
                    file = t.file;
                    break;
                }
            if (file.empty()) {
                hit.message = "cue sheet has no data track";
                return;
            }
        } else if (format == "iso" || format == "bin" || format == "img") {
            file = hit.files.path;
        } else {
            hit.message = "cannot verify " + format + " files";
            return;
        }
        fmt::Digests d;
        if (!fmt::hashFile(file, d, cancel, onProgress)) {
            hit.message = isCancelled(cancel) ? "verification cancelled" : "cannot read " + file;
            return;
        }
        if (const KnownDump* m = matchDump(*g, d)) {
            hit.files.verified = "match:" + m->label;
            hit.message.clear();
        } else {
            hit.files.verified = "mismatch";
            hit.message = "crc32 " + d.crc32 + " matches no known dump";
        }
    } catch (...) {
        hit.message = "verification failed";
    }
}

std::string identifySystemFile(const std::string& path) {
    try {
        const fs::path p = utf8ToPath(path);
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return {};
        const uint64_t size = fileSizeOf(p);
        std::ifstream in(p, std::ios::binary);
        if (!in) return {};
        auto head = [&](size_t n) {
            std::string s(static_cast<size_t>(std::min<uint64_t>(n, size)), '\0');
            in.read(s.data(), static_cast<std::streamsize>(s.size()));
            s.resize(static_cast<size_t>(in.gcount()));
            return s;
        };
        if (extOf(p) == ".pup") return head(5) == "SCEUF" ? "ps3-firmware" : "";
        if (size == 512 * 1024) {
            // Sony kernels carry "CEX-..." at 0x108 and/or a Sony copyright string; 512 KiB
            // MAME ROMs and OpenBIOS builds do not.
            const std::string rom = head(size);
            if (rom.size() == size && (rom.compare(0x108, 4, "CEX-") == 0 ||
                                       rom.find("Sony Computer Entertainment") != std::string::npos))
                return "ps1-bios";
            return {};
        }
        if (size == 4u << 20 || size == 8u << 20) {
            // PS2 ROMDIR: 16-byte records, the first named "RESET".
            const std::string h = head(0x4000);
            for (size_t o = 0; o + 16 <= h.size(); o += 16)
                if (h.compare(o, 6, std::string("RESET\0", 6)) == 0) return "ps2-bios";
        }
    } catch (...) {
    }
    return {};
}

}  // namespace rrm
