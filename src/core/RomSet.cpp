#include "core/RomSet.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string_view>
#include <system_error>

#include "core/Platform.h"
#include "core/formats/Formats.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz/miniz.h"

namespace rrm {

namespace {

constexpr uint32_t kSigLocal = 0x04034b50, kSigCentral = 0x02014b50, kSigEnd = 0x06054b50;
constexpr uint32_t kSigEnd64 = 0x06064b50, kSigLocator64 = 0x07064b50;
constexpr uint64_t kMaxRomBytes = 1ull << 30;  // in-memory cap per file
constexpr uint64_t kMaxCentralDir = 64ull << 20;
constexpr uint16_t kDosTime = 0;                          // 00:00:00
constexpr uint16_t kDosDate = (20 << 9) | (1 << 5) | 1;  // 2000-01-01, keeps output reproducible

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
void put16(std::string& s, uint16_t v) {
    s += char(v & 0xFF);
    s += char(v >> 8);
}
void put32(std::string& s, uint32_t v) {
    put16(s, uint16_t(v & 0xFFFF));
    put16(s, uint16_t(v >> 16));
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool readAt(std::ifstream& in, uint64_t off, void* out, size_t len) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    if (!in) return false;
    in.read(static_cast<char*>(out), static_cast<std::streamsize>(len));
    return static_cast<size_t>(in.gcount()) == len;
}

bool readWholeFile(const fs::path& p, std::string& out, std::string& err) {
    out.clear();
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    if (ec) {
        err = "cannot stat " + pathToUtf8(p);
        return false;
    }
    if (size > kMaxRomBytes) {
        err = pathToUtf8(p) + " is too large";
        return false;
    }
    std::ifstream in(p, std::ios::binary);
    out.resize(static_cast<size_t>(size));
    if (!in || (size && !in.read(out.data(), static_cast<std::streamsize>(size)))) {
        out.clear();
        err = "cannot read " + pathToUtf8(p);
        return false;
    }
    return true;
}

bool crcOfFile(const fs::path& p, uint32_t& crc, std::vector<char>& buf) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    crc = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        if (n <= 0) break;
        crc = fmt::crc32Update(crc, buf.data(), static_cast<size_t>(n));
    }
    return in.eof() && !in.bad();
}

