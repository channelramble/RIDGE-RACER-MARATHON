// Shared by the PSP backends (PPSSPP standalone and the in-app PPSSPP core): disc ids from
// .iso/.cso images, the European discs' widescreen codes, and HD texture pack folders.
// Header-only; included by those backend .cpp files only.
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "backends/EmuCommon.h"
#include "core/formats/Formats.h"
#include "miniz/miniz.h"

namespace rrm::psp {

using namespace emu;

inline constexpr double kNativeAspect = 480.0 / 272.0;  // the PSP screen (1.7647)

// CWCheat widescreen codes: both addresses take the projection aspect (width/height) as a float.
struct WideCheat {
    const char* discId;  // PARAM.SFO DISC_ID (no dash)
    const char* label;   // "_S" line
    const char* title;   // "_G" line
    uint32_t addr[2];
};
inline constexpr WideCheat kWideCheats[] = {
    {"UCES00002", "UCES-00002", "Ridge Racer [EU]", {0x201A6444, 0x201A9258}},
    {"UCES00422", "UCES-00422", "Ridge Racer 2 [EU]", {0x201B35A4, 0x201B6638}},
};
inline constexpr const char* kCheatName = "Marathon widescreen";
// Put under another cheat's "_C" line when Marathon switches it off for its own code, so that it
// is switched back on when Marathon's code goes. PPSSPP skips "//" lines and keeps them on saving.
inline constexpr const char* kSwitchedOff = "// Marathon: switched off while Marathon widescreen is on";

// Texture packs made for one disc that also fit another (the RR2 HD HUD pack's README:
// UCAS40119 and UCES00422 share their textures; the Japanese discs do not).
inline constexpr const char* kTextureAliases[][2] = {
    {"UCES00422", "UCAS40119"},
    {"UCAS40119", "UCES00422"},
};

inline std::string normId(const std::string& s) {
    std::string o;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) o += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

inline const WideCheat* wideCheatFor(const std::string& discId) {
    for (const auto& c : kWideCheats)
        if (discId == c.discId) return &c;
    return nullptr;
}

// ---- disc id -------------------------------------------------------------------------------------

// CISO v0/v1 (.cso): deflate-compressed blocks behind an index table, read the way
// PPSSPP's CISOFileBlockDevice does. Just enough to reach PARAM.SFO.
class CsoReader : public fmt::SectorReader {
public:
    static std::unique_ptr<fmt::SectorReader> open(const std::string& path) {
        std::unique_ptr<CsoReader> r(new CsoReader());
        r->in_.open(utf8ToPath(path), std::ios::binary);
        unsigned char h[24] = {};
        if (!r->in_ || !r->in_.read(reinterpret_cast<char*>(h), sizeof(h)) || std::memcmp(h, "CISO", 4) != 0) return nullptr;
        auto le32 = [&](int o) {
            return uint32_t(h[o]) | uint32_t(h[o + 1]) << 8 | uint32_t(h[o + 2]) << 16 | uint32_t(h[o + 3]) << 24;
        };
        r->total_ = uint64_t(le32(8)) | uint64_t(le32(12)) << 32;
        r->block_ = le32(16);
        r->align_ = h[21];
        if (h[20] > 1 || r->block_ < 2048 || r->block_ > (1u << 20) || (r->block_ & (r->block_ - 1)) || r->total_ == 0 ||
            r->align_ > 16)
            return nullptr;
        const uint64_t frames = (r->total_ + r->block_ - 1) / r->block_;
        if (frames > (1u << 24)) return nullptr;
        std::vector<unsigned char> raw(static_cast<size_t>(frames + 1) * 4);
        if (!r->in_.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) return nullptr;
        r->index_.resize(static_cast<size_t>(frames + 1));
        for (size_t i = 0; i < r->index_.size(); ++i)
            r->index_[i] = uint32_t(raw[i * 4]) | uint32_t(raw[i * 4 + 1]) << 8 | uint32_t(raw[i * 4 + 2]) << 16 |
                           uint32_t(raw[i * 4 + 3]) << 24;
        return r;
    }

