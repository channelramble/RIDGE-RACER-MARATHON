// Unit tests for the scanner module on synthetic media (no user files needed):
// hashing, CUE, raw/cooked sector readers, ISO9660, SYSTEM.CNF, PARAM.SFO,
// XDVDFS/XEX, zip reading/writing, romset rebuild and an end-to-end scan;
// plus the catalog's extra-file merging.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "core/Catalog.h"
#include "core/Platform.h"
#include "core/RomSet.h"
#include "core/Scanner.h"
#include "core/formats/Formats.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz/miniz.h"

using namespace rrm;

namespace {

int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        const auto& va_ = (a);                                                                      \
        const auto& vb_ = (b);                                                                      \
        if (!(va_ == vb_)) {                                                                        \
            ++g_failures;                                                                           \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got: %s\n  want: %s\n", __FILE__, \
                         __LINE__, #a, #b, toStr(va_).c_str(), toStr(vb_).c_str());                \
        }                                                                                           \
    } while (0)

std::string toStr(const std::string& s) { return "\"" + s + "\""; }
template <class T>
std::string toStr(const T& v) {
    return std::to_string(v);
}

fs::path g_tmp;

// Native separators, as the code under test returns them ("a/b" is "a\\b" on Windows).
fs::path tmpPath(const std::string& rel) { return (g_tmp / utf8ToPath(rel)).make_preferred(); }
std::string tmpStr(const std::string& rel) { return pathToUtf8(tmpPath(rel)); }

void writePath(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
}
void writeFile(const std::string& rel, const std::string& data) { writePath(tmpPath(rel), data); }

std::string crcHex(const std::string& data) {
    char b[9];
    std::snprintf(b, sizeof(b), "%08x", fmt::crc32Update(0, data.data(), data.size()));
    return b;
}

std::string pattern(size_t n, uint32_t seed) {
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (auto& c : s) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = char(x & 0xFF);
    }
    return s;
}

void put32le(std::string& s, size_t o, uint32_t v) {
    for (int i = 0; i < 4; ++i) s[o + i] = char(v >> (8 * i));
}
void put32be(std::string& s, size_t o, uint32_t v) {
    for (int i = 0; i < 4; ++i) s[o + i] = char(v >> (24 - 8 * i));
}
void put16le(std::string& s, size_t o, uint16_t v) {
    s[o] = char(v & 0xFF);
    s[o + 1] = char(v >> 8);
}

// ---- synthetic ISO9660 ------------------------------------------------------------------------

struct TNode {
    std::string name;
    bool dir = false;
    std::string data;
    bool multiExtent = false;
    std::vector<TNode> kids;
    std::vector<std::pair<uint32_t, uint32_t>> extents;
};

std::string dirRecord(const std::string& name, uint32_t lba, uint32_t size, uint8_t flags) {
    std::string r(33, '\0');
    r += name;
    if (r.size() % 2) r += '\0';
    r[0] = char(r.size());
    put32le(r, 2, lba);
    put32be(r, 6, lba);
    put32le(r, 10, size);
    put32be(r, 14, size);
    r[25] = char(flags);
    r[28] = 1;
    r[31] = 1;
    r[32] = char(name.size());
    return r;
}

// Records never cross a sector boundary: pad with zeros instead.
std::string packRecords(const std::vector<std::string>& recs) {
    std::string out;
    for (const auto& r : recs) {
        const size_t used = out.size() % 2048;
        if (used + r.size() > 2048) out.append(2048 - used, '\0');
        out += r;
    }
    out.append((2048 - out.size() % 2048) % 2048, '\0');
    return out;
}

class IsoBuilder {
public:
    // path like "PSP_GAME/PARAM.SFO"; names are stored with ";1" for files.
    void add(const std::string& path, std::string data, bool multiExtent = false) {
        TNode* cur = &root_;
        size_t pos = 0;
        while (true) {
            const size_t slash = path.find('/', pos);
            const std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            if (slash == std::string::npos) {
                TNode f;
                f.name = part;
                f.data = std::move(data);
                f.multiExtent = multiExtent;
                cur->kids.push_back(std::move(f));
                return;
            }
            auto it = std::find_if(cur->kids.begin(), cur->kids.end(), [&](const TNode& n) { return n.dir && n.name == part; });
            if (it == cur->kids.end()) {
                TNode d;
                d.name = part;
                d.dir = true;
                cur->kids.push_back(std::move(d));
                it = cur->kids.end() - 1;
            }
            cur = &*it;
            pos = slash + 1;
        }
    }
    void addFiller(int count) { filler_ = count; }

    std::string build(const std::string& volume) {
        root_.dir = true;
        for (int i = 0; i < filler_; ++i) add("FILLER" + std::to_string(1000 + i) + ".DAT", "");
        next_ = 20;
        layoutDirs(root_);
        layoutFiles(root_);
        std::string img(size_t(next_) * 2048, '\0');
        std::string pvd(2048, '\0');
        pvd[0] = 1;
        std::memcpy(&pvd[1], "CD001", 5);
        pvd[6] = 1;
        std::string vol = volume;
        vol.resize(32, ' ');
        std::memcpy(&pvd[40], vol.data(), 32);
        put32le(pvd, 80, next_);
        put32be(pvd, 84, next_);
        const std::string rootRec = dirRecord(std::string(1, '\0'), root_.extents[0].first, root_.extents[0].second, 2);
        std::memcpy(&pvd[156], rootRec.data(), 34);
        std::memcpy(&img[16 * 2048], pvd.data(), 2048);
        std::string term(2048, '\0');
        term[0] = char(255);
        std::memcpy(&term[1], "CD001", 5);
        term[6] = 1;
        std::memcpy(&img[17 * 2048], term.data(), 2048);
        writeNodes(root_, root_, img);
        return img;
    }

private:
    std::vector<std::string> records(const TNode& d, const TNode& parent) const {
        std::vector<std::string> recs;
        auto ext = [](const TNode& n) { return n.extents.empty() ? std::pair<uint32_t, uint32_t>{0, 0} : n.extents[0]; };
        recs.push_back(dirRecord(std::string(1, '\0'), ext(d).first, ext(d).second, 2));
        recs.push_back(dirRecord(std::string(1, '\1'), ext(parent).first, ext(parent).second, 2));
        for (const auto& k : d.kids) {
            if (k.dir) {
                recs.push_back(dirRecord(k.name, ext(k).first, ext(k).second, 2));
            } else if (k.multiExtent && k.extents.size() == 2) {
                recs.push_back(dirRecord(k.name + ";1", k.extents[0].first, k.extents[0].second, 0x80));
                recs.push_back(dirRecord(k.name + ";1", k.extents[1].first, k.extents[1].second, 0));
            } else {
                recs.push_back(dirRecord(k.name + ";1", ext(k).first, uint32_t(k.data.size()), 0));
                if (k.multiExtent) recs.push_back(dirRecord(k.name + ";1", 0, 0, 0));  // size placeholder
            }
        }
        return recs;
    }
    void layoutDirs(TNode& d) {
        const uint32_t bytes = uint32_t(packRecords(records(d, d)).size());
        d.extents = {{next_, bytes}};
        next_ += bytes / 2048;
        for (auto& k : d.kids)
            if (k.dir) layoutDirs(k);
    }
    void layoutFiles(TNode& d) {
        for (auto& k : d.kids) {
            if (k.dir) {
                layoutFiles(k);
                continue;
            }
            const uint32_t size = uint32_t(k.data.size());
            if (k.multiExtent) {
                // Non-final extents are whole sectors; leave a gap sector between extents.
                const uint32_t first = std::max<uint32_t>(2048, (size / 2) / 2048 * 2048);
                k.extents = {{next_, first}, {next_ + first / 2048 + 1, size - first}};
                next_ += first / 2048 + 1 + (size - first + 2047) / 2048;
            } else {
                k.extents = {{size ? next_ : 0, size}};
                next_ += (size + 2047) / 2048;
            }
        }
    }
    void writeNodes(const TNode& d, const TNode& parent, std::string& img) const {
        const std::string packed = packRecords(records(d, parent));
        std::memcpy(&img[size_t(d.extents[0].first) * 2048], packed.data(), packed.size());
        for (const auto& k : d.kids) {
            if (k.dir) {
                writeNodes(k, d, img);
                continue;
            }
            size_t off = 0;
            for (const auto& [lba, bytes] : k.extents) {
                if (bytes) std::memcpy(&img[size_t(lba) * 2048], k.data.data() + off, bytes);
                off += bytes;
            }
        }
    }