// Central directory only: names, sizes and CRC32s without touching file data.
bool readZipDirectory(const fs::path& zipPath, std::vector<RomFile>& out, std::string& err) {
    out.clear();
    std::error_code ec;
    const uint64_t size = fs::file_size(zipPath, ec);
    if (ec || size < 22) {
        err = "not a zip file";
        return false;
    }
    std::ifstream in(zipPath, std::ios::binary);
    if (!in) {
        err = "cannot open zip";
        return false;
    }
    // The end record sits in the last 64 KiB + 22 bytes (it may be followed by a comment).
    const size_t tailLen = static_cast<size_t>(std::min<uint64_t>(size, 0xFFFF + 22));
    const uint64_t tailOff = size - tailLen;
    std::vector<uint8_t> tail(tailLen);
    if (!readAt(in, tailOff, tail.data(), tailLen)) {
        err = "cannot read zip";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = tailLen - 22 + 1; i-- > 0;)
        if (le32(&tail[i]) == kSigEnd) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) {
        err = "zip end record not found";
        return false;
    }
    const uint8_t* e = &tail[eocd];
    uint64_t entries = le16(e + 10), cdSize = le32(e + 12), cdOff = le32(e + 16);
    if (entries == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) {
        uint8_t loc[20], z[56];
        const uint64_t eocdAbs = tailOff + eocd;
        if (eocdAbs >= 20 && readAt(in, eocdAbs - 20, loc, sizeof(loc)) && le32(loc) == kSigLocator64) {
            const uint64_t z64 = le64(loc + 8);
            if (z64 < size && size - z64 >= sizeof(z) && readAt(in, z64, z, sizeof(z)) && le32(z) == kSigEnd64) {
                entries = le64(z + 32);
                cdSize = le64(z + 40);
                cdOff = le64(z + 48);
            }
        }
    }
    if (cdOff > size || cdSize > size - cdOff || cdSize > kMaxCentralDir) {
        err = "corrupt zip central directory";
        return false;
    }
    std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
    if (!cd.empty() && !readAt(in, cdOff, cd.data(), cd.size())) {
        err = "cannot read zip central directory";
        return false;
    }
    size_t pos = 0;
    for (uint64_t i = 0; i < entries && pos + 46 <= cd.size(); ++i) {
        const uint8_t* h = &cd[pos];
        if (le32(h) != kSigCentral) break;
        const size_t nameLen = le16(h + 28), extraLen = le16(h + 30), commentLen = le16(h + 32);
        if (pos + 46 + nameLen + extraLen + commentLen > cd.size()) break;
        RomFile f;
        f.flags = le16(h + 8);
        f.method = le16(h + 10);
        f.crc32 = le32(h + 16);
        f.hasCrc = true;
        f.compressedSize = le32(h + 20);
        f.size = le32(h + 24);
        f.zipOffset = le32(h + 42);
        f.name.assign(reinterpret_cast<const char*>(h + 46), nameLen);
        // ZIP64 extra field: 64-bit values replace 0xFFFFFFFF placeholders, in this order.
        const uint8_t* x = h + 46 + nameLen;
        const uint8_t* xend = x + extraLen;
        while (xend - x >= 4) {
            const uint16_t id = le16(x), len = le16(x + 2);
            const uint8_t* d = x + 4;
            if (len > xend - d) break;
            const uint8_t* dend = d + len;
            if (id == 0x0001) {
                for (uint64_t* field : {&f.size, &f.compressedSize, &f.zipOffset}) {
                    if (*field != 0xFFFFFFFFu || dend - d < 8) continue;
                    *field = le64(d);
                    d += 8;
                }
            }
            x = dend;
        }
        pos += 46 + nameLen + extraLen + commentLen;
        if (!f.name.empty() && f.name.back() != '/' && f.name.back() != '\\') out.push_back(std::move(f));
    }
    return true;
}

bool readZipEntry(const fs::path& zipPath, const RomFile& f, std::string& out, std::string& err) {
    out.clear();
    if (f.flags & 1) {
        err = "encrypted zip entries are not supported";
        return false;
    }
    if (f.size > kMaxRomBytes || f.compressedSize > kMaxRomBytes) {
        err = "entry too large";
        return false;
    }
    std::ifstream in(zipPath, std::ios::binary);
    uint8_t lh[30];
    if (!in || !readAt(in, f.zipOffset, lh, sizeof(lh)) || le32(lh) != kSigLocal) {
        err = "bad zip local header";
        return false;
    }
    const uint64_t dataOff = f.zipOffset + 30 + le16(lh + 26) + le16(lh + 28);
    std::string comp(static_cast<size_t>(f.compressedSize), '\0');
    if (!comp.empty() && !readAt(in, dataOff, comp.data(), comp.size())) {
        err = "truncated zip entry";
        return false;
    }
    if (f.method == 0) {
        if (f.compressedSize != f.size) {
            err = "stored entry size mismatch";
            return false;
        }
        out = std::move(comp);
    } else if (f.method == 8) {
        out.assign(static_cast<size_t>(f.size), '\0');
        if (!out.empty()) {
            // Raw deflate (no zlib header) straight into the exactly-sized output.
            const size_t got = tinfl_decompress_mem_to_mem(out.data(), out.size(), comp.data(), comp.size(), 0);
            if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got != out.size()) {
                out.clear();
                err = "inflate failed";
                return false;
            }
        }
    } else {
        err = "unsupported zip compression method " + std::to_string(f.method);
        return false;
    }
    if (fmt::crc32Update(0, out.data(), out.size()) != f.crc32) {
        out.clear();
        err = "CRC mismatch";
        return false;
    }
    return true;
}

