// Xbox 360 discs: XDVDFS game partition -> default.xex -> XEX2 execution info
// (title id / media id). Also accepts an extracted game folder or a .xex file.
#include "core/formats/Formats.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "core/Platform.h"

namespace rrm::fmt {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kSector = 2048;
constexpr uint32_t kMaxXexHeader = 1u << 20;
// Game partition start in 2048-byte sectors: plain XISO, XGD2, XGD3, XGD1.
constexpr uint32_t kPartitions[] = {0, 0xFD90000 / kSector, 0x2080000 / kSector, 0x18300000 / kSector};

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

std::string hex8(uint32_t v) {
    static const char* digits = "0123456789ABCDEF";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i, v >>= 4) s[size_t(i)] = digits[v & 15];
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Reads `size` bytes starting at an absolute 2048-byte sector.
bool readSpan(SectorReader& r, uint64_t sector, uint64_t size, std::vector<uint8_t>& out) {
    out.clear();
    if (size == 0 || size > (16u << 20)) return false;
    const uint64_t sectors = (size + kSector - 1) / kSector;
    if (sector + sectors > r.sectorCount()) return false;
    out.resize(static_cast<size_t>(sectors * kSector));
    for (uint64_t i = 0; i < sectors; ++i)
        if (!r.read(static_cast<uint32_t>(sector + i), out.data() + i * kSector)) return false;
    out.resize(static_cast<size_t>(size));
    return true;
}

// XDVDFS directories are binary trees: u16 left, u16 right (4-byte units),
// u32 sector, u32 size, u8 attributes, u8 name length, name.
bool findInDir(const std::vector<uint8_t>& dir, const std::string& name, uint32_t& sector, uint32_t& size) {
    const std::string want = lower(name);
    std::vector<uint32_t> stack{0};
    std::vector<bool> seen(dir.size() / 4 + 1, false);
    while (!stack.empty()) {
        const uint32_t idx = stack.back();
        stack.pop_back();
        const size_t o = size_t(idx) * 4;
        if (o + 14 > dir.size() || seen[idx]) continue;
        seen[idx] = true;
        const uint8_t* e = dir.data() + o;
        const uint16_t left = le16(e), right = le16(e + 2);
        if (left == 0xFFFF && right == 0xFFFF) continue;  // 0xFF sector padding
        const uint8_t nameLen = e[13];
        if (o + 14 + nameLen > dir.size()) continue;
        if (nameLen == want.size() && lower(std::string(reinterpret_cast<const char*>(e + 14), nameLen)) == want) {
            sector = le32(e + 4);
            size = le32(e + 8);
            return true;
        }
        if (left != 0 && left != 0xFFFF) stack.push_back(left);
        if (right != 0 && right != 0xFFFF) stack.push_back(right);
    }
    return false;
}

// XEX2: big-endian; optional header count at 0x14, (key, value) pairs from 0x18.
// Key 0x00040006 points at the execution info {media_id, version, base_version, title_id, ...}.
bool parseXex(const std::vector<uint8_t>& x, std::string& titleId, std::string* mediaId) {
    if (x.size() < 0x18 || std::memcmp(x.data(), "XEX2", 4) != 0) return false;
    const uint32_t count = be32(&x[0x14]);
    if (count > 4096) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t o = 0x18 + size_t(i) * 8;
        if (o + 8 > x.size()) return false;
        if (be32(&x[o]) != 0x00040006u) continue;
        const uint32_t off = be32(&x[o + 4]);
        if (uint64_t(off) + 24 > x.size()) return false;
        if (mediaId) *mediaId = hex8(be32(&x[off]));
        titleId = hex8(be32(&x[off + 12]));
        return true;
    }
    return false;
}

// How much of default.xex to read: the whole header (size at 0x08), bounded.
size_t xexHeaderBytes(const std::vector<uint8_t>& first, uint64_t fileSize) {
    uint64_t want = kSector;
    if (first.size() >= 12) want = std::clamp<uint64_t>(be32(&first[8]), kSector, kMaxXexHeader);
    return static_cast<size_t>(std::min<uint64_t>(want, fileSize));
}

bool parseXexFile(const fs::path& path, std::string& titleId, std::string* mediaId) {
    std::error_code ec;
    const uint64_t fileSize = fs::file_size(path, ec);
    if (ec || fileSize < 0x18) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> x(static_cast<size_t>(std::min<uint64_t>(fileSize, kSector)));
    if (!in.read(reinterpret_cast<char*>(x.data()), std::streamsize(x.size()))) return false;
    const size_t want = xexHeaderBytes(x, fileSize);
    if (want > x.size()) {
        const size_t have = x.size();
        x.resize(want);
        if (!in.read(reinterpret_cast<char*>(x.data() + have), std::streamsize(want - have))) return false;
    }
    return parseXex(x, titleId, mediaId);
}

fs::path findChildCI(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    const std::string want = lower(name);
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (int budget = 20000; !ec && it != end && budget-- > 0; it.increment(ec))
        if (lower(rrm::pathToUtf8(it->path().filename())) == want) return it->path();
    return {};
}

}  // namespace

bool xbox360TitleId(const std::string& isoPath, std::string& titleId, std::string* mediaId) {
    titleId.clear();
    if (mediaId) mediaId->clear();
    try {
        std::error_code ec;
        const fs::path p = rrm::utf8ToPath(isoPath);
        if (fs::is_directory(p, ec)) {
            const fs::path xex = findChildCI(p, "default.xex");
            return !xex.empty() && parseXexFile(xex, titleId, mediaId);
        }
        if (lower(rrm::pathToUtf8(p.extension())) == ".xex") return parseXexFile(p, titleId, mediaId);

        auto r = SectorReader::open(isoPath);
        if (!r) return false;
        uint8_t vd[kSector];
        for (uint32_t part : kPartitions) {
            if (uint64_t(part) + 33 > r->sectorCount() || !r->read(part + 32, vd)) continue;
            if (std::memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20) != 0) continue;
            std::vector<uint8_t> dir, xex;
            uint32_t xexSector = 0, xexSize = 0;
            if (!readSpan(*r, uint64_t(part) + le32(vd + 20), le32(vd + 24), dir) ||
                !findInDir(dir, "default.xex", xexSector, xexSize) ||
                !readSpan(*r, uint64_t(part) + xexSector, std::min<uint32_t>(xexSize, kSector), xex))
                return false;
            const size_t want = xexHeaderBytes(xex, xexSize);
            if (want > xex.size() && !readSpan(*r, uint64_t(part) + xexSector, want, xex)) return false;
            if (parseXex(xex, titleId, mediaId)) return true;
            titleId.clear();
            return false;
        }
    } catch (...) {
    }
    titleId.clear();
    if (mediaId) mediaId->clear();
    return false;
}

}  // namespace rrm::fmt
