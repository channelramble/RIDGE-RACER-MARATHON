// CUE sheet parsing and SectorReader (cooked .iso, raw .bin/.img, first data
// track of a .cue). Kept in one file so the reader can use the INDEX 01 offsets
// that the public CueTrack does not carry.
#include "core/formats/Formats.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>

#include "core/Platform.h"

namespace rrm::fmt {

namespace fs = std::filesystem;

namespace {

struct FileCloser {
    void operator()(std::FILE* f) const {
        if (f) std::fclose(f);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr openRead(const std::string& path) {
    std::FILE* f = nullptr;
#if defined(_WIN32)
    const std::wstring w = rrm::utf8ToPath(path).wstring();
#if defined(_MSC_VER)
    if (_wfopen_s(&f, w.c_str(), L"rb") != 0) f = nullptr;
#else
    f = _wfopen(w.c_str(), L"rb");
#endif
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    // Random sector reads: let every fread go straight to the OS.
    if (f) std::setvbuf(f, nullptr, _IONBF, 0);
    return FilePtr(f);
}

bool readAt(std::FILE* f, uint64_t off, void* out, size_t len) {
#if defined(_WIN32)
    if (_fseeki64(f, static_cast<long long>(off), SEEK_SET) != 0) return false;
#else
    if (fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0) return false;
#endif
    return std::fread(out, 1, len, f) == len;
}

uint64_t sizeOf(const fs::path& p) {
    std::error_code ec;
    const auto s = fs::file_size(p, ec);
    return ec ? 0 : static_cast<uint64_t>(s);
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ') s.remove_prefix(1);
    while (!s.empty() && static_cast<unsigned char>(s.back()) <= ' ') s.remove_suffix(1);
    return s;
}

// Splits off the next whitespace-delimited token; "quoted strings" form one token.
std::string nextToken(std::string_view& s) {
    s = trim(s);
    if (s.empty()) return {};
    if (s.front() == '"') {
        const size_t end = s.find('"', 1);
        std::string tok(s.substr(1, end == std::string_view::npos ? std::string_view::npos : end - 1));
        s = end == std::string_view::npos ? std::string_view() : s.substr(end + 1);
        return tok;
    }
    size_t end = 0;
    while (end < s.size() && static_cast<unsigned char>(s[end]) > ' ') ++end;
    std::string tok(s.substr(0, end));
    s.remove_prefix(end);
    return tok;
}

// Parses a small non-negative decimal; -1 on failure.
int parseNum(std::string_view s) {
    if (s.empty() || s.size() > 6) return -1;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    return v;
}

bool isFileType(const std::string& t) {
    return t == "BINARY" || t == "MOTOROLA" || t == "WAVE" || t == "AIFF" || t == "MP3" || t == "FLAC" ||
           t == "OGG";
}

void setLayout(CueTrack& t) {
    struct Layout {
        const char* mode;
        uint32_t sectorSize, dataOffset;
    };
    static const Layout kLayouts[] = {
        {"MODE1/2048", 2048, 0}, {"MODE2/2048", 2048, 0}, {"MODE1/2352", 2352, 16}, {"MODE2/2352", 2352, 24},
        {"CDI/2352", 2352, 24},  {"MODE2/2336", 2336, 8}, {"CDI/2336", 2336, 8},    {"MODE2/2324", 2324, 0},
        {"CDG", 2448, 0},
    };
    t.sectorSize = 2352;  // AUDIO and unknown modes
    t.dataOffset = 0;
    for (const auto& l : kLayouts)
        if (t.mode == l.mode) {
            t.sectorSize = l.sectorSize;
            t.dataOffset = l.dataOffset;
        }
}

// Resolves a FILE name relative to the cue directory. Falls back to the bare
// file name (stale absolute paths) and to a case-insensitive directory match.
fs::path resolveTrackFile(const fs::path& cueDir, std::string name, bool& exists) {
    std::replace(name.begin(), name.end(), '\\', '/');
    const fs::path rel = rrm::utf8ToPath(name);
    const fs::path first = rel.is_absolute() ? rel : cueDir / rel;
    std::error_code ec;
    exists = fs::is_regular_file(first, ec);
    if (exists) return first;
    const fs::path bare = cueDir / rel.filename();
    if (!rel.filename().empty() && fs::is_regular_file(bare, ec)) {
        exists = true;
        return bare;
    }
    const std::string want = lower(rrm::pathToUtf8(rel.filename()));
    if (want.empty()) return first;
    int budget = 20000;
    fs::directory_iterator it(cueDir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && budget-- > 0; it.increment(ec)) {
        std::error_code fec;
        if (lower(rrm::pathToUtf8(it->path().filename())) == want && it->is_regular_file(fec)) {
            exists = true;
            return it->path();
        }
    }
    return first;
}

struct ParsedCue {
    CueSheet sheet;
    std::vector<uint64_t> index1;  // per track: byte offset of INDEX 01 inside its FILE
};

bool parseCueImpl(const std::string& cuePath, ParsedCue& pc, std::string* error) {
    pc = ParsedCue{};
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    const fs::path cue = rrm::utf8ToPath(cuePath);
    std::error_code ec;
    fs::path absCue = fs::absolute(cue, ec);
    if (ec) absCue = cue;
    const fs::path dir = absCue.parent_path();

    const uint64_t size = sizeOf(cue);
    if (size == 0) return fail("cannot read " + cuePath);
    if (size > 512 * 1024) return fail("not a cue sheet (file too large)");
    FilePtr f = openRead(cuePath);
    if (!f) return fail("cannot open " + cuePath);
    std::string text(static_cast<size_t>(size), '\0');
    text.resize(std::fread(text.data(), 1, text.size(), f.get()));
    if (text.size() >= 3 && std::memcmp(text.data(), "\xEF\xBB\xBF", 3) == 0) text.erase(0, 3);
    if (text.find('\0') != std::string::npos) return fail("not a cue sheet (binary data)");

    std::string curFile;
    bool haveFile = false;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find_first_of("\r\n", pos);
        const size_t stop = eol == std::string::npos ? text.size() : eol;
        std::string_view rest(text.data() + pos, stop - pos);
        pos = stop + 1;
        const std::string kw = upper(nextToken(rest));
        if (kw == "FILE") {
            rest = trim(rest);
            std::string name;
            if (!rest.empty() && rest.front() == '"') {
                name = nextToken(rest);
            } else {
                // Unquoted names may contain spaces: the file type is the last word.
                const size_t sp = rest.find_last_of(" \t");
                if (sp != std::string_view::npos && isFileType(upper(std::string(trim(rest.substr(sp))))))
                    name = std::string(trim(rest.substr(0, sp)));
                else
                    name = std::string(rest);
            }
            if (name.empty()) continue;
            bool exists = false;
            curFile = rrm::pathToUtf8(resolveTrackFile(dir, name, exists).lexically_normal());
            haveFile = true;
            auto& missing = pc.sheet.missingFiles;
            if (!exists && std::find(missing.begin(), missing.end(), curFile) == missing.end())
                missing.push_back(curFile);
        } else if (kw == "TRACK") {
            if (!haveFile) continue;
            const int number = parseNum(nextToken(rest));
            const std::string mode = upper(nextToken(rest));
            if (number <= 0 || number > 99 || mode.empty()) continue;
            CueTrack t;
            t.number = number;
            t.mode = mode;
            t.file = curFile;
            setLayout(t);
            pc.sheet.tracks.push_back(std::move(t));
            pc.index1.push_back(0);
        } else if (kw == "INDEX") {
            if (pc.sheet.tracks.empty()) continue;
            const int idx = parseNum(nextToken(rest));
            const std::string msf = nextToken(rest);
            if (idx != 1) continue;
            const size_t c1 = msf.find(':');
            if (c1 == std::string::npos) continue;
            const size_t c2 = msf.find(':', c1 + 1);
            if (c2 == std::string::npos) continue;
            const int mm = parseNum(std::string_view(msf).substr(0, c1));
            const int ss = parseNum(std::string_view(msf).substr(c1 + 1, c2 - c1 - 1));
            const int ff = parseNum(std::string_view(msf).substr(c2 + 1));
            if (mm < 0 || ss < 0 || ss >= 60 || ff < 0 || ff >= 75) continue;
            const uint64_t frames = (uint64_t(mm) * 60 + uint64_t(ss)) * 75 + uint64_t(ff);
            pc.index1.back() = frames * pc.sheet.tracks.back().sectorSize;
        }
        // REM, TITLE, PERFORMER, PREGAP, POSTGAP, FLAGS, CATALOG ... carry no file data.
    }
    if (pc.sheet.tracks.empty()) return fail("cue sheet has no TRACK entries");
    return true;
}

bool isSync(const uint8_t* s) {
    if (s[0] != 0 || s[11] != 0) return false;
    for (int i = 1; i <= 10; ++i)
        if (s[i] != 0xFF) return false;
    return true;
}

class ImageReader final : public SectorReader {
public:
    ImageReader(FilePtr f, uint64_t base, uint64_t fileSize, uint32_t sectorSize, uint32_t dataOffset)
        : f_(std::move(f)), base_(base), sectorSize_(sectorSize), dataOffset_(dataOffset) {
        count_ = (sectorSize_ && fileSize > base_) ? (fileSize - base_) / sectorSize_ : 0;
    }