    bool read(uint32_t lba, void* out2048) override {
        const uint64_t offset = uint64_t(lba) * 2048;
        if (offset + 2048 > total_) return false;
        const uint64_t frame = offset / block_;
        if (static_cast<int64_t>(frame) != cached_ && !load(frame)) return false;
        std::memcpy(out2048, frame_.data() + (offset % block_), 2048);
        return true;
    }
    uint64_t sectorCount() const override { return total_ / 2048; }

private:
    bool load(uint64_t frame) {
        const uint32_t idx = index_[static_cast<size_t>(frame)];
        const uint64_t pos = uint64_t(idx & 0x7FFFFFFF) << align_;
        const uint64_t end = uint64_t(index_[static_cast<size_t>(frame + 1)] & 0x7FFFFFFF) << align_;
        if (end <= pos || end - pos > block_ + (uint64_t(1) << align_)) return false;
        std::vector<unsigned char> packed(static_cast<size_t>(end - pos));
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(pos));
        if (!in_.read(reinterpret_cast<char*>(packed.data()), static_cast<std::streamsize>(packed.size()))) return false;
        frame_.assign(block_, 0);
        if (idx & 0x80000000) {
            std::memcpy(frame_.data(), packed.data(), std::min<size_t>(packed.size(), block_));
        } else if (tinfl_decompress_mem_to_mem(frame_.data(), block_, packed.data(), packed.size(), 0) != block_) {
            return false;  // flags 0 = raw deflate, as CSO stores it
        }
        cached_ = static_cast<int64_t>(frame);
        return true;
    }

    std::ifstream in_;
    uint64_t total_ = 0;
    uint32_t block_ = 2048;
    uint8_t align_ = 0;
    std::vector<uint32_t> index_;
    std::vector<unsigned char> frame_;
    int64_t cached_ = -1;
};

// DISC_ID from PARAM.SFO (or UMD_DATA.BIN) of an .iso or .cso; "" for anything else (.chd, .pbp).
inline std::string discIdFromImage(const std::string& path) {
    const std::string ext = lower(pathToUtf8(utf8ToPath(path).extension()));
    std::unique_ptr<fmt::SectorReader> reader;
    if (ext == ".iso") reader = fmt::SectorReader::open(path);
    else if (ext == ".cso") reader = CsoReader::open(path);
    if (!reader) return {};
    fmt::Iso9660 iso(*reader);
    if (!iso.mount()) return {};
    fmt::IsoEntry e;
    std::string data;
    std::vector<std::pair<std::string, std::string>> kv;
    if (iso.find("/PSP_GAME/PARAM.SFO", e) && !e.directory && iso.readFile(e, data, 1 << 20) && fmt::parseSfo(data, kv)) {
        std::string id = normId(fmt::sfoValue(kv, "DISC_ID"));
        if (!id.empty()) return id;
    }
    if (iso.find("/UMD_DATA.BIN", e) && !e.directory && iso.readFile(e, data, 4096)) {
        std::string id = normId(data.substr(0, data.find('|')));
        if (id.size() == 9) return id;
    }
    return {};
}

// settings.extra.discId (override) > the scanner's "disc id XXXX" > reading the image.
// The UI asks missingRequirements() for every game card on every frame: image reads are cached
// per file (path, size, time).
inline std::string discIdFor(const LaunchRequest& req) {
    std::string id = normId(jget<std::string>(req.settings.extra, "discId", ""));
    if (!id.empty() || !req.entry) return id;
    const std::string& by = req.entry->files.detectedBy;
    if (by.rfind("disc id ", 0) == 0) return normId(by.substr(8));
    static std::mutex mutex;
    static std::map<std::string, std::string> cache;
    std::error_code ec;
    const fs::path p = utf8ToPath(req.entry->files.path);
    const std::string key = req.entry->files.path + "|" + std::to_string(fs::file_size(p, ec)) + "|" +
                            std::to_string(static_cast<long long>(fs::last_write_time(p, ec).time_since_epoch().count()));
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, discIdFromImage(req.entry->files.path)).first;
    return it->second;
}

// ---- cheat file ----------------------------------------------------------------------------------