bool hasNonAscii(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
}

// Streams stored entries; only the central directory records stay in memory.
class StoredZipWriter {
public:
    explicit StoredZipWriter(const fs::path& path) : out_(path, std::ios::binary | std::ios::trunc) {}
    bool ok() const { return static_cast<bool>(out_); }

    bool add(const std::string& name, const std::string& data, std::string& err) {
        if (!out_) {
            err = "write error";
            return false;
        }
        if (name.empty() || name.size() > 0xFFFF) {
            err = "invalid zip entry name";
            return false;
        }
        if (data.size() >= 0xFFFFFFFFull || offset_ + 30 + name.size() + data.size() >= 0xFFFFFFFFull ||
            entries_.size() >= 0xFFFF) {
            err = "zip would need ZIP64 (not supported)";
            return false;
        }
        Entry e;
        e.name = name;
        e.crc = fmt::crc32Update(0, data.data(), data.size());
        e.size = static_cast<uint32_t>(data.size());
        e.offset = static_cast<uint32_t>(offset_);
        e.flags = hasNonAscii(name) ? 0x0800 : 0;  // bit 11: UTF-8 name
        std::string h;
        put32(h, kSigLocal);
        put16(h, 10);  // version needed: 1.0 (stored)
        put16(h, e.flags);
        put16(h, 0);  // method: stored
        put16(h, kDosTime);
        put16(h, kDosDate);
        put32(h, e.crc);
        put32(h, e.size);
        put32(h, e.size);
        put16(h, static_cast<uint16_t>(name.size()));
        put16(h, 0);
        h += name;
        out_.write(h.data(), static_cast<std::streamsize>(h.size()));
        out_.write(data.data(), static_cast<std::streamsize>(data.size()));
        offset_ += h.size() + data.size();
        entries_.push_back(std::move(e));
        if (!out_) {
            err = "write error";
            return false;
        }
        return true;
    }

    bool finish(std::string& err) {
        std::string cd;
        for (const auto& e : entries_) {
            put32(cd, kSigCentral);
            put16(cd, 20);  // made by: MS-DOS, spec 2.0
            put16(cd, 10);
            put16(cd, e.flags);
            put16(cd, 0);
            put16(cd, kDosTime);
            put16(cd, kDosDate);
            put32(cd, e.crc);
            put32(cd, e.size);
            put32(cd, e.size);
            put16(cd, static_cast<uint16_t>(e.name.size()));
            put16(cd, 0);  // extra
            put16(cd, 0);  // comment
            put16(cd, 0);  // disk
            put16(cd, 0);  // internal attributes
            put32(cd, 0);  // external attributes
            put32(cd, e.offset);
            cd += e.name;
        }
        if (offset_ + cd.size() >= 0xFFFFFFFFull) {
            err = "zip would need ZIP64 (not supported)";
            return false;
        }
        std::string end;
        put32(end, kSigEnd);
        put16(end, 0);
        put16(end, 0);
        put16(end, static_cast<uint16_t>(entries_.size()));
        put16(end, static_cast<uint16_t>(entries_.size()));
        put32(end, static_cast<uint32_t>(cd.size()));
        put32(end, static_cast<uint32_t>(offset_));
        put16(end, 0);
        out_.write(cd.data(), static_cast<std::streamsize>(cd.size()));
        out_.write(end.data(), static_cast<std::streamsize>(end.size()));
        out_.close();
        if (out_.fail()) {
            err = "write error";
            return false;
        }
        return true;
    }

private:
    struct Entry {
        std::string name;
        uint32_t crc = 0, size = 0, offset = 0;
        uint16_t flags = 0;
    };
    std::ofstream out_;
    uint64_t offset_ = 0;
    std::vector<Entry> entries_;
};