    bool read(uint32_t lba, void* out2048) override {
        if (!out2048 || lba >= count_) return false;
        const uint64_t off = base_ + uint64_t(lba) * sectorSize_;
        if (sectorSize_ == 2048) return readAt(f_.get(), off, out2048, 2048);
        uint8_t raw[2448];
        if (sectorSize_ > sizeof(raw) || !readAt(f_.get(), off, raw, sectorSize_)) return false;
        uint32_t data = dataOffset_;
        if (sectorSize_ >= 2352) {
            // Raw sector: the header's mode byte decides where user data starts.
            if (raw[15] == 1) data = 16;
            else if (raw[15] == 2) data = 24;
        }
        if (data + 2048 > sectorSize_) return false;
        std::memcpy(out2048, raw + data, 2048);
        return true;
    }

    uint64_t sectorCount() const override { return count_; }

private:
    FilePtr f_;
    uint64_t base_;
    uint32_t sectorSize_;
    uint32_t dataOffset_;
    uint64_t count_ = 0;
};

std::unique_ptr<SectorReader> openCueReader(const std::string& cuePath) {
    ParsedCue pc;
    if (!parseCueImpl(cuePath, pc, nullptr)) return nullptr;
    for (size_t i = 0; i < pc.sheet.tracks.size(); ++i) {
        const CueTrack& t = pc.sheet.tracks[i];
        if (t.mode == "AUDIO" || t.mode == "CDG") continue;
        const uint64_t size = sizeOf(rrm::utf8ToPath(t.file));
        if (size <= pc.index1[i]) return nullptr;
        FilePtr f = openRead(t.file);
        if (!f) return nullptr;
        return std::make_unique<ImageReader>(std::move(f), pc.index1[i], size, t.sectorSize, t.dataOffset);
    }
    return nullptr;
}

}  // namespace

bool parseCue(const std::string& cuePath, CueSheet& out, std::string* error) {
    try {
        ParsedCue pc;
        const bool ok = parseCueImpl(cuePath, pc, error);
        out = std::move(pc.sheet);
        return ok;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    } catch (...) {
        if (error) *error = "unexpected error";
    }
    out = CueSheet{};
    return false;
}

std::unique_ptr<SectorReader> SectorReader::open(const std::string& path) {
    try {
        const fs::path p = rrm::utf8ToPath(path);
        const std::string ext = lower(rrm::pathToUtf8(p.extension()));
        if (ext == ".cue") return openCueReader(path);
        const uint64_t size = sizeOf(p);
        if (size < 2048) return nullptr;
        FilePtr f = openRead(path);
        if (!f) return nullptr;
        // Raw 2352-byte images carry a sync pattern on every data sector; sector 16 is the PVD.
        bool raw = false;
        if (size >= 17ull * 2352 && (ext != ".iso" || size % 2352 == 0)) {
            uint8_t sync[12];
            raw = readAt(f.get(), 16ull * 2352, sync, sizeof(sync)) && isSync(sync);
        }
        if (raw) return std::make_unique<ImageReader>(std::move(f), 0, size, 2352, 24);
        return std::make_unique<ImageReader>(std::move(f), 0, size, 2048, 0);
    } catch (...) {
        return nullptr;
    }
}

}  // namespace rrm::fmt