    TNode root_;
    uint32_t next_ = 20;
    int filler_ = 0;
};

uint8_t bcd(uint32_t v) { return uint8_t((v / 10) << 4 | (v % 10)); }

// Cooked 2048-byte image -> raw 2352-byte MODE1 or MODE2 form 1 sectors.
std::string toRaw(const std::string& cooked, int mode, uint32_t leadSectors = 0) {
    std::string out;
    const size_t count = cooked.size() / 2048 + leadSectors;
    out.reserve(count * 2352);
    for (size_t s = 0; s < count; ++s) {
        std::string sec(2352, '\0');
        for (int i = 1; i <= 10; ++i) sec[i] = char(0xFF);
        const uint32_t a = uint32_t(s) + 150;
        sec[12] = char(bcd(a / 75 / 60));
        sec[13] = char(bcd(a / 75 % 60));
        sec[14] = char(bcd(a % 75));
        sec[15] = char(mode);
        const bool lead = s < leadSectors;
        const char* src = lead ? nullptr : cooked.data() + (s - leadSectors) * 2048;
        if (mode == 1) {
            if (src) std::memcpy(&sec[16], src, 2048);
        } else {
            sec[18] = sec[22] = 8;  // subheader: data, form 1
            if (src) std::memcpy(&sec[24], src, 2048);
        }
        out += sec;
    }
    return out;
}

std::string makeSfo(const std::vector<std::pair<std::string, std::string>>& strs,
                    const std::vector<std::pair<std::string, uint32_t>>& ints) {
    struct E {
        std::string key;
        uint16_t fmt;
        std::string data;
    };
    std::vector<E> es;
    for (const auto& [k, v] : strs) es.push_back({k, 0x0204, v + std::string(1, '\0')});
    for (const auto& [k, v] : ints) {
        std::string d(4, '\0');
        put32le(d, 0, v);
        es.push_back({k, 0x0404, d});
    }
    std::sort(es.begin(), es.end(), [](const E& a, const E& b) { return a.key < b.key; });
    std::string keys, data;
    std::string index(es.size() * 16, '\0');
    for (size_t i = 0; i < es.size(); ++i) {
        const uint32_t maxLen = uint32_t((es[i].data.size() + 3) / 4 * 4);
        put16le(index, i * 16, uint16_t(keys.size()));
        put16le(index, i * 16 + 2, es[i].fmt);
        put32le(index, i * 16 + 4, uint32_t(es[i].data.size()));
        put32le(index, i * 16 + 8, maxLen);
        put32le(index, i * 16 + 12, uint32_t(data.size()));
        keys += es[i].key + std::string(1, '\0');
        std::string d = es[i].data;
        d.resize(maxLen, '\0');
        data += d;
    }
    while (keys.size() % 4) keys += '\0';
    std::string h(20, '\0');
    std::memcpy(&h[0], "\0PSF", 4);
    put32le(h, 4, 0x0101);
    put32le(h, 8, uint32_t(20 + index.size()));
    put32le(h, 12, uint32_t(20 + index.size() + keys.size()));
    put32le(h, 16, uint32_t(es.size()));
    return h + index + keys + data;
}

// ---- synthetic Xbox 360 -------------------------------------------------------------------------

std::string makeXex(uint32_t titleId, uint32_t mediaId) {
    std::string x(0x1000, '\0');
    std::memcpy(&x[0], "XEX2", 4);
    put32be(x, 0x08, 0x1000);
    put32be(x, 0x10, 0x100);
    put32be(x, 0x14, 3);
    put32be(x, 0x18, 0x00010001);
    put32be(x, 0x1C, 0);
    put32be(x, 0x20, 0x000183FF);
    put32be(x, 0x24, 0x300);
    put32be(x, 0x28, 0x00040006);
    put32be(x, 0x2C, 0x400);
    put32be(x, 0x400, mediaId);
    put32be(x, 0x404, 1);
    put32be(x, 0x40C, titleId);
    return x;
}

void xdvdfsEntry(std::string& dir, size_t o, uint16_t left, uint16_t right, uint32_t sector, uint32_t size,
                 uint8_t attr, const std::string& name) {
    put16le(dir, o, left);
    put16le(dir, o + 2, right);
    put32le(dir, o + 4, sector);
    put32le(dir, o + 8, size);
    dir[o + 12] = char(attr);
    dir[o + 13] = char(name.size());
    std::memcpy(&dir[o + 14], name.data(), name.size());
}

// Sparse image with an XDVDFS game partition at `partBytes`.
void writeXboxImage(const fs::path& p, uint64_t partBytes, const std::string& xex) {
    fs::create_directories(p.parent_path());
    { std::ofstream(p, std::ios::binary | std::ios::trunc); }
    fs::resize_file(p, partBytes + 64 * 2048);
    std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
    auto writeAt = [&](uint64_t sector, const std::string& data) {
        f.seekp(std::streamoff(partBytes + sector * 2048));
        f.write(data.data(), std::streamsize(data.size()));
    };
    std::string vd(2048, '\0');
    std::memcpy(&vd[0], "MICROSOFT*XBOX*MEDIA", 20);
    put32le(vd, 20, 40);
    put32le(vd, 24, 2048);
    std::memcpy(&vd[0x7EC], "MICROSOFT*XBOX*MEDIA", 20);
    writeAt(32, vd);
    // Tree: "media" at the root, "aaa.bin" on its left, "default.xex" on its right.
    std::string dir(2048, char(0xFF));
    xdvdfsEntry(dir, 0, 5, 11, 50, 2048, 0x10, "media");
    xdvdfsEntry(dir, 20, 0, 0, 51, 10, 0x80, "aaa.bin");
    xdvdfsEntry(dir, 44, 0, 0, 41, uint32_t(xex.size()), 0x80, "default.xex");
    writeAt(40, dir);
    writeAt(41, xex);
}

// ---- tests ---------------------------------------------------------------------------------------

void testHashing() {
    struct V {
        std::string data, crc, md5, sha1;
    };
    std::vector<V> vectors = {
        {"", "00000000", "d41d8cd98f00b204e9800998ecf8427e", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"abc", "352441c2", "900150983cd24fb0d6963f7d28e17f72", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"123456789", "cbf43926", "25f9e794323b453885f5181f1b624d0b", "f7c3bc1d808e04732adf679965ccc34ca7ae3441"},
        {std::string(1000000, 'a'), "dc25bfbc", "7707d6ae4e027c70eea2a935c2296f21",
         "34aa973cd4c4daa4f61eeb2bdbad27316534016f"},
    };
    // Padding boundaries: 55/56/63/64/65/119/120 bytes of 0,1,2,...
    const std::map<size_t, std::array<const char*, 3>> boundaries = {
        {55, {"fd4fdad4", "6912ee65fff2d9f9ce2508cddf8bcda0", "8ae2d46729cfe68ff927af5eec9c7d1b66d65ac2"}},
        {56, {"ebfc1395", "51fdd1acda72405dfdfa03fcb85896d7", "636e2ec698dac903498e648bd2f3af641d3c88cb"}},
        {63, {"dbdea683", "48a6295221902e8e0938f773a7185e72", "6d942da0c4392b123528f2905c713a3ce28364bd"}},
        {64, {"100ece8c", "b2d3f56bc197fd985d5965079b5e7148", "c6138d514ffa2135bfce0ed0b8fac65669917ec7"}},
        {65, {"40c06fd8", "8bd7053801c768420faf816fadba971c", "69bd728ad6e13cd76ff19751fde427b00e395746"}},
        {119, {"9353a1a6", "1c772251899a7ff007400b888d6b2042", "41c89d06001bab4ab78736b44efe7ce18ce6ae08"}},
        {120, {"23455e6e", "b7ba1efc6022e9ed272f00b8831e26e6", "d3dbd653bd8597b7475321b60a36891278e6a04a"}},
    };
    for (const auto& [len, d] : boundaries) {
        std::string s(len, '\0');
        for (size_t i = 0; i < len; ++i) s[i] = char(i);
        vectors.push_back({s, d[0], d[1], d[2]});
    }
    // 9 MiB + 12345 bytes: spans three 4 MiB chunks.
    std::string big(9 * 1024 * 1024 + 12345, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = char((i * 31 + (i >> 13)) & 0xFF);
    vectors.push_back({big, "8d52f8ef", "a8a199e9104dc21f59f2408ad36b3b06", "599c55e8c52d6727fbf0c0e1b32466ff476c776b"});

    for (size_t i = 0; i < vectors.size(); ++i) {
        const auto& v = vectors[i];
        const std::string path = tmpStr("hash/v" + std::to_string(i) + ".bin");
        writePath(utf8ToPath(path), v.data);
        fmt::Digests d;
        uint64_t lastDone = 0, lastTotal = 0;
        CHECK(fmt::hashFile(path, d, nullptr, [&](uint64_t done, uint64_t total) {
            lastDone = done;
            lastTotal = total;
        }));
        CHECK_EQ(d.crc32, v.crc);
        CHECK_EQ(d.md5, v.md5);
        CHECK_EQ(d.sha1, v.sha1);
        CHECK_EQ(d.size, uint64_t(v.data.size()));
        if (!v.data.empty()) {
            CHECK_EQ(lastDone, uint64_t(v.data.size()));
            CHECK_EQ(lastTotal, uint64_t(v.data.size()));
        }
        CHECK_EQ(crcHex(v.data), v.crc);
    }
    // Incremental CRC equals one-shot CRC (unaligned split).
    const uint32_t whole = fmt::crc32Update(0, big.data(), big.size());
    uint32_t parts = fmt::crc32Update(0, big.data(), 7);
    parts = fmt::crc32Update(parts, big.data() + 7, big.size() - 7);
    CHECK_EQ(whole, parts);

    std::atomic<bool> cancel{true};
    fmt::Digests d;
    CHECK(!fmt::hashFile(tmpStr("hash/v" + std::to_string(vectors.size() - 1) + ".bin"), d, &cancel));
    CHECK(!fmt::hashFile(tmpStr("hash/does-not-exist.bin"), d));
}

std::string ps1Iso(const std::string& systemCnf, int filler = 0) {
    IsoBuilder b;
    b.add("SYSTEM.CNF", systemCnf);
    b.add("SCUS_943.00", pattern(5000, 1));
    b.add("DATA/BIG.DAT", pattern(3 * 2048 + 100, 2), true);
    b.add("DATA/SUB/DEEP.TXT", "deep");
    b.addFiller(filler);
    return b.build("RIDGERACERUSA");
}

void testIso9660() {
    const std::string cnf = "BOOT = cdrom:SCUS-943.00;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n\x1a";
    const std::string cooked = ps1Iso(cnf, 70);  // 70 filler entries: root spans 3 sectors
    writeFile("iso/cooked.iso", cooked);
    writeFile("iso/mode2.bin", toRaw(cooked, 2));
    writeFile("iso/mode1.img", toRaw(cooked, 1));
    // A raw image named .iso is detected by size % 2352 and the sync pattern.
    writeFile("iso/rawnamed.iso", toRaw(cooked, 2));

    for (const char* name : {"iso/cooked.iso", "iso/mode2.bin", "iso/mode1.img", "iso/rawnamed.iso"}) {
        auto r = fmt::SectorReader::open(tmpStr(name));
        CHECK(r != nullptr);
        if (!r) continue;
        CHECK_EQ(r->sectorCount(), uint64_t(cooked.size() / 2048));
        fmt::Iso9660 iso(*r);
        CHECK(iso.mount());
        CHECK_EQ(iso.volumeId(), std::string("RIDGERACERUSA"));
        std::vector<fmt::IsoEntry> root;
        CHECK(iso.list("/", root));
        CHECK_EQ(root.size(), size_t(3 + 70));  // SYSTEM.CNF, SCUS_943.00, DATA + fillers
        fmt::IsoEntry e;
        CHECK(iso.find("/system.cnf", e));  // case-insensitive
        CHECK_EQ(e.name, std::string("SYSTEM.CNF"));
        std::string data;
        CHECK(iso.readFile(e, data));
        CHECK_EQ(data, cnf);
        CHECK_EQ(fmt::bootSerialFromSystemCnf(data), std::string("SCUS-943.00"));
        CHECK(iso.find("FILLER1069.DAT;1", e));  // last record, in the third directory sector
        CHECK(iso.find("\\DATA\\SUB\\deep.txt", e));
        CHECK(iso.readFile(e, data) && data == "deep");
        CHECK(iso.find("/DATA/BIG.DAT", e));
        CHECK_EQ(e.extents.size(), size_t(2));
        CHECK_EQ(e.size, uint64_t(3 * 2048 + 100));
        CHECK(iso.readFile(e, data));
        CHECK(data == pattern(3 * 2048 + 100, 2));
        CHECK(iso.readFile(e, data, 3000) && data == pattern(3 * 2048 + 100, 2).substr(0, 3000));
        CHECK(!iso.find("/DATA/NOPE", e));
        CHECK(!iso.find("/SYSTEM.CNF/X", e));
        uint8_t sec[2048];
        CHECK(!r->read(uint32_t(r->sectorCount()), sec));
    }
    // Garbage and tiny files never mount.
    writeFile("iso/garbage.iso", pattern(40 * 2048, 9));
    auto g = fmt::SectorReader::open(tmpStr("iso/garbage.iso"));
    CHECK(g != nullptr);
    if (g) {
        fmt::Iso9660 iso(*g);
        CHECK(!iso.mount());
        fmt::IsoEntry e;
        CHECK(!iso.find("/SYSTEM.CNF", e));
    }
    writeFile("iso/tiny.bin", "x");
    CHECK(fmt::SectorReader::open(tmpStr("iso/tiny.bin")) == nullptr);
    // Truncated image: directory sectors beyond the end must fail cleanly.
    writeFile("iso/truncated.iso", cooked.substr(0, 21 * 2048));
    if (auto t = fmt::SectorReader::open(tmpStr("iso/truncated.iso"))) {
        fmt::Iso9660 iso(*t);
        fmt::IsoEntry e;
        CHECK(iso.mount());
        CHECK(!iso.find("/DATA/BIG.DAT", e));
    }
}

void testCue() {
    const std::string cooked = ps1Iso("BOOT = cdrom:\\SLUS_004.03;1\r\n");
    writeFile("cue/Game (Track 01).bin", toRaw(cooked, 2));
    writeFile("cue/Game (Track 02).bin", pattern(2352 * 20, 3));
    writeFile("cue/sub/Data Track.bin", toRaw(cooked, 1));
    writeFile("cue/upper.bin", toRaw(cooked, 2));
    writeFile("cue/cooked.iso", cooked);
    writeFile("cue/pregap.bin", toRaw(cooked, 2, 150));

    writeFile("cue/quoted.cue",
              "\xEF\xBB\xBF" "REM test\r\nFILE \"Game (Track 01).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 "
              "00:00:00\r\nFILE \"Game (Track 02).bin\" BINARY\r\n  TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n    "
              "INDEX 01 00:02:00\r\n");
    writeFile("cue/unquoted.cue", "FILE sub\\Data Track.bin BINARY\n  TRACK 1 MODE1/2352\n    INDEX 1 00:00:00\n");
    writeFile("cue/case.cue", "FILE \"UPPER.BIN\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    writeFile("cue/cooked.cue", "FILE \"cooked.iso\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n");
    writeFile("cue/stale.cue", "FILE \"C:\\Games\\Game (Track 01).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    writeFile("cue/pregap.cue",
              "FILE \"pregap.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n");
    writeFile("cue/missing.cue",
              "FILE \"Game (Track 01).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\nFILE \"gone.bin\" BINARY\n"
              "  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n");
    writeFile("cue/nodata.cue", "FILE \"gone.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    writeFile("cue/empty.cue", "REM nothing here\n");
    writeFile("cue/binary.cue", std::string("FILE \0\0\0", 8));

    fmt::CueSheet cue;
    std::string err;
    CHECK(fmt::parseCue(tmpStr("cue/quoted.cue"), cue, &err));
    CHECK_EQ(cue.tracks.size(), size_t(2));
    if (cue.tracks.size() == 2) {
        CHECK_EQ(cue.tracks[0].number, 1);
        CHECK_EQ(cue.tracks[0].mode, std::string("MODE2/2352"));
        CHECK_EQ(cue.tracks[0].sectorSize, 2352u);
        CHECK_EQ(cue.tracks[0].dataOffset, 24u);
        CHECK_EQ(cue.tracks[0].file, tmpStr("cue/Game (Track 01).bin"));
        CHECK_EQ(cue.tracks[1].mode, std::string("AUDIO"));
        CHECK_EQ(cue.tracks[1].dataOffset, 0u);
    }
    CHECK(cue.missingFiles.empty());

    CHECK(fmt::parseCue(tmpStr("cue/unquoted.cue"), cue));
    CHECK(cue.tracks.size() == 1 && cue.tracks[0].file == tmpStr("cue/sub/Data Track.bin"));
    CHECK(cue.tracks.size() == 1 && cue.tracks[0].dataOffset == 16);
    CHECK(fmt::parseCue(tmpStr("cue/stale.cue"), cue) && cue.missingFiles.empty());
    CHECK(fmt::parseCue(tmpStr("cue/missing.cue"), cue));
    CHECK(cue.missingFiles.size() == 1 && cue.missingFiles[0] == tmpStr("cue/gone.bin"));
    CHECK(!fmt::parseCue(tmpStr("cue/empty.cue"), cue, &err) && !err.empty());
    CHECK(!fmt::parseCue(tmpStr("cue/binary.cue"), cue));
    CHECK(!fmt::parseCue(tmpStr("cue/does-not-exist.cue"), cue));

    for (const char* name : {"cue/quoted.cue", "cue/unquoted.cue", "cue/case.cue", "cue/cooked.cue", "cue/stale.cue",
                             "cue/pregap.cue", "cue/missing.cue"}) {
        auto r = fmt::SectorReader::open(tmpStr(name));
        CHECK(r != nullptr);
        if (!r) {
            std::fprintf(stderr, "  (reader for %s)\n", name);
            continue;
        }
        fmt::Iso9660 iso(*r);
        fmt::IsoEntry e;
        std::string data;
        CHECK(iso.mount() && iso.find("SYSTEM.CNF", e) && iso.readFile(e, data));
        CHECK_EQ(fmt::bootSerialFromSystemCnf(data), std::string("SLUS_004.03"));
    }
    CHECK(fmt::SectorReader::open(tmpStr("cue/nodata.cue")) == nullptr);
}

void testConsoleMeta() {
    CHECK_EQ(fmt::bootSerialFromSystemCnf("BOOT = cdrom:\\SCUS_943.00;1\r\nTCB = 4\r\n"), std::string("SCUS_943.00"));
    CHECK_EQ(fmt::bootSerialFromSystemCnf("BOOT = cdrom:SCUS-943.00;1\r\n"), std::string("SCUS-943.00"));
    CHECK_EQ(fmt::bootSerialFromSystemCnf("BOOT2 = cdrom0:\\SLUS_200.02;1\r\nVER = 1.10\r\nVMODE = NTSC\r"),
             std::string("SLUS_200.02"));
    CHECK_EQ(fmt::bootSerialFromSystemCnf("boot=cdrom:\\\\SLPS_005.65;1 arg\n"), std::string("SLPS_005.65"));
    CHECK_EQ(fmt::bootSerialFromSystemCnf("BOOT\t=\tcdrom:\\GAME\\MAIN.EXE;1\n"), std::string("MAIN.EXE"));
    CHECK_EQ(fmt::bootSerialFromSystemCnf("VMODE = NTSC\nBOOT2 = cdrom0:\\SLES_123.45;1\nBOOT = x\n"),
             std::string("SLES_123.45"));
    CHECK_EQ(fmt::bootSerialFromSystemCnf("TCB = 4\n"), std::string());
    CHECK_EQ(fmt::bootSerialFromSystemCnf(""), std::string());

    const std::string sfo = makeSfo({{"DISC_ID", "UCES00422"}, {"TITLE", "RIDGE RACER 2"}, {"CATEGORY", "UG"}},
                                    {{"PARENTAL_LEVEL", 2}, {"BOOTABLE", 1}});
    std::vector<std::pair<std::string, std::string>> kv;
    CHECK(fmt::parseSfo(sfo, kv));
    CHECK_EQ(kv.size(), size_t(5));
    CHECK_EQ(fmt::sfoValue(kv, "DISC_ID"), std::string("UCES00422"));
    CHECK_EQ(fmt::sfoValue(kv, "TITLE"), std::string("RIDGE RACER 2"));
    CHECK_EQ(fmt::sfoValue(kv, "PARENTAL_LEVEL"), std::string("2"));
    CHECK_EQ(fmt::sfoValue(kv, "NOPE"), std::string());
    CHECK(!fmt::parseSfo("not an sfo", kv));
    CHECK(!fmt::parseSfo(sfo.substr(0, 30), kv));  // index table cut off
    // Every truncation and a corrupted header must be handled without crashing.
    for (size_t n = 0; n < sfo.size(); ++n) fmt::parseSfo(sfo.substr(0, n), kv);
    std::string bad = sfo;
    put32le(bad, 8, 0xFFFFFFF0u);
    CHECK(!fmt::parseSfo(bad, kv));
}

void testXbox() {
    const std::string xex = makeXex(0x4E4D07D3, 0x1234ABCD);
    struct Case {
        const char* name;
        uint64_t part;
    };
    for (const Case& c : {Case{"x360/xiso.iso", 0}, Case{"x360/xgd3.iso", 0x2080000}, Case{"x360/xgd2.iso", 0xFD90000},
                          Case{"x360/xgd1.iso", 0x18300000}}) {
        writeXboxImage(tmpPath(c.name), c.part, xex);
        std::string title, media;
        CHECK(fmt::xbox360TitleId(tmpStr(c.name), title, &media));
        CHECK_EQ(title, std::string("4E4D07D3"));
        CHECK_EQ(media, std::string("1234ABCD"));
        fs::remove(tmpPath(c.name));  // sparse, but do not leave 400 MB files around
    }
    writeFile("x360/folder/Default.XEX", xex);
    std::string title;
    CHECK(fmt::xbox360TitleId(tmpStr("x360/folder"), title) && title == "4E4D07D3");
    CHECK(fmt::xbox360TitleId(tmpStr("x360/folder/Default.XEX"), title) && title == "4E4D07D3");
    writeFile("x360/notxbox.iso", ps1Iso("BOOT = cdrom:\\SCUS_943.00;1\n"));
    CHECK(!fmt::xbox360TitleId(tmpStr("x360/notxbox.iso"), title) && title.empty());
    std::string badXex = xex;
    put32be(badXex, 0x2C, 0xFFFFFF00u);  // execution info offset out of range
    writeFile("x360/badxex/default.xex", badXex);
    CHECK(!fmt::xbox360TitleId(tmpStr("x360/badxex"), title));
}

std::string zipEntryCrc(const std::vector<RomFile>& files, const std::string& name) {
    for (const auto& f : files)
        if (f.name == name) {
            char b[9];
            std::snprintf(b, sizeof(b), "%08x", f.crc32);
            return b;
        }
    return "(missing)";
}

bool writeDeflatedZip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& entries) {
    fs::create_directories(p.parent_path());
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, pathToUtf8(p).c_str(), 0)) return false;
    bool ok = true;
    for (const auto& [name, data] : entries)
        ok = ok && mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_BEST_COMPRESSION);
    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);
    return ok;
}

