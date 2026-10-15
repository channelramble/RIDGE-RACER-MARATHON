// PlayStation metadata: SYSTEM.CNF boot file (PS1/PS2) and PARAM.SFO (PSP/PS3).
#include "core/formats/Formats.h"

#include <cctype>
#include <cstring>
#include <string_view>

namespace rrm::fmt {

namespace {

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// Trims whitespace and control characters (SYSTEM.CNF often ends with 0x1A).
std::string_view trim(std::string_view s) {
    while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ') s.remove_prefix(1);
    while (!s.empty() && static_cast<unsigned char>(s.back()) <= ' ') s.remove_suffix(1);
    return s;
}

std::string upper(std::string_view s) {
    std::string o(s);
    for (auto& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

}  // namespace

std::string bootSerialFromSystemCnf(const std::string& systemCnf) {
    try {
        std::string_view boot, boot2;
        std::string_view text(systemCnf);
        while (!text.empty()) {
            const size_t eol = text.find_first_of("\r\n");
            std::string_view line = text.substr(0, eol);
            text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
            const size_t eq = line.find('=');
            if (eq == std::string_view::npos) continue;
            const std::string key = upper(trim(line.substr(0, eq)));
            if (key == "BOOT2") boot2 = trim(line.substr(eq + 1));
            else if (key == "BOOT") boot = trim(line.substr(eq + 1));
        }
        // PS2 discs use BOOT2; a PS1 BOOT line is the fallback.
        std::string_view v = !boot2.empty() ? boot2 : boot;
        if (v.empty()) return {};
        // Drop the device ("cdrom:", "cdrom0:"), arguments, directories and ";1".
        if (const size_t colon = v.find(':'); colon != std::string_view::npos) v = trim(v.substr(colon + 1));
        size_t end = 0;
        while (end < v.size() && static_cast<unsigned char>(v[end]) > ' ') ++end;
        v = v.substr(0, end);
        if (const size_t slash = v.find_last_of("\\/"); slash != std::string_view::npos) v = v.substr(slash + 1);
        if (const size_t semi = v.find(';'); semi != std::string_view::npos) v = v.substr(0, semi);
        return std::string(trim(v));
    } catch (...) {
        return {};
    }
}

bool parseSfo(const std::string& data, std::vector<std::pair<std::string, std::string>>& out) {
    out.clear();
    try {
        const auto* p = reinterpret_cast<const uint8_t*>(data.data());
        const size_t n = data.size();
        if (n < 20 || std::memcmp(p, "\0PSF", 4) != 0) return false;
        const uint32_t keyStart = le32(p + 8);
        const uint32_t dataStart = le32(p + 12);
        const uint32_t count = le32(p + 16);
        if (keyStart > n || dataStart > n || count > 4096 || 20 + uint64_t(count) * 16 > n) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = p + 20 + size_t(i) * 16;
            const uint16_t keyOff = le16(e);
            const uint16_t fmt = le16(e + 2);
            const uint32_t len = le32(e + 4);
            const uint32_t dataOff = le32(e + 12);
            const uint64_t kpos = uint64_t(keyStart) + keyOff;
            if (kpos >= n) continue;
            const void* nul = std::memchr(p + kpos, 0, n - size_t(kpos));
            if (!nul) continue;
            std::string key(reinterpret_cast<const char*>(p + kpos), static_cast<const char*>(nul));
            const uint64_t vpos = uint64_t(dataStart) + dataOff;
            if (vpos > n || len > n - vpos) continue;
            std::string value;
            if (fmt == 0x0404) {  // int32
                if (len < 4) continue;
                value = std::to_string(le32(p + vpos));
            } else {  // 0x0004 utf8-S, 0x0204 utf8 (NUL terminated)
                value.assign(reinterpret_cast<const char*>(p + vpos), len);
                while (!value.empty() && value.back() == '\0') value.pop_back();
            }
            out.emplace_back(std::move(key), std::move(value));
        }
        return true;
    } catch (...) {
        out.clear();
        return false;
    }
}

std::string sfoValue(const std::vector<std::pair<std::string, std::string>>& kv, const std::string& key) {
    for (const auto& [k, v] : kv)
        if (k == key) return v;
    return {};
}

}  // namespace rrm::fmt