inline uint32_t floatBits(double v) {
    const float f = static_cast<float>(v);
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

// Rewrites Marathon's own cheat in PSP/Cheats/<DISC_ID>.ini and keeps every other cheat;
// other enabled cheats that write the same addresses are switched off while Marathon's is on
// (marked with kSwitchedOff) and switched back on once it is off.
inline bool writeCheatFile(const fs::path& file, const std::string& discId, const WideCheat* cheat, double ratio) {
    std::error_code ec;
    const bool exists = fs::exists(file, ec);
    if (!exists && !cheat) return true;
    std::string text = exists ? readText(file) : std::string();
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // PPSSPP starts new cheat files with a BOM
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> blocks;  // one per "_C" line, with its "_L" lines
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = end + 1;
        if (line.rfind("_C", 0) == 0) blocks.push_back({line});
        else if (blocks.empty()) header.push_back(line);
        else blocks.back().push_back(line);
    }
    std::string out;
    if (std::none_of(header.begin(), header.end(), [](const std::string& l) { return l.rfind("_S", 0) == 0; })) {
        const WideCheat* known = wideCheatFor(discId);
        out += "_S " + std::string(known ? known->label : discId.c_str()) + "\n";
        if (known) out += std::string("_G ") + known->title + "\n";
    }
    for (auto& l : header)
        if (!l.empty()) out += l + "\n";
    for (auto& b : blocks) {
        if (b[0].size() > 4 && b[0].compare(4, std::strlen(kCheatName), kCheatName) == 0) continue;  // ours, rewritten below
        std::string first = b[0];
        bool clashes = false;
        if (cheat)
            for (size_t i = 1; i < b.size(); ++i)
                for (uint32_t a : cheat->addr)
                    if (lower(b[i]).find(lower(hex32(a)).substr(2)) != std::string::npos) clashes = true;
        const auto note = std::find(b.begin() + 1, b.end(), kSwitchedOff);
        if (clashes && first.size() > 2 && first[2] >= '1' && first[2] <= '9') {
            first[2] = '0';
            if (note == b.end()) b.insert(b.begin() + 1, kSwitchedOff);
        } else if (!clashes && note != b.end()) {
            if (first.size() > 2) first[2] = '1';  // Marathon's code is off: the user's cheat is back
            b.erase(note);
        }
        out += first + "\n";
        for (size_t i = 1; i < b.size(); ++i) out += b[i] + "\n";
    }
    if (cheat) {
        const std::string value = hex32(floatBits(ratio));
        out += std::string("_C1 ") + kCheatName + " " + fmtNumber(ratio) + ":1\n";
        for (uint32_t a : cheat->addr) out += "_L " + hex32(a) + " " + value + "\n";
    }
    if (exists && out == readText(file)) return true;
    return writeText(file, out);
}

// ---- texture packs -------------------------------------------------------------------------------

inline constexpr const char* kPackMarker = ".marathon-pack";  // in TEXTURES/<ID> copies: the source path

inline bool isPackRoot(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "textures.ini", ec) || fs::is_regular_file(dir / "textures.zip", ec);
}

inline fs::path childNamed(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return {};
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec) && normId(pathToUtf8(e.path().filename())) == normId(name)) return e.path();
    return {};
}

// A zip with textures.ini at its root is what PPSSPP reads as TEXTURES/<ID>/textures.zip.
inline bool zipHasRootIni(const fs::path& zip) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, pathToUtf8(zip).c_str(), 0)) return false;
    const bool ok = mz_zip_reader_locate_file(&z, "textures.ini", nullptr, 0) >= 0;
    mz_zip_reader_end(&z);
    return ok;
}

struct PackSource {
    fs::path path;  // the folder that becomes TEXTURES/<ID>, or a zip that becomes TEXTURES/<ID>/textures.zip
    bool zip = false;
    std::string error;
};

// settings.extra.texturePack rules:
//  - a folder holding textures.ini (or textures.zip) is the pack for the disc being started;
//  - otherwise <folder>/<ID>, <folder>/TEXTURES/<ID> or <folder>/PSP/TEXTURES/<ID> (ID with or
//    without the dash, any case), also below one wrapper folder (GitHub's "<repo>-<branch>/"),
//    then the same for a disc that shares the textures (kTextureAliases);
//  - a .zip with textures.ini at its root is used as textures.zip as it is; any other .zip is
//    unpacked once into the cache and searched like a folder.
inline PackSource findPack(const std::string& setting, const std::string& discId) {
    PackSource s;
    std::error_code ec;
    fs::path src = utf8ToPath(setting);
    if (fs::is_regular_file(src, ec) && endsWithI(setting, ".zip")) {
        if (zipHasRootIni(src)) {
            s.path = src;
            s.zip = true;
            return s;
        }
        const fs::path unpacked = paths().cache / "texture-packs" /
                                  utf8ToPath(pathToUtf8(src.stem()) + "-" + std::to_string(fs::file_size(src, ec)));
        if (!fs::exists(unpacked / ".complete", ec)) {
            if (dryRun()) {
                s.error = "would unpack " + setting;  // no processes in dry runs
                return s;
            }
            fs::remove_all(unpacked, ec);
            std::string error;
            if (!extractArchive(setting, pathToUtf8(unpacked), error)) {
                s.error = "Could not unpack the texture pack: " + error;
                return s;
            }
            writeText(unpacked / ".complete", setting);
        }
        src = unpacked;
    }
    if (!fs::is_directory(src, ec)) {
        s.error = "Texture pack not found: " + setting;
        return s;
    }
    if (isPackRoot(src)) {
        s.path = src;
        return s;
    }
    std::vector<std::string> ids{discId};
    for (auto& a : kTextureAliases)
        if (discId == a[0]) ids.push_back(a[1]);
    std::vector<fs::path> roots{src}, subdirs;
    for (auto& e : fs::directory_iterator(src, ec))
        if (const std::string n = pathToUtf8(e.path().filename()); e.is_directory(ec) && n.rfind('.', 0) != 0 && n != "__MACOSX")
            subdirs.push_back(e.path());
    if (subdirs.size() == 1) roots.push_back(subdirs.front());
    for (auto& id : ids)
        for (auto& root : roots)
            for (const fs::path& base : {root, childNamed(root, "TEXTURES"), childNamed(childNamed(root, "PSP"), "TEXTURES")}) {
                const fs::path dir = childNamed(base, id);
                if (!dir.empty() && isPackRoot(dir)) {
                    s.path = dir;
                    return s;
                }
            }
    s.error = "The texture pack at " + setting + " has nothing for " + discId + " (textures.ini in the folder itself or in a " +
              discId + " subfolder).";
    return s;
}