void testZipAndRebuild() {
    const std::string a = pattern(5000, 11), b = std::string(70000, 'z') + pattern(3000, 12), c = "tiny";
    std::string err;
    CHECK(writeStoredZip({{"a.bin", a}, {"sub/b.bin", b}, {"\xC3\xBC.bin", c}, {"a.bin", "dup ignored"}},
                         tmpStr("zip/stored.zip"), err));
    std::vector<RomFile> files;
    CHECK(listRomSource(tmpStr("zip/stored.zip"), files, &err));
    CHECK_EQ(files.size(), size_t(3));
    CHECK_EQ(zipEntryCrc(files, "a.bin"), crcHex(a));
    CHECK_EQ(zipEntryCrc(files, "sub/b.bin"), crcHex(b));
    for (const auto& f : files) {
        std::string out;
        CHECK(readRomFile(tmpStr("zip/stored.zip"), f, out, &err));
        CHECK(out == (f.name == "a.bin" ? a : f.name == "sub/b.bin" ? b : c));
    }
    CHECK(fs::exists(tmpPath("zip/stored.zip")) && !fs::exists(tmpPath("zip/stored.zip.tmp")));

    CHECK(writeDeflatedZip(tmpPath("zip/deflated.zip"), {{"a.bin", a}, {"dir/", ""}, {"b.bin", b}}));
    CHECK(listRomSource(tmpStr("zip/deflated.zip"), files, &err));
    CHECK_EQ(files.size(), size_t(2));  // directory entries are skipped
    for (const auto& f : files) {
        std::string out;
        CHECK(f.hasCrc);
        CHECK(readRomFile(tmpStr("zip/deflated.zip"), f, out, &err));
        CHECK(out == (f.name == "a.bin" ? a : b));
        if (f.name == "b.bin") CHECK(f.method == 8 && f.compressedSize < f.size);
    }
    std::string bytes;
    CHECK(findFileByCrc(tmpStr("zip/deflated.zip"), crcHex(b), bytes) && bytes == b);
    CHECK(findFileByCrc(tmpStr("zip/deflated.zip"), "0x" + crcHex(a), bytes) && bytes == a);
    CHECK(!findFileByCrc(tmpStr("zip/deflated.zip"), "12345678", bytes) && bytes.empty());
    CHECK(!findFileByCrc(tmpStr("zip/deflated.zip"), "xyz", bytes));

    // Corrupted entry data: CRC check must fail.
    {
        std::ifstream in(tmpPath("zip/stored.zip"), std::ios::binary);
        std::string z((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        z[30 + 5 + 10] ^= 0x55;  // inside a.bin's data
        writeFile("zip/corrupt.zip", z);
        CHECK(listRomSource(tmpStr("zip/corrupt.zip"), files));
        std::string out;
        CHECK(!files.empty() && !readRomFile(tmpStr("zip/corrupt.zip"), files[0], out, &err));
        // Truncations of a valid zip never crash.
        for (size_t n = 0; n < z.size(); n += 97) {
            writeFile("zip/trunc.zip", z.substr(0, n));
            if (listRomSource(tmpStr("zip/trunc.zip"), files))
                for (const auto& f : files) readRomFile(tmpStr("zip/trunc.zip"), f, out);
        }
    }
    writeFile("zip/notzip.zip", pattern(1000, 5));
    CHECK(!listRomSource(tmpStr("zip/notzip.zip"), files, &err));

    // Folder source with old file names, rebuilt under catalog names.
    const std::string r1 = pattern(4096, 21), r2 = pattern(8192, 22), r3 = pattern(256, 23);
    writeFile("romsrc/old_one.bin", r1);
    writeFile("romsrc/OLD-TWO.ROM", r2);
    writeFile("romsrc/gamma.prom", r3);
    writeFile("romsrc/unrelated.txt", "hello");
    writeFile("romsrc/.DS_Store", "junk");
    GameDef g;
    g.id = "test-arcade";
    g.mediaKind = "romset";
    g.identify.mameSet = "testset";
    for (const auto& [name, data] : std::vector<std::pair<std::string, std::string>>{
             {"new1.rom", r1}, {"new2.rom", r2}, {"gam.1", r3}, {"gam.2", r3}, {"new1-copy.rom", r1}}) {
        KnownDump d;
        d.label = "MAME testset";
        d.file = name;
        d.size = data.size();
        d.crc32 = crcHex(data);
        g.identify.dumps.push_back(d);
    }
    CHECK(rebuildMameSet(g, tmpStr("romsrc"), tmpStr("out/testset.zip"), err));
    CHECK(listRomSource(tmpStr("out/testset.zip"), files));
    CHECK_EQ(files.size(), size_t(5));
    CHECK_EQ(zipEntryCrc(files, "new1.rom"), crcHex(r1));
    CHECK_EQ(zipEntryCrc(files, "new1-copy.rom"), crcHex(r1));
    CHECK_EQ(zipEntryCrc(files, "gam.2"), crcHex(r3));
    CHECK(findFileByCrc(tmpStr("romsrc"), crcHex(r2), bytes) && bytes == r2);
    CHECK(findFileByCrc(tmpStr("romsrc/gamma.prom"), crcHex(r3), bytes) && bytes == r3);

    // Zip source (deflated, arbitrary names), output replaces an existing file.
    CHECK(writeDeflatedZip(tmpPath("romsrc.zip"), {{"x/a", r1}, {"b", r2}, {"c", r3}}));
    CHECK(rebuildMameSet(g, tmpStr("romsrc.zip"), tmpStr("out/testset.zip"), err));
    CHECK(listRomSource(tmpStr("out/testset.zip"), files) && files.size() == 5);
    for (const auto& f : files) {
        std::string out;
        CHECK(readRomFile(tmpStr("out/testset.zip"), f, out) && f.method == 0);
    }

    // Missing ROM: nothing is written.
    fs::remove(tmpPath("romsrc/OLD-TWO.ROM"));
    CHECK(!rebuildMameSet(g, tmpStr("romsrc"), tmpStr("out/incomplete.zip"), err));
    CHECK(err.find("new2.rom") != std::string::npos);
    CHECK(!fs::exists(tmpPath("out/incomplete.zip")) && !fs::exists(tmpPath("out/incomplete.zip.tmp")));
    writeFile("romsrc/OLD-TWO.ROM", r2);

    CHECK(writeSingleFileZip(tmpStr("romsrc/gamma.prom"), "c74.bin", tmpStr("out/namcoc74.zip"), err));
    CHECK(listRomSource(tmpStr("out/namcoc74.zip"), files) && files.size() == 1 && files[0].name == "c74.bin" &&
          zipEntryCrc(files, "c74.bin") == crcHex(r3));
    CHECK(!writeSingleFileZip(tmpStr("romsrc/none"), "x", tmpStr("out/none.zip"), err));

    uint32_t v = 0;
    CHECK(parseCrc32("A3DCE360", v) && v == 0xa3dce360u);
    CHECK(parseCrc32(" 0x0de7775b ", v) && v == 0x0de7775bu);
    CHECK(!parseCrc32("", v) && !parseCrc32("123456789", v) && !parseCrc32("0x", v) && !parseCrc32("12g4", v));
}

std::string gamesJson(const std::map<std::string, std::string>& dumpCrc) {
    auto dump = [&](const std::string& key, const std::string& label) {
        auto it = dumpCrc.find(key);
        return it == dumpCrc.end() ? std::string()
                                   : "{\"label\":\"" + label + "\",\"crc32\":\"" + it->second + "\"}";
    };
    std::string arcade;
    for (int i = 0; i < 10; ++i) {
        const std::string data = pattern(1024 + 512 * size_t(i), 100 + uint32_t(i));
        arcade += std::string(i ? "," : "") + "{\"label\":\"MAME testset\",\"file\":\"rom" + std::to_string(i) +
                  ".bin\",\"size\":" + std::to_string(data.size()) + ",\"crc32\":\"" + crcHex(data) + "\"}";
    }
    return R"J({"games":[
      {"id":"ps1game","title":"PS1 Game","system":"ps1","mediaKind":"cue","acceptedFormats":["cue","chd"],
       "identify":{"bootSerials":["SCUS_943.00"],"nameHints":["ridge racer (usa)"],"dumps":[)J" +
           dump("ps1", "Test track 1") + R"J(]}},
      {"id":"ps2game","title":"PS2 Game","system":"ps2","mediaKind":"cue","acceptedFormats":["cue","iso"],
       "identify":{"bootSerials":["SLUS_200.02"],"nameHints":["ridge racer v"],"dumps":[)J" +
           dump("ps2", "Test PS2") + R"J(]}},
      {"id":"pspgame","title":"PSP Game","system":"psp","mediaKind":"iso","acceptedFormats":["iso"],
       "identify":{"discIds":["UCES00422"],"nameHints":["ridge racer 2"],"dumps":[{"label":"Bad","crc32":"deadbeef"}]}},
      {"id":"ps3game","title":"PS3 Game","system":"ps3","identify":{"discIds":["BLUS30001"]}},
      {"id":"x360game","title":"360 Game","system":"x360","identify":{"xboxTitleIds":["4e4d07d3"]}},
      {"id":"arcade","title":"Arcade","system":"arcade","mediaKind":"romset","acceptedFormats":["zip","folder"],
       "identify":{"mameSet":"testset","nameHints":["testset"],"dumps":[)J" +
           arcade + R"J(]}}
    ]})J";
}

const ScanHit* findHit(const std::vector<ScanHit>& hits, const std::string& pathRel) {
    for (const auto& h : hits)
        if (h.files.path == tmpStr(pathRel)) return &h;
    return nullptr;
}

void testScanner() {
    const fs::path root = tmpPath("scan");
    // PS1 cue + 2 tracks, a lone PS2 bin, PSP iso (PARAM.SFO) and one with only UMD_DATA.BIN.
    const std::string ps1 = toRaw(ps1Iso("BOOT = cdrom:SCUS-943.00;1\r\n"), 2);
    writeFile("scan/disc1/Game (USA) (Track 01).bin", ps1);
    writeFile("scan/disc1/Game (USA) (Track 02).bin", pattern(2352 * 30, 4));
    writeFile("scan/disc1/Game (USA).cue",
              "FILE \"Game (USA) (Track 01).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
              "FILE \"Game (USA) (Track 02).bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n");
    IsoBuilder ps2b;
    ps2b.add("SYSTEM.CNF", "BOOT2 = cdrom0:\\SLUS_200.02;1\r\nVER = 1.10\r\n");
    const std::string ps2 = toRaw(ps2b.build("RIDGERACERV"), 2);
    writeFile("scan/lone/ps2.bin", ps2);
    IsoBuilder pspb;
    pspb.add("UMD_DATA.BIN", "UCES-00422|3026D769FA180971|0001|G");
    pspb.add("PSP_GAME/PARAM.SFO", makeSfo({{"DISC_ID", "UCES00422"}, {"TITLE", "RIDGE RACER 2"}}, {}));
    writeFile("scan/psp/game.iso", pspb.build(""));
    IsoBuilder umdb;
    umdb.add("UMD_DATA.BIN", "UCES-00422|3026D769FA180971|0001|G");
    writeFile("scan/psp2/umd-only.iso", umdb.build(""));
    // PS3 folder, Xbox 360 image and extracted folder.
    writeFile("scan/ps3/My Game/PS3_GAME/PARAM.SFO", makeSfo({{"TITLE_ID", "BLUS30001"}, {"CATEGORY", "DG"}}, {}));
    writeFile("scan/ps3/My Game/PS3_GAME/USRDIR/EBOOT.BIN", "elf");
    writeFile("scan/ps3/My Game/PS3_GAME/USRDIR/data/fake.cue", "garbage");
    writeXboxImage(tmpPath("scan/x360/rr6.iso"), 0, makeXex(0x4E4D07D3, 0));
    writeFile("scan/x360dir/game/default.xex", makeXex(0x4E4D07D3, 0));
    writeFile("scan/x360dir/game/media/fake.iso", "x");
    // Arcade: complete folder (old names + extras), 9/10 folder, deflated zip, 5/10 folder.
    for (int i = 0; i < 10; ++i) {
        const std::string data = pattern(1024 + 512 * size_t(i), 100 + uint32_t(i));
        writeFile("scan/roms/old" + std::to_string(i) + ".x", data);
        if (i < 9) writeFile("scan/roms_partial/p" + std::to_string(i), data);
        if (i < 5) writeFile("scan/roms_half/h" + std::to_string(i), data);
    }
    writeFile("scan/roms/extra.bin", pattern(8192, 7));
    {
        std::vector<std::pair<std::string, std::string>> entries;
        for (int i = 0; i < 10; ++i)
            entries.push_back({"z" + std::to_string(i), pattern(1024 + 512 * size_t(i), 100 + uint32_t(i))});
        CHECK(writeDeflatedZip(tmpPath("scan/zips/whatever.zip"), entries));
    }
    // Name-hint formats, hidden folder, .app bundle, deep nesting and a symlink loop.
    writeFile("scan/chd/Ridge Racer (USA).chd", "chd");
    writeFile("scan/chd/Ridge Racer V (USA).cso", "cso");
    writeFile("scan/chd/Unknown Game.chd", "chd");
    writeFile("scan/.hidden/copy.iso", ps2b.build("X"));
    writeFile("scan/Thing.app/Contents/copy.iso", ps2b.build("X"));
    writeFile("scan/a/b/c/d/e/f/g/deep.bin", ps2);
    std::error_code ec;
    fs::create_directory_symlink(root, tmpPath("scan/loop"), ec);
    const bool haveSymlink = !ec;

    std::map<std::string, std::string> crcs;
    {
        fmt::Digests d;
        fmt::hashFile(tmpStr("scan/disc1/Game (USA) (Track 01).bin"), d);
        crcs["ps1"] = d.crc32;
        fmt::hashFile(tmpStr("scan/lone/ps2.bin"), d);
        crcs["ps2"] = d.crc32;
    }
    Catalog cat;
    std::string err;
    CHECK(cat.loadFromStrings(gamesJson(crcs), R"({"backends":[]})", err));

    ScanProgress progress;
    std::vector<ScanHit> hits = scanFolder(cat, pathToUtf8(root), &progress);
    for (const auto& h : hits)
        std::printf("  hit %-9s %-11s %-22s %-22s %s\n", h.gameId.c_str(), h.files.format.c_str(),
                    h.files.detectedBy.c_str(), h.files.verified.c_str(),
                    pathToUtf8(utf8ToPath(h.files.path).lexically_relative(root)).c_str());

    const ScanHit* h = findHit(hits, "scan/disc1/Game (USA).cue");
    CHECK(h && h->gameId == "ps1game" && h->files.format == "cue" && h->files.detectedBy == "boot SCUS-943.00" &&
          h->serial == "SCUS-943.00" && h->files.size == ps1.size() + 2352 * 30);
    CHECK(!findHit(hits, "scan/disc1/Game (USA) (Track 01).bin"));
    CHECK(!findHit(hits, "scan/disc1/Game (USA) (Track 02).bin"));
    h = findHit(hits, "scan/lone/ps2.bin");
    CHECK(h && h->gameId == "ps2game" && h->files.format == "bin" && h->files.detectedBy == "boot SLUS_200.02");
    h = findHit(hits, "scan/psp/game.iso");
    CHECK(h && h->gameId == "pspgame" && h->files.detectedBy == "disc id UCES00422");
    h = findHit(hits, "scan/psp2/umd-only.iso");
    CHECK(h && h->gameId == "pspgame" && h->serial == "UCES00422");
    h = findHit(hits, "scan/ps3/My Game");
    CHECK(h && h->gameId == "ps3game" && h->files.format == "ps3-folder" && h->files.detectedBy == "title id BLUS30001");
    h = findHit(hits, "scan/x360/rr6.iso");
    CHECK(h && h->gameId == "x360game" && h->files.detectedBy == "title id 4E4D07D3");
    h = findHit(hits, "scan/x360dir/game");
    CHECK(h && h->gameId == "x360game" && h->files.format == "xex-folder");
    h = findHit(hits, "scan/roms");
    CHECK(h && h->gameId == "arcade" && h->files.format == "romset-dir" && h->files.detectedBy == "mame crc 10/10" &&
          h->files.verified == "match:MAME testset");
    h = findHit(hits, "scan/roms_partial");
    CHECK(h && h->files.detectedBy == "mame crc 9/10" && h->files.verified == "partial" &&
          h->message.find("rom9.bin") != std::string::npos);
    CHECK(!findHit(hits, "scan/roms_half"));
    h = findHit(hits, "scan/zips/whatever.zip");
    CHECK(h && h->gameId == "arcade" && h->files.format == "romset-zip" && h->files.verified == "match:MAME testset");
    h = findHit(hits, "scan/chd/Ridge Racer (USA).chd");
    CHECK(h && h->gameId == "ps1game" && h->files.detectedBy == "file name" && h->message.empty());
    h = findHit(hits, "scan/chd/Ridge Racer V (USA).cso");
    CHECK(h && h->gameId == "ps2game" && !h->message.empty());  // cso is not an accepted format there
    CHECK(!findHit(hits, "scan/chd/Unknown Game.chd"));
    CHECK(!findHit(hits, "scan/.hidden/copy.iso"));
    CHECK(!findHit(hits, "scan/Thing.app/Contents/copy.iso"));
    CHECK(!findHit(hits, "scan/a/b/c/d/e/f/g/deep.bin"));  // deeper than maxDepth 6
    for (const auto& x : hits) CHECK(x.files.path.find("loop") == std::string::npos);
    CHECK_EQ(hits.size(), size_t(12));
    CHECK_EQ(progress.hits.load(), int(hits.size()));
    CHECK(progress.filesSeen.load() > 20);
    (void)haveSymlink;

    // A deeper walk does find the nested bin.
    std::vector<ScanHit> deep = scanFolder(cat, pathToUtf8(tmpPath("scan/a")), nullptr, 10);
    CHECK(deep.size() == 1 && deep[0].gameId == "ps2game");
    // Cancellation before the walk returns quickly with nothing.
    ScanProgress cancelled;
    cancelled.cancel = true;
    CHECK(scanFolder(cat, pathToUtf8(root), &cancelled).empty());

    // identifyPath on files and folders.
    ScanHit one;
    CHECK(identifyPath(cat, tmpStr("scan/disc1"), one) && one.files.path == tmpStr("scan/disc1/Game (USA).cue"));
    CHECK(identifyPath(cat, tmpStr("scan/ps3/My Game/PS3_GAME"), one) && one.files.path == tmpStr("scan/ps3/My Game"));
    CHECK(identifyPath(cat, tmpStr("scan/ps3/My Game/PS3_GAME/USRDIR/EBOOT.BIN"), one) && one.gameId == "ps3game");
    CHECK(identifyPath(cat, tmpStr("scan/x360dir/game/default.xex"), one) && one.files.format == "xex-folder");
    CHECK(identifyPath(cat, tmpStr("scan/roms"), one) && one.files.format == "romset-dir");
    CHECK(identifyPath(cat, tmpStr("scan/disc1/Game (USA) (Track 01).bin"), one) && one.gameId == "ps1game");
    CHECK(!identifyPath(cat, tmpStr("scan/disc1/Game (USA) (Track 02).bin"), one));
    CHECK(!identifyPath(cat, tmpStr("scan/nothing-here"), one));
    IsoBuilder unk;
    unk.add("SYSTEM.CNF", "BOOT = cdrom:\\SLUS_999.99;1\n");
    writeFile("scan2/unknown.iso", unk.build("X"));
    CHECK(!identifyPath(cat, tmpStr("scan2/unknown.iso"), one) && one.serial == "SLUS_999.99");

    // Missing data track: reported by name with a warning.
    writeFile("scan2/Ridge Racer (USA).cue", "FILE \"Ridge Racer (USA) (Track 01).bin\" BINARY\n  TRACK 01 MODE2/2352\n");
    CHECK(identifyPath(cat, tmpStr("scan2/Ridge Racer (USA).cue"), one) && one.files.detectedBy == "file name" &&
          one.message.find("missing") != std::string::npos);

    // Verification.
    auto verify = [&](const std::string& rel) {
        const ScanHit* src = findHit(hits, rel);
        ScanHit copy = src ? *src : ScanHit{};
        uint64_t last = 0, total = 0;
        verifyHit(cat, copy, nullptr, [&](uint64_t d, uint64_t t) {
            last = d;
            total = t;
        });
        CHECK(!src || (last == total && total > 0) || copy.files.format == "chd");
        return copy;
    };
    CHECK_EQ(verify("scan/disc1/Game (USA).cue").files.verified, std::string("match:Test track 1"));
    CHECK_EQ(verify("scan/lone/ps2.bin").files.verified, std::string("match:Test PS2"));
    CHECK_EQ(verify("scan/psp/game.iso").files.verified, std::string("mismatch"));
    CHECK_EQ(verify("scan/roms").files.verified, std::string("match:MAME testset"));
    CHECK_EQ(verify("scan/zips/whatever.zip").files.verified, std::string("match:MAME testset"));
    CHECK_EQ(verify("scan/roms_partial").files.verified, std::string("partial"));
    CHECK_EQ(verify("scan/chd/Ridge Racer (USA).chd").files.verified, std::string());

    // A ROM whose CRC matches during the scan but whose data changed afterwards.
    writeFile("scan/roms/old3.x", pattern(1024 + 512 * 3, 999));
    CHECK_EQ(verify("scan/roms").files.verified, std::string("partial"));
}

void testSystemFiles() {
    std::string ps1(512 * 1024, '\0');
    std::memcpy(&ps1[0x108], "CEX-3000 KT-3  by K.S.", 22);
    writeFile("sys/scph5501.bin", ps1);
    writeFile("sys/rom512k.bin", pattern(512 * 1024, 3));
    std::string ps2(4 * 1024 * 1024, '\0');
    std::memcpy(&ps2[0x2700], "RESET\0\0\0\0\0", 10);
    std::memcpy(&ps2[0x2710], "ROMDIR\0\0\0\0", 10);
    writeFile("sys/SCPH-70012.BIN", ps2);
    writeFile("sys/other4m.bin", pattern(4 * 1024 * 1024, 4));
    writeFile("sys/PS3UPDAT.PUP", "SCEUF\0\0\0" + pattern(1000, 5));
    writeFile("sys/bad.pup", pattern(1000, 6));
    CHECK_EQ(identifySystemFile(tmpStr("sys/scph5501.bin")), std::string("ps1-bios"));
    CHECK_EQ(identifySystemFile(tmpStr("sys/rom512k.bin")), std::string());
    CHECK_EQ(identifySystemFile(tmpStr("sys/SCPH-70012.BIN")), std::string("ps2-bios"));
    CHECK_EQ(identifySystemFile(tmpStr("sys/other4m.bin")), std::string());
    CHECK_EQ(identifySystemFile(tmpStr("sys/PS3UPDAT.PUP")), std::string("ps3-firmware"));
    CHECK_EQ(identifySystemFile(tmpStr("sys/bad.pup")), std::string());
    CHECK_EQ(identifySystemFile(tmpStr("sys/missing.bin")), std::string());
    CHECK_EQ(identifySystemFile(tmpStr("sys")), std::string());
}

// Extra catalog files (docs/CATALOG.md): merge rules, and a broken file is skipped as a whole.
void testCatalogExtras() {
    const std::string games = R"({"games": [{"id": "g1", "title": "Game", "backends": [
        {"backend": "port", "os": ["macos", "windows", "linux"], "priority": 100}]}]})";
    Catalog cat;
    std::string err;
    CHECK(cat.loadFromStrings(games, R"({"backends": [{"id": "port", "kind": "port"}]})", err));
    CHECK_EQ(cat.backend("port")->adapter, std::string("port"));  // defaults to the id

    // A new backend that reuses another adapter under its own id, and an option for it.
    CHECK(cat.mergeExtra(R"({"schema": 1,
        "backends": [{"id": "port-wide", "adapter": "port", "kind": "port", "params": {"wide": true}}],
        "gameOptions": {"g1": [{"backend": "port-wide", "os": ["macos"], "priority": 110}]}})", err));
    CHECK(cat.backend("port-wide") && cat.backend("port-wide")->adapter == "port");
    CHECK_EQ(cat.game("g1")->backends.size(), size_t(2));
    CHECK_EQ(cat.optionsFor(*cat.game("g1"), Os::MacOS).front()->backend, std::string("port-wide"));
    CHECK_EQ(cat.optionsFor(*cat.game("g1"), Os::Linux).front()->backend, std::string("port"));
    // A known id replaces the backend in place.
    CHECK(cat.mergeExtra(R"({"backends": [{"id": "port", "name": "Replaced"}]})", err));
    CHECK_EQ(cat.backends().size(), size_t(2));
    CHECK_EQ(cat.backend("port")->name, std::string("Replaced"));

    // Anything wrong rejects the whole file and leaves the catalog as it was.
    for (const char* bad : {
             "{ not json",
             "[]",
             R"({"schema": 2, "backends": [{"id": "x"}]})",
             R"({"backends": {"id": "x"}})",
             R"({"backends": [{"name": "no id"}]})",
             R"({"backends": [{"id": "x"}], "gameOptions": {"nope": [{"backend": "x"}]}})",
             R"({"backends": [{"id": "x"}], "gameOptions": {"g1": [{"backend": "x"}, {"backend": "missing"}]}})",
             R"({"gameOptions": {"g1": {"backend": "port"}}})",
         }) {
        err.clear();
        CHECK(!cat.mergeExtra(bad, err) && !err.empty());
        CHECK(!cat.backend("x"));
        CHECK_EQ(cat.game("g1")->backends.size(), size_t(2));
    }

    // Files: *.json in name order (a later file wins), hidden and other files ignored,
    // a broken file skipped with a warning while the others still load.
    writeFile("extras/a.json", R"({"backends": [{"id": "extra", "name": "A"}]})");
    writeFile("extras/b-broken.json", R"({"backends": [{"id": "broken"}], "gameOptions": {"g1": [{"backend": "zzz"}]}})");
    writeFile("extras/c.json", R"({"backends": [{"id": "extra", "name": "C"}]})");
    writeFile("extras/._a.json", "junk");  // macOS metadata copy
    writeFile("extras/notes.txt", "not a catalog");
    cat.loadExtraFiles(tmpPath("extras"));
    CHECK(cat.backend("extra") && cat.backend("extra")->name == "C");
    CHECK(!cat.backend("broken"));
    CHECK_EQ(cat.warnings().size(), size_t(1));
    CHECK(!cat.warnings().empty() && cat.warnings()[0].find("b-broken.json") != std::string::npos);
    cat.loadExtraFiles(tmpPath("no-such-folder"));
    CHECK_EQ(cat.warnings().size(), size_t(1));
    // Reloading the bundled catalog starts over.
    CHECK(cat.loadFromStrings(games, R"({"backends": [{"id": "port"}]})", err));
    CHECK(cat.warnings().empty() && !cat.backend("extra"));
}

}  // namespace

int main() {
    std::error_code ec;
    g_tmp = fs::temp_directory_path(ec) / "rrm-scanner-tests";
    fs::remove_all(g_tmp, ec);
    fs::create_directories(g_tmp);
    g_tmp = fs::canonical(g_tmp);

    const std::pair<const char*, std::function<void()>> tests[] = {
        {"hashing", testHashing}, {"iso9660", testIso9660},     {"cue", testCue},
        {"console", testConsoleMeta}, {"xbox360", testXbox},     {"zip/rebuild", testZipAndRebuild},
        {"scanner", testScanner}, {"system files", testSystemFiles}, {"catalog extras", testCatalogExtras},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        std::printf("[%s]\n", name);
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::fprintf(stderr, "  exception: %s\n", e.what());
        }
        std::printf("  %s\n", g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures) fs::remove_all(g_tmp, ec);
    return g_failures ? 1 : 0;
}
