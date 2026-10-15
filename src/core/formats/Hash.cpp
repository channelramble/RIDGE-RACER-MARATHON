// CRC32 (zlib polynomial), MD5 and SHA-1 without external dependencies, plus
// a streaming hashFile() that computes all three in one pass.
#include "core/formats/Formats.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <future>
#include <system_error>
#include <vector>

#include "core/Platform.h"

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace rrm::fmt {

namespace {

inline uint32_t rotl(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

std::string toHex(const uint8_t* p, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = digits[p[i] >> 4];
        s[2 * i + 1] = digits[p[i] & 15];
    }
    return s;
}

#if !defined(__ARM_FEATURE_CRC32)
// Slice-by-8 tables for the reflected polynomial 0xEDB88320.
struct CrcTables {
    uint32_t t[8][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};
const CrcTables& crcTables() {
    static const CrcTables tables;
    return tables;
}
#endif

// Shared Merkle-Damgard buffering for the two 64-byte-block digests.
template <class Derived>
class BlockHash {
public:
    void update(const uint8_t* p, size_t n) {
        total_ += n;
        if (fill_) {
            size_t take = std::min(n, sizeof(buf_) - fill_);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take;
            p += take;
            n -= take;
            if (fill_ < sizeof(buf_)) return;
            static_cast<Derived*>(this)->block(buf_);
            fill_ = 0;
        }
        for (; n >= 64; p += 64, n -= 64) static_cast<Derived*>(this)->block(p);
        if (n) {
            std::memcpy(buf_, p, n);
            fill_ = n;
        }
    }

protected:
    // Appends 0x80, zero padding and the 64-bit bit length (big or little endian).
    void pad(bool bigEndianLength) {
        const uint64_t bits = total_ * 8;
        static const uint8_t zeros[64] = {0x80};
        size_t padLen = fill_ < 56 ? 56 - fill_ : 120 - fill_;
        update(zeros, padLen);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (bigEndianLength ? 56 - 8 * i : 8 * i));
        update(len, 8);
    }

private:
    uint64_t total_ = 0;
    uint8_t buf_[64] = {};
    size_t fill_ = 0;
};

class Md5 : public BlockHash<Md5> {
public:
    std::string finishHex() {
        pad(false);
        uint8_t out[16];
        for (int i = 0; i < 4; ++i)
            for (int b = 0; b < 4; ++b) out[4 * i + b] = uint8_t(s_[i] >> (8 * b));
        return toHex(out, 16);
    }
    void block(const uint8_t* p);

private:
    uint32_t s_[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
};

#define MD5_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define MD5_G(x, y, z) ((y) ^ ((z) & ((x) ^ (y))))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_STEP(f, a, b, c, d, x, t, s) \
    (a) += f((b), (c), (d)) + (x) + (t);  \
    (a) = rotl((a), (s));                 \
    (a) += (b);

void Md5::block(const uint8_t* p) {
    uint32_t x[16];
    for (int i = 0; i < 16; ++i) x[i] = le32(p + 4 * i);
    uint32_t a = s_[0], b = s_[1], c = s_[2], d = s_[3];

    MD5_STEP(MD5_F, a, b, c, d, x[0], 0xd76aa478u, 7)
    MD5_STEP(MD5_F, d, a, b, c, x[1], 0xe8c7b756u, 12)
    MD5_STEP(MD5_F, c, d, a, b, x[2], 0x242070dbu, 17)
    MD5_STEP(MD5_F, b, c, d, a, x[3], 0xc1bdceeeu, 22)
    MD5_STEP(MD5_F, a, b, c, d, x[4], 0xf57c0fafu, 7)
    MD5_STEP(MD5_F, d, a, b, c, x[5], 0x4787c62au, 12)
    MD5_STEP(MD5_F, c, d, a, b, x[6], 0xa8304613u, 17)
    MD5_STEP(MD5_F, b, c, d, a, x[7], 0xfd469501u, 22)
    MD5_STEP(MD5_F, a, b, c, d, x[8], 0x698098d8u, 7)
    MD5_STEP(MD5_F, d, a, b, c, x[9], 0x8b44f7afu, 12)
    MD5_STEP(MD5_F, c, d, a, b, x[10], 0xffff5bb1u, 17)
    MD5_STEP(MD5_F, b, c, d, a, x[11], 0x895cd7beu, 22)
    MD5_STEP(MD5_F, a, b, c, d, x[12], 0x6b901122u, 7)
    MD5_STEP(MD5_F, d, a, b, c, x[13], 0xfd987193u, 12)
    MD5_STEP(MD5_F, c, d, a, b, x[14], 0xa679438eu, 17)
    MD5_STEP(MD5_F, b, c, d, a, x[15], 0x49b40821u, 22)

    MD5_STEP(MD5_G, a, b, c, d, x[1], 0xf61e2562u, 5)
    MD5_STEP(MD5_G, d, a, b, c, x[6], 0xc040b340u, 9)
    MD5_STEP(MD5_G, c, d, a, b, x[11], 0x265e5a51u, 14)
    MD5_STEP(MD5_G, b, c, d, a, x[0], 0xe9b6c7aau, 20)
    MD5_STEP(MD5_G, a, b, c, d, x[5], 0xd62f105du, 5)
    MD5_STEP(MD5_G, d, a, b, c, x[10], 0x02441453u, 9)
    MD5_STEP(MD5_G, c, d, a, b, x[15], 0xd8a1e681u, 14)
    MD5_STEP(MD5_G, b, c, d, a, x[4], 0xe7d3fbc8u, 20)
    MD5_STEP(MD5_G, a, b, c, d, x[9], 0x21e1cde6u, 5)
    MD5_STEP(MD5_G, d, a, b, c, x[14], 0xc33707d6u, 9)
    MD5_STEP(MD5_G, c, d, a, b, x[3], 0xf4d50d87u, 14)
    MD5_STEP(MD5_G, b, c, d, a, x[8], 0x455a14edu, 20)
    MD5_STEP(MD5_G, a, b, c, d, x[13], 0xa9e3e905u, 5)
    MD5_STEP(MD5_G, d, a, b, c, x[2], 0xfcefa3f8u, 9)
    MD5_STEP(MD5_G, c, d, a, b, x[7], 0x676f02d9u, 14)
    MD5_STEP(MD5_G, b, c, d, a, x[12], 0x8d2a4c8au, 20)

    MD5_STEP(MD5_H, a, b, c, d, x[5], 0xfffa3942u, 4)
    MD5_STEP(MD5_H, d, a, b, c, x[8], 0x8771f681u, 11)
    MD5_STEP(MD5_H, c, d, a, b, x[11], 0x6d9d6122u, 16)
    MD5_STEP(MD5_H, b, c, d, a, x[14], 0xfde5380cu, 23)
    MD5_STEP(MD5_H, a, b, c, d, x[1], 0xa4beea44u, 4)
    MD5_STEP(MD5_H, d, a, b, c, x[4], 0x4bdecfa9u, 11)
    MD5_STEP(MD5_H, c, d, a, b, x[7], 0xf6bb4b60u, 16)
    MD5_STEP(MD5_H, b, c, d, a, x[10], 0xbebfbc70u, 23)
    MD5_STEP(MD5_H, a, b, c, d, x[13], 0x289b7ec6u, 4)
    MD5_STEP(MD5_H, d, a, b, c, x[0], 0xeaa127fau, 11)
    MD5_STEP(MD5_H, c, d, a, b, x[3], 0xd4ef3085u, 16)
    MD5_STEP(MD5_H, b, c, d, a, x[6], 0x04881d05u, 23)
    MD5_STEP(MD5_H, a, b, c, d, x[9], 0xd9d4d039u, 4)
    MD5_STEP(MD5_H, d, a, b, c, x[12], 0xe6db99e5u, 11)
    MD5_STEP(MD5_H, c, d, a, b, x[15], 0x1fa27cf8u, 16)
    MD5_STEP(MD5_H, b, c, d, a, x[2], 0xc4ac5665u, 23)

    MD5_STEP(MD5_I, a, b, c, d, x[0], 0xf4292244u, 6)
    MD5_STEP(MD5_I, d, a, b, c, x[7], 0x432aff97u, 10)
    MD5_STEP(MD5_I, c, d, a, b, x[14], 0xab9423a7u, 15)
    MD5_STEP(MD5_I, b, c, d, a, x[5], 0xfc93a039u, 21)
    MD5_STEP(MD5_I, a, b, c, d, x[12], 0x655b59c3u, 6)
    MD5_STEP(MD5_I, d, a, b, c, x[3], 0x8f0ccc92u, 10)
    MD5_STEP(MD5_I, c, d, a, b, x[10], 0xffeff47du, 15)
    MD5_STEP(MD5_I, b, c, d, a, x[1], 0x85845dd1u, 21)
    MD5_STEP(MD5_I, a, b, c, d, x[8], 0x6fa87e4fu, 6)
    MD5_STEP(MD5_I, d, a, b, c, x[15], 0xfe2ce6e0u, 10)
    MD5_STEP(MD5_I, c, d, a, b, x[6], 0xa3014314u, 15)
    MD5_STEP(MD5_I, b, c, d, a, x[13], 0x4e0811a1u, 21)
    MD5_STEP(MD5_I, a, b, c, d, x[4], 0xf7537e82u, 6)
    MD5_STEP(MD5_I, d, a, b, c, x[11], 0xbd3af235u, 10)
    MD5_STEP(MD5_I, c, d, a, b, x[2], 0x2ad7d2bbu, 15)
    MD5_STEP(MD5_I, b, c, d, a, x[9], 0xeb86d391u, 21)

    s_[0] += a;
    s_[1] += b;
    s_[2] += c;
    s_[3] += d;
}

#undef MD5_F
#undef MD5_G
#undef MD5_H
#undef MD5_I
#undef MD5_STEP

class Sha1 : public BlockHash<Sha1> {
public:
    std::string finishHex() {
        pad(true);
        uint8_t out[20];
        for (int i = 0; i < 5; ++i)
            for (int b = 0; b < 4; ++b) out[4 * i + b] = uint8_t(h_[i] >> (24 - 8 * b));
        return toHex(out, 20);
    }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = be32(p + 4 * i);
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        auto round = [&](uint32_t f, uint32_t k, uint32_t wi) {
            uint32_t t = rotl(a, 5) + f + e + k + wi;
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        };
        for (int i = 0; i < 20; ++i) round(d ^ (b & (c ^ d)), 0x5A827999u, w[i]);
        for (int i = 20; i < 40; ++i) round(b ^ c ^ d, 0x6ED9EBA1u, w[i]);
        for (int i = 40; i < 60; ++i) round((b & c) | (d & (b | c)), 0x8F1BBCDCu, w[i]);
        for (int i = 60; i < 80; ++i) round(b ^ c ^ d, 0xCA62C1D6u, w[i]);
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

private:
    uint32_t h_[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
};

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
    if (f) std::setvbuf(f, nullptr, _IONBF, 0);  // large reads go straight to the OS
    return FilePtr(f);
}

}  // namespace

uint32_t crc32Update(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
#if defined(__ARM_FEATURE_CRC32)
    // ARMv8 CRC32 instructions use the same reflected 0x04C11DB7 polynomial as zlib.
    for (; len && (reinterpret_cast<uintptr_t>(p) & 7); --len) crc = __crc32b(crc, *p++);
    for (; len >= 8; len -= 8, p += 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        crc = __crc32d(crc, v);
    }
    for (; len; --len) crc = __crc32b(crc, *p++);
#else
    const auto& T = crcTables().t;
    for (; len >= 8; len -= 8, p += 8) {
        uint32_t a = crc ^ le32(p);
        uint32_t b = le32(p + 4);
        crc = T[7][a & 0xFF] ^ T[6][(a >> 8) & 0xFF] ^ T[5][(a >> 16) & 0xFF] ^ T[4][a >> 24] ^ T[3][b & 0xFF] ^
              T[2][(b >> 8) & 0xFF] ^ T[1][(b >> 16) & 0xFF] ^ T[0][b >> 24];
    }
    for (; len; --len) crc = (crc >> 8) ^ T[0][(crc ^ *p++) & 0xFF];
#endif
    return ~crc;
}

bool hashFile(const std::string& path, Digests& out, const std::atomic<bool>* cancel,
              const std::function<void(uint64_t, uint64_t)>& progress) {
    out = Digests{};
    try {
        FilePtr f = openRead(path);
        if (!f) return false;
        std::error_code ec;
        uint64_t total = std::filesystem::file_size(rrm::utf8ToPath(path), ec);
        if (ec) total = 0;

        constexpr size_t kChunk = 4u << 20;
        std::vector<uint8_t> bufs[2] = {std::vector<uint8_t>(kChunk), std::vector<uint8_t>(kChunk)};
        uint32_t crc = 0;
        Md5 md5;
        Sha1 sha1;
        uint64_t done = 0;
        int cur = 0;
        size_t n = std::fread(bufs[cur].data(), 1, kChunk, f.get());
        while (n > 0) {
            if (cancel && cancel->load()) return false;
            const uint8_t* p = bufs[cur].data();
            // MD5 runs on a helper thread while this thread does CRC32 + SHA-1 and
            // reads the next chunk into the other buffer.
            std::future<void> md5Task;
            try {
                md5Task = std::async(std::launch::async, [&md5, p, n] { md5.update(p, n); });
            } catch (const std::system_error&) {
                md5.update(p, n);  // no threads available: stay sequential
            }
            crc = crc32Update(crc, p, n);
            sha1.update(p, n);
            size_t next = std::fread(bufs[cur ^ 1].data(), 1, kChunk, f.get());
            if (md5Task.valid()) md5Task.get();
            done += n;
            if (progress) progress(done, total > done ? total : done);
            cur ^= 1;
            n = next;
        }
        if (std::ferror(f.get())) return false;

        uint8_t c[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
        out.crc32 = toHex(c, 4);
        out.md5 = md5.finishHex();
        out.sha1 = sha1.finishHex();
        out.size = done;
        return true;
    } catch (...) {
        out = Digests{};
        return false;
    }
}

}  // namespace rrm::fmt
