// Low-level readers for the media formats Marathon must recognise.
// All readers are read-only and never load whole images into memory.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rrm::fmt {

// ---- hashing ---------------------------------------------------------------
struct Digests {
    std::string crc32, md5, sha1;  // lower-case hex
    uint64_t size = 0;
};
// Streams the file once computing all three digests. Returns false on I/O error
// or when *cancel becomes true.
bool hashFile(const std::string& path, Digests& out, const std::atomic<bool>* cancel = nullptr,
              const std::function<void(uint64_t, uint64_t)>& progress = {});
uint32_t crc32Update(uint32_t crc, const void* data, size_t len);

// ---- CUE sheets --------------------------------------------------------------
struct CueTrack {
    int number = 0;
    std::string mode;     // "MODE1/2352", "MODE2/2352", "AUDIO", "MODE1/2048"
    std::string file;     // absolute path of the backing file
    uint32_t sectorSize = 2352;
    uint32_t dataOffset = 0;  // byte offset of user data inside each raw sector (16 or 24)
};
struct CueSheet {
    std::vector<CueTrack> tracks;
    std::vector<std::string> missingFiles;
};
bool parseCue(const std::string& cuePath, CueSheet& out, std::string* error = nullptr);

// ---- sector-addressed image ----------------------------------------------------
// Presents 2048-byte logical sectors for ISO9660 regardless of the image type
// (.iso cooked 2048, raw 2352 MODE1/MODE2, track 1 of a CUE).
class SectorReader {
public:
    static std::unique_ptr<SectorReader> open(const std::string& path);  // .iso/.bin/.cue/.img
    virtual ~SectorReader() = default;
    virtual bool read(uint32_t lba, void* out2048) = 0;
    virtual uint64_t sectorCount() const = 0;
};

// ---- ISO9660 -----------------------------------------------------------------------
struct IsoEntry {
    std::string name;   // without ";1"
    bool directory = false;
    uint64_t size = 0;
    std::vector<std::pair<uint32_t, uint32_t>> extents;  // (lba, bytes) — multi-extent safe
};
class Iso9660 {
public:
    explicit Iso9660(SectorReader& reader) : r_(reader) {}
    bool mount();                                              // reads the PVD
    bool find(const std::string& path, IsoEntry& out);         // "/SYSTEM.CNF", case-insensitive
    bool list(const std::string& dir, std::vector<IsoEntry>& out);
    bool readFile(const IsoEntry& e, std::string& out, uint64_t maxBytes = 1 << 20);
    std::string volumeId() const { return volumeId_; }
private:
    SectorReader& r_;
    IsoEntry root_;
    std::string volumeId_;
};

// ---- console metadata -------------------------------------------------------------
// "BOOT = cdrom:\SCUS_943.00;1" / "BOOT2 = cdrom0:\SLUS_200.02;1" -> "SCUS_943.00"
std::string bootSerialFromSystemCnf(const std::string& systemCnf);
// PARAM.SFO key/value pairs (PSP UMD and PS3 discs). Returns false if invalid.
bool parseSfo(const std::string& data, std::vector<std::pair<std::string, std::string>>& out);
std::string sfoValue(const std::vector<std::pair<std::string, std::string>>& kv, const std::string& key);

// Xbox 360 disc (XGD2/XGD3 XDVDFS): locate default.xex and read its title id.
// Returns "" when not an Xbox 360 image. titleId is upper-case hex, e.g. "4E4D07D2".
bool xbox360TitleId(const std::string& isoPath, std::string& titleId, std::string* mediaId = nullptr);

}  // namespace rrm::fmt
