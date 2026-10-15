// Minimal read-only ISO9660: primary volume descriptor, directory listing
// (multi-extent files for >4 GB PS3 images), case-insensitive path lookup.
#include "core/formats/Formats.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace rrm::fmt {

namespace {

constexpr uint32_t kSector = 2048;
constexpr uint64_t kMaxDirBytes = 32ull << 20;  // sanity cap for malformed directory sizes

inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// "SYSTEM.CNF;1" -> "SYSTEM.CNF", "README.;1" -> "README"
std::string cleanName(std::string n) {
    if (auto semi = n.find(';'); semi != std::string::npos) n.resize(semi);
    if (n.size() > 1 && n.back() == '.') n.pop_back();
    return n;
}

bool listDir(SectorReader& r, const IsoEntry& dir, std::vector<IsoEntry>& out) {
    out.clear();
    if (!dir.directory || dir.extents.empty()) return false;
    const uint64_t count = r.sectorCount();
    uint64_t budget = kMaxDirBytes;
    bool continues = false;  // previous record had the multi-extent flag
    uint8_t sec[kSector];
    for (const auto& [lba, bytes] : dir.extents) {
        const uint64_t use = std::min<uint64_t>(bytes, budget);
        budget -= use;
        const uint32_t sectors = static_cast<uint32_t>((use + kSector - 1) / kSector);
        for (uint32_t s = 0; s < sectors; ++s) {
            if (uint64_t(lba) + s >= count || !r.read(lba + s, sec)) return false;
            const size_t limit = static_cast<size_t>(std::min<uint64_t>(kSector, use - uint64_t(s) * kSector));
            size_t pos = 0;
            // Records never cross a sector; a zero length byte pads to the next sector.
            while (pos + 33 <= limit) {
                const uint8_t len = sec[pos];
                if (len == 0 || pos + len > limit || len < 33) break;
                const uint8_t* rec = sec + pos;
                pos += len;
                const uint8_t nameLen = rec[32];
                if (nameLen == 0 || 33u + nameLen > len) continue;
                if (nameLen == 1 && (rec[33] == 0 || rec[33] == 1)) continue;  // "." and ".."
                const uint32_t extLba = le32(rec + 2) + rec[1];  // + extended attribute record length
                const uint32_t extSize = le32(rec + 10);
                const uint8_t flags = rec[25];
                std::string name = cleanName(std::string(reinterpret_cast<const char*>(rec + 33), nameLen));
                if (continues && !out.empty() && out.back().name == name) {
                    out.back().extents.emplace_back(extLba, extSize);
                    out.back().size += extSize;
                } else {
                    IsoEntry e;
                    e.name = std::move(name);
                    e.directory = (flags & 0x02) != 0;
                    e.size = extSize;
                    e.extents.emplace_back(extLba, extSize);
                    out.push_back(std::move(e));
                }
                continues = (flags & 0x80) != 0;
            }
        }
        if (budget == 0) break;
    }
    return true;
}

}  // namespace

bool Iso9660::mount() {
    root_ = IsoEntry{};
    volumeId_.clear();
    try {
        uint8_t vd[kSector];
        // Volume descriptors start at sector 16 and end with a type 255 terminator.
        for (uint32_t lba = 16; lba < 16 + 64; ++lba) {
            if (!r_.read(lba, vd)) return false;
            if (std::memcmp(vd + 1, "CD001", 5) != 0) return false;
            if (vd[0] == 255) return false;
            if (vd[0] != 1) continue;
            const uint8_t* rootRec = vd + 156;
            const uint32_t lbaRoot = le32(rootRec + 2) + rootRec[1];
            const uint32_t size = le32(rootRec + 10);
            if (lbaRoot == 0 || lbaRoot >= r_.sectorCount() || size == 0) return false;
            root_.name = "/";
            root_.directory = true;
            root_.size = size;
            root_.extents.emplace_back(lbaRoot, size);
            std::string vol(reinterpret_cast<const char*>(vd + 40), 32);
            while (!vol.empty() && (vol.back() == ' ' || vol.back() == '\0')) vol.pop_back();
            volumeId_ = vol;
            return true;
        }
    } catch (...) {
        root_ = IsoEntry{};
    }
    return false;
}

bool Iso9660::list(const std::string& dir, std::vector<IsoEntry>& out) {
    out.clear();
    try {
        IsoEntry e;
        if (!find(dir, e)) return false;
        return listDir(r_, e, out);
    } catch (...) {
        return false;
    }
}

bool Iso9660::find(const std::string& path, IsoEntry& out) {
    try {
        if (root_.extents.empty()) return false;
        IsoEntry cur = root_;
        std::vector<IsoEntry> entries;
        size_t pos = 0;
        while (pos < path.size()) {
            size_t end = path.find_first_of("/\\", pos);
            if (end == std::string::npos) end = path.size();
            std::string part = path.substr(pos, end - pos);
            pos = end + 1;
            if (part.empty() || part == ".") continue;
            part = cleanName(part);
            if (!cur.directory || !listDir(r_, cur, entries)) return false;
            auto it = std::find_if(entries.begin(), entries.end(),
                                   [&](const IsoEntry& e) { return sameName(e.name, part); });
            if (it == entries.end()) return false;
            cur = std::move(*it);
        }
        out = std::move(cur);
        return true;
    } catch (...) {
        return false;
    }
}

bool Iso9660::readFile(const IsoEntry& e, std::string& out, uint64_t maxBytes) {
    out.clear();
    try {
        const uint64_t want = std::min<uint64_t>(e.size, maxBytes);
        if (want > (1ull << 31)) return false;  // in-memory reads only; callers ask for small files
        out.reserve(static_cast<size_t>(want));
        uint8_t sec[kSector];
        for (const auto& [lba, bytes] : e.extents) {
            uint64_t remain = std::min<uint64_t>(bytes, want - out.size());
            for (uint32_t s = 0; remain > 0; ++s) {
                if (uint64_t(lba) + s >= r_.sectorCount() || !r_.read(lba + s, sec)) {
                    out.clear();
                    return false;
                }
                const size_t take = static_cast<size_t>(std::min<uint64_t>(kSector, remain));
                out.append(reinterpret_cast<const char*>(sec), take);
                remain -= take;
            }
            if (out.size() >= want) break;
        }
        return true;
    } catch (...) {
        out.clear();
        return false;
    }
}

}  // namespace rrm::fmt