// findPack()'s complaint about settings.extra.texturePack, for the UI's per-frame requirement
// checks. A zip is only looked into at launch (unpacking it can take long). The answer is cached
// against the path's time, so it follows a drive being mounted or the pack appearing or going, and
// is checked again every few seconds for changes deeper inside the folder.
inline std::string packProblem(const std::string& setting, const std::string& discId) {
    std::error_code ec;
    const fs::path path = utf8ToPath(setting);
    if (fs::is_regular_file(path, ec) && endsWithI(setting, ".zip")) return {};
    struct Checked {
        std::string stamp, error;
        std::chrono::steady_clock::time_point at;
    };
    static std::mutex mutex;
    static std::map<std::string, Checked> cache;
    const std::string stamp = fs::exists(path, ec) ? std::to_string(static_cast<long long>(
                                                         fs::last_write_time(path, ec).time_since_epoch().count()))
                                                   : "missing";
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex);
    Checked& c = cache[setting + "|" + discId];
    if (c.stamp != stamp || now - c.at > std::chrono::seconds(5)) c = {stamp, findPack(setting, discId).error, now};
    return c.error;
}

inline std::string canonicalUtf8(const fs::path& p) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(p, ec);
    return pathToUtf8(ec ? p : c);
}

// TEXTURES/<ID> made by Marathon (a link, or a copy carrying the marker)?
inline bool ownedPackDir(const fs::path& dest) {
    std::error_code ec;
    return fs::is_symlink(dest, ec) || fs::exists(dest / kPackMarker, ec);
}

// Links the pack into TEXTURES/<ID>, or copies it where symlinks are not allowed (Windows
// without developer mode). A folder Marathon did not create is left alone.
inline bool installPack(const PackSource& pack, const fs::path& dest, std::string& error) {
    std::error_code ec;
    const std::string want = canonicalUtf8(pack.path);
    if (fs::is_symlink(dest, ec)) {
        if (!pack.zip && canonicalUtf8(fs::read_symlink(dest, ec)) == want) return true;
        fs::remove(dest, ec);
    } else if (fs::exists(dest / kPackMarker, ec)) {
        if (readText(dest / kPackMarker) == want) return true;
        fs::remove_all(dest, ec);
    } else if (fs::exists(dest, ec)) {
        error = pathToUtf8(dest) + " holds textures Marathon did not put there; they are used as they are.";
        return true;
    }
    fs::create_directories(dest.parent_path(), ec);
    if (pack.zip) {
        fs::create_directories(dest, ec);
        fs::create_symlink(pack.path, dest / "textures.zip", ec);
        if (ec) {
            ec.clear();
            fs::copy_file(pack.path, dest / "textures.zip", fs::copy_options::overwrite_existing, ec);
        }
    } else {
        fs::create_directory_symlink(pack.path, dest, ec);
        if (!ec) return true;
        ec.clear();
        fs::copy(pack.path, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
        error = "Could not install the texture pack into " + pathToUtf8(dest) + ": " + ec.message();
        return false;
    }
    return writeText(dest / kPackMarker, want);
}

}  // namespace rrm::psp
