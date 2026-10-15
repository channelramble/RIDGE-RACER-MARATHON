// Downloads and archive extraction for backend installs.
// Desktop: uses the system curl and tar/unzip/7z tools (present on macOS,
// Windows 10+ and mainstream Linux). Android/iOS ship their cores bundled.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/Json.h"

namespace rrm {

struct TaskProgress;

// GET with redirects to a file. Writes to dest + ".part" then renames.
bool downloadFile(const std::string& url, const std::string& dest, TaskProgress* progress,
                  std::string& error);
// GET into memory (small API responses). Adds a GitHub-friendly User-Agent.
bool httpGet(const std::string& url, std::string& body, std::string& error);

struct GithubAsset {
    std::string name, url, tag;
    uint64_t size = 0;
    std::string checksumsUrl;  // a SHA256SUMS-style asset in the same release, if any
    std::string sha256;        // GitHub's own "digest" for the asset ("sha256:<hex>"), if published
};
// Latest (or a specific tag's) release asset whose name matches a glob pattern
// such as "pcsx2-v*-macos-Qt.tar.xz". includePrerelease picks newest overall.
bool githubLatestAsset(const std::string& repo, const std::string& pattern, bool includePrerelease,
                       GithubAsset& out, std::string& error, const std::string& tag = "");

// Extracts .zip/.7z/.tar.*/.dmg (macOS) into destDir. Returns false with error.
bool extractArchive(const std::string& archive, const std::string& destDir, std::string& error,
                    TaskProgress* progress = nullptr);

bool globMatch(const std::string& pattern, const std::string& text);  // * and ? only

// SHA-256 of a file as lower-case hex ("" on I/O error).
std::string sha256File(const std::string& path);
// Looks up `fileName` in a "SHA256SUMS" text ("<hex>  <name>" per line). "" if absent.
std::string checksumFor(const std::string& sumsText, const std::string& fileName);
// Finds the first file under root whose relative path matches a glob (case-insensitive), the
// shallowest one that `accept` (if given) agrees to.
std::string findInTree(const std::string& root, const std::string& pattern, int maxDepth = 6,
                       const std::function<bool(const std::string&)>& accept = {});

}  // namespace rrm