// Writes to "<out>.tmp" through `fill`, then renames over `outZip`.
template <class Fill>
bool writeZipAtomically(const std::string& outZip, std::string& error, Fill&& fill) {
    const fs::path out = utf8ToPath(outZip);
    std::error_code ec;
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    fs::path tmp = out;
    tmp += ".tmp";
    bool ok = false;
    {
        StoredZipWriter w(tmp);
        if (!w.ok()) error = "cannot create " + pathToUtf8(tmp);
        else ok = fill(w) && w.finish(error);
    }
    if (ok) {
        fs::rename(tmp, out, ec);
        if (ec) {  // some platforms refuse to rename over an existing file
            fs::remove(out, ec);
            ec.clear();
            fs::rename(tmp, out, ec);
        }
        if (ec) {
            error = "cannot replace " + outZip + ": " + ec.message();
            ok = false;
        }
    }
    if (!ok) fs::remove(tmp, ec);
    return ok;
}

std::string joinNames(const std::vector<std::string>& names, size_t maxShown) {
    std::string s;
    for (size_t i = 0; i < names.size() && i < maxShown; ++i) s += (i ? ", " : "") + names[i];
    if (names.size() > maxShown) s += ", ...";
    return s;
}

}  // namespace

bool parseCrc32(const std::string& hex, uint32_t& out) {
    std::string_view s(hex);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
    if (s.empty() || s.size() > 8) return false;
    uint32_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | uint32_t(d);
    }
    out = v;
    return true;
}

bool listRomSource(const std::string& sourcePath, std::vector<RomFile>& out, std::string* error) {
    out.clear();
    std::string err;
    try {
        const fs::path p = utf8ToPath(sourcePath);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
            for (int budget = 100000; !ec && it != end && budget-- > 0; it.increment(ec)) {
                std::error_code fec;
                if (!it->is_regular_file(fec)) continue;
                RomFile f;
                f.name = pathToUtf8(it->path().filename());
                if (f.name.empty() || f.name[0] == '.') continue;
                f.path = pathToUtf8(it->path());
                f.size = it->file_size(fec);
                if (!fec) out.push_back(std::move(f));
            }
            std::sort(out.begin(), out.end(), [](const RomFile& a, const RomFile& b) { return a.name < b.name; });
            return true;
        }
        if (fs::is_regular_file(p, ec)) {
            if (lower(pathToUtf8(p.extension())) == ".zip") {
                if (readZipDirectory(p, out, err)) return true;
            } else {
                RomFile f;
                f.name = pathToUtf8(p.filename());
                f.path = sourcePath;
                f.size = fs::file_size(p, ec);
                if (!ec) {
                    out.push_back(std::move(f));
                    return true;
                }
                err = "cannot stat " + sourcePath;
            }
        } else {
            err = sourcePath + " not found";
        }
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "unexpected error";
    }
    out.clear();
    if (error) *error = err;
    return false;
}

