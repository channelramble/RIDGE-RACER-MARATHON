#include "core/Net.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <thread>
#include <utility>

#include "backends/Backend.h"
#include "core/Platform.h"
#include "core/Process.h"

namespace rrm {

namespace {

const char* kUserAgent = "RidgeRacerMarathon/" RRM_VERSION;

std::string curlPath() {
#if defined(_WIN32)
    // Windows 10 1803+ ships curl.exe in System32.
    std::string found = findOnPath("curl.exe");
    if (!found.empty()) return found;
    if (const char* root = std::getenv("SystemRoot")) return std::string(root) + "\\System32\\curl.exe";
    return "curl.exe";
#elif defined(__APPLE__)
    return "/usr/bin/curl";
#else
    std::string found = findOnPath("curl");
    return found.empty() ? "/usr/bin/curl" : found;
#endif
}

std::vector<std::string> authHeaders(const std::string& url) {
    std::vector<std::string> args;
    if (url.rfind("https://api.github.com/", 0) == 0) {
        args.insert(args.end(), {"-H", "Accept: application/vnd.github+json"});
        const char* token = std::getenv("GITHUB_TOKEN");
        if (token && *token) args.insert(args.end(), {"-H", std::string("Authorization: Bearer ") + token});
    }
    return args;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool run(const std::vector<std::string>& argv, std::string& error, const std::string& cwd = {}) {
    ProcessSpec spec;
    spec.argv = argv;
    spec.workingDir = cwd;
    std::string out;
    int code = runProcess(spec, &out);
    if (code != 0) {
        error = argv[0] + " failed (" + std::to_string(code) + "): " + out.substr(0, 600);
        return false;
    }
    return true;
}

}  // namespace

bool httpGet(const std::string& url, std::string& body, std::string& error) {
    ProcessSpec spec;
    spec.argv = {curlPath(), "-sSL", "--fail", "--retry", "2", "-A", kUserAgent};
    for (auto& a : authHeaders(url)) spec.argv.push_back(a);
    spec.argv.push_back(url);
    body.clear();
    int code = runProcess(spec, &body);
    if (code != 0) {
        error = "Download failed (" + std::to_string(code) + "): " + url;
        return false;
    }
    return true;
}

bool downloadFile(const std::string& url, const std::string& dest, TaskProgress* progress, std::string& error) {
    std::error_code ec;
    fs::path destPath = utf8ToPath(dest);
    fs::create_directories(destPath.parent_path(), ec);
    fs::path part = destPath;
    part += ".part";
    fs::remove(part, ec);

    ProcessSpec spec;
    spec.argv = {curlPath(), "-sSL", "--fail", "--retry", "3", "--connect-timeout", "20", "-A", kUserAgent};
    for (auto& a : authHeaders(url)) spec.argv.push_back(a);
    spec.argv.insert(spec.argv.end(), {"-o", pathToUtf8(part), url});
    std::string output;
    auto keepWaiting = [&]() {
        if (progress) {
            std::error_code e2;
            auto size = fs::exists(part, e2) ? fs::file_size(part, e2) : 0;
            if (!e2) progress->done = size;
            return !progress->cancel.load();
        }
        return true;
    };
    int code = runProcess(spec, &output, keepWaiting);
    if (progress && progress->cancel) {
        fs::remove(part, ec);
        error = "Cancelled";
        return false;
    }
    if (code != 0) {
        fs::remove(part, ec);
        error = "Download failed (curl " + std::to_string(code) + ") for " + url + (output.empty() ? "" : ": " + output.substr(0, 300));
        return false;
    }
    fs::remove(destPath, ec);
    fs::rename(part, destPath, ec);
    if (ec) {
        error = "Could not save " + dest + ": " + ec.message();
        return false;
    }
    if (progress) progress->done = fs::file_size(destPath, ec);
    return true;
}

bool globMatch(const std::string& pattern, const std::string& text) {
    // Iterative wildcard match; '*' also matches '/', comparison is case-insensitive.
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    auto eq = [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || eq(pattern[p], text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string findInTree(const std::string& root, const std::string& pattern, int maxDepth,
                       const std::function<bool(const std::string&)>& accept) {
    std::error_code ec;
    fs::path base = utf8ToPath(root);
    std::string best;
    size_t bestDepth = SIZE_MAX;
    auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > maxDepth) {
            it.disable_recursion_pending();
            continue;
        }
        std::string rel = pathToUtf8(fs::relative(it->path(), base, ec));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (!it->is_directory(ec) || endsWith(lower(rel), ".app")) {
            if (globMatch(pattern, rel) && (size_t)it.depth() < bestDepth) {
                std::string found = pathToUtf8(it->path());
                if (accept && !accept(found)) continue;
                best = std::move(found);
                bestDepth = (size_t)it.depth();
            }
        }
    }
    return best;
}

bool githubLatestAsset(const std::string& repo, const std::string& pattern, bool includePrerelease, GithubAsset& out,
                       std::string& error, const std::string& tag) {
    std::string body;
    std::string url = "https://api.github.com/repos/" + repo + "/releases?per_page=30";
    if (!httpGet(url, body, error)) return false;
    json releases;
    try {
        releases = json::parse(body);
    } catch (const std::exception& e) {
        error = std::string("GitHub API returned invalid JSON: ") + e.what();
        return false;
    }
    if (!releases.is_array()) {
        error = "GitHub API error for " + repo + ": " + jget<std::string>(releases, "message", "unexpected response");
        return false;
    }
    for (auto& rel : releases) {
        if (jget(rel, "draft", false)) continue;
        std::string relTag = jget<std::string>(rel, "tag_name", "");
        if (!tag.empty() && relTag != tag) continue;
        if (tag.empty() && !includePrerelease && jget(rel, "prerelease", false)) continue;
        const json assets = rel.value("assets", json::array());
        for (auto& a : assets) {
            std::string name = jget<std::string>(a, "name", "");
            if (globMatch(pattern, name)) {
                out.name = name;
                out.url = jget<std::string>(a, "browser_download_url", "");
                out.size = jget<uint64_t>(a, "size", 0);
                out.tag = relTag;
                std::string digest = jget<std::string>(a, "digest", "");
                if (digest.rfind("sha256:", 0) == 0) out.sha256 = digest.substr(7);
                for (auto& c : assets) {
                    std::string cn = lower(jget<std::string>(c, "name", ""));
                    if (cn.find("sha256sums") != std::string::npos || cn == "checksums.txt")
                        out.checksumsUrl = jget<std::string>(c, "browser_download_url", "");
                }
                return true;
            }
        }
    }
    error = "No release asset matching \"" + pattern + "\" in " + repo + (includePrerelease ? "" : " (stable releases)");
    return false;
}

namespace {
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t len = 0;
    uint8_t buf[64];
    size_t used = 0;
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            size_t take = std::min(n, 64 - used);
            std::memcpy(buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    std::string finish() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (used != 56) update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = (uint8_t)(bits >> (56 - i * 8));
        update(lenBytes, 8);
        char hex[65];
        for (int i = 0; i < 8; ++i) snprintf(hex + i * 8, 9, "%08x", h[i]);
        return std::string(hex, 64);
    }
};
}  // namespace

std::string sha256File(const std::string& path) {
    FILE* f = nullptr;
#if defined(_WIN32)
    f = _wfopen(utf8ToPath(path).c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return {};
    Sha256 sha;
    std::vector<uint8_t> chunk(1 << 20);
    size_t n;
    while ((n = std::fread(chunk.data(), 1, chunk.size(), f)) > 0) sha.update(chunk.data(), n);
    bool err = std::ferror(f) != 0;
    std::fclose(f);
    return err ? std::string() : sha.finish();
}

std::string checksumFor(const std::string& sumsText, const std::string& fileName) {
    size_t pos = 0;
    while (pos < sumsText.size()) {
        size_t end = sumsText.find('\n', pos);
        if (end == std::string::npos) end = sumsText.size();
        std::string line = sumsText.substr(pos, end - pos);
        pos = end + 1;
        if (line.size() < 66) continue;
        std::string hex = lower(line.substr(0, 64));
        std::string name = line.substr(64);
        while (!name.empty() && (name.front() == ' ' || name.front() == '*' || name.front() == '\t')) name.erase(0, 1);
        while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
        if (name.rfind("./", 0) == 0) name.erase(0, 2);
        if (name == fileName) return hex;
    }
    return {};
}

bool extractArchive(const std::string& archive, const std::string& destDir, std::string& error, TaskProgress* progress) {
    std::error_code ec;
    fs::create_directories(utf8ToPath(destDir), ec);
    std::string name = lower(archive);
    if (progress) progress->addLog("extract " + archive);
#if defined(_WIN32)
    std::string tar = findOnPath("tar.exe");
    if (endsWith(name, ".7z")) {
        std::string sevenZip = findOnPath("7z.exe");
        if (sevenZip.empty()) {
            // 7zr.exe is the official standalone extractor (LGPL) from 7-zip.org.
            fs::path tool = paths().cache / "7zr.exe";
            if (!fs::exists(tool, ec) && !downloadFile("https://www.7-zip.org/a/7zr.exe", pathToUtf8(tool), nullptr, error))
                return false;
            sevenZip = pathToUtf8(tool);
        }
        return run({sevenZip, "x", "-y", "-o" + destDir, archive}, error);
    }
    if (tar.empty()) {
        error = "tar.exe not found (Windows 10 1803 or newer is required)";
        return false;
    }
    return run({tar, "-xf", archive, "-C", destDir}, error);
#else
#if defined(__APPLE__)
    if (endsWith(name, ".zip")) return run({"/usr/bin/ditto", "-x", "-k", archive, destDir}, error);
    if (endsWith(name, ".dmg")) {
        fs::path mount = paths().cache / ("mnt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(mount, ec);
        if (!run({"/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", pathToUtf8(mount), archive},
                 error))
            return false;
        bool ok = true;
        for (auto& e : fs::directory_iterator(mount, ec)) {
            if (e.is_symlink(ec)) continue;  // skip the /Applications alias
            ok = ok && run({"/usr/bin/ditto", pathToUtf8(e.path()), destDir + "/" + pathToUtf8(e.path().filename())}, error);
        }
        std::string ignore;
        run({"/usr/bin/hdiutil", "detach", pathToUtf8(mount), "-force"}, ignore);
        fs::remove(mount, ec);
        return ok;
    }
    // bsdtar (libarchive) handles tar.*, 7z and zip.
    return run({"/usr/bin/tar", "-xf", archive, "-C", destDir}, error);
#else
    if (endsWith(name, ".zip")) {
        std::string unzip = findOnPath("unzip");
        if (!unzip.empty()) return run({unzip, "-q", "-o", archive, "-d", destDir}, error);
        std::string bsdtar = findOnPath("bsdtar");
        if (!bsdtar.empty()) return run({bsdtar, "-xf", archive, "-C", destDir}, error);
        std::string py = findOnPath("python3");
        if (!py.empty()) return run({py, "-m", "zipfile", "-e", archive, destDir}, error);
        error = "Install 'unzip' to extract " + archive;
        return false;
    }
    if (endsWith(name, ".7z")) {
        for (const char* tool : {"7z", "7za", "7zz"}) {
            std::string t = findOnPath(tool);
            if (!t.empty()) return run({t, "x", "-y", "-o" + destDir, archive}, error);
        }
        std::string bsdtar = findOnPath("bsdtar");
        if (!bsdtar.empty()) return run({bsdtar, "-xf", archive, "-C", destDir}, error);
        error = "Install p7zip (7z) to extract " + archive;
        return false;
    }
    std::string tarTool = findOnPath("tar");
    if (tarTool.empty()) {
        error = "tar not found";
        return false;
    }
    return run({tarTool, "-xf", archive, "-C", destDir}, error);
#endif
#endif
}

}  // namespace rrm