bool computeRomCrcs(std::vector<RomFile>& files, const std::function<bool(const RomFile&)>& filter,
                    const std::atomic<bool>* cancel) {
    try {
        std::vector<char> buf(1 << 20);
        for (auto& f : files) {
            if (f.hasCrc || f.path.empty() || (filter && !filter(f))) continue;
            if (cancel && cancel->load()) return false;
            uint32_t crc = 0;
            if (crcOfFile(utf8ToPath(f.path), crc, buf)) {
                f.crc32 = crc;
                f.hasCrc = true;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool readRomFile(const std::string& sourcePath, const RomFile& file, std::string& out, std::string* error) {
    out.clear();
    std::string err;
    try {
        if (!file.path.empty()) {
            if (readWholeFile(utf8ToPath(file.path), out, err)) {
                if (!file.hasCrc || fmt::crc32Update(0, out.data(), out.size()) == file.crc32) return true;
                err = "CRC mismatch (file changed?)";
            }
        } else if (readZipEntry(utf8ToPath(sourcePath), file, out, err)) {
            return true;
        }
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "unexpected error";
    }
    out.clear();
    if (error) *error = file.name + ": " + err;
    return false;
}

bool findFileByCrc(const std::string& sourcePath, const std::string& crc32hex, std::string& outBytes) {
    outBytes.clear();
    try {
        uint32_t want = 0;
        std::vector<RomFile> files;
        if (!parseCrc32(crc32hex, want) || !listRomSource(sourcePath, files)) return false;
        // Zip entries already carry CRCs; loose files are hashed until one matches.
        for (auto& f : files) {
            if (!f.hasCrc && f.size <= kMaxRomBytes) {
                std::vector<RomFile> one{f};
                computeRomCrcs(one);
                f = std::move(one.front());
            }
            if (f.hasCrc && f.crc32 == want && readRomFile(sourcePath, f, outBytes)) return true;
        }
    } catch (...) {
    }
    outBytes.clear();
    return false;
}

bool writeStoredZip(const std::vector<std::pair<std::string, std::string>>& entries, const std::string& outZip,
                    std::string& error) {
    error.clear();
    try {
        return writeZipAtomically(outZip, error, [&](StoredZipWriter& w) {
            std::set<std::string> seen;
            for (const auto& [name, data] : entries) {
                if (!seen.insert(name).second) continue;
                if (!w.add(name, data, error)) return false;
            }
            return true;
        });
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unexpected error";
    }
    return false;
}

bool writeSingleFileZip(const std::string& srcFile, const std::string& nameInZip, const std::string& outZip,
                        std::string& error) {
    error.clear();
    try {
        std::string bytes;
        if (!readWholeFile(utf8ToPath(srcFile), bytes, error)) return false;
        return writeStoredZip({{nameInZip, std::move(bytes)}}, outZip, error);
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unexpected error";
    }
    return false;
}

bool rebuildMameSet(const GameDef& game, const std::string& sourcePath, const std::string& outZipPath,
                    std::string& error) {
    error.clear();
    try {
        const auto& dumps = game.identify.dumps;
        if (dumps.empty()) {
            error = game.id + " has no ROM list in the catalog";
            return false;
        }
        std::vector<RomFile> files;
        if (!listRomSource(sourcePath, files, &error)) return false;
        std::set<uint64_t> sizes;
        for (const auto& d : dumps) sizes.insert(d.size);
        computeRomCrcs(files, [&](const RomFile& f) { return sizes.count(f.size) || sizes.count(0); });

        // Resolve every entry before writing so an incomplete set never produces a zip.
        struct Pick {
            const KnownDump* dump;
            const RomFile* file;
        };
        std::vector<Pick> picks;
        std::vector<std::string> missing;
        std::set<std::string> names;
        for (const auto& d : dumps) {
            if (d.file.empty()) {
                error = "catalog entry " + d.crc32 + " has no file name";
                return false;
            }
            if (!names.insert(lower(d.file)).second) continue;
            uint32_t crc = 0;
            const RomFile* src = nullptr;
            if (parseCrc32(d.crc32, crc))
                for (const auto& f : files)
                    if (f.hasCrc && f.crc32 == crc && (d.size == 0 || f.size == d.size)) {
                        src = &f;
                        break;
                    }
            if (src) picks.push_back({&d, src});
            else missing.push_back(d.file);
        }
        if (!missing.empty()) {
            error = "missing " + std::to_string(missing.size()) + " of " + std::to_string(names.size()) +
                    " ROMs: " + joinNames(missing, 8);
            return false;
        }
        return writeZipAtomically(outZipPath, error, [&](StoredZipWriter& w) {
            std::string bytes;
            for (const auto& p : picks) {
                if (!readRomFile(sourcePath, *p.file, bytes, &error)) return false;
                if (!w.add(p.dump->file, bytes, error)) return false;
            }
            return true;
        });
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unexpected error";
    }
    return false;
}

}  // namespace rrm
