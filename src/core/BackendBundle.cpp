#include "core/BackendBundle.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

#include "backends/Backend.h"
#include "core/Catalog.h"
#include "core/Json.h"
#include "core/Library.h"
#include "core/Net.h"
#include "core/Platform.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz/miniz.h"

namespace rrm {
namespace {
bool relativePath(const std::string& text) {
    if (text.empty() || text.front() == '/' || text.find('\\') != std::string::npos ||
        text.find(':') != std::string::npos || text.find('\0') != std::string::npos) return false;
    const fs::path p = utf8ToPath(text);
    if (p.is_absolute() || p.has_root_name()) return false;
    for (const auto& part : p) {
        if (part == "..") return false;
#if defined(_WIN32)
        const std::string component = pathToUtf8(part);
        // Win32 strips trailing spaces/dots; reject paths that would acquire a
        // different meaning (including '.. ') when opened by the filesystem.
        if (part != "." && !component.empty() && (component.back() == ' ' || component.back() == '.')) return false;
#endif
    }
    return p.lexically_normal() != ".";
}

bool contained(const fs::path& root, const fs::path& target) {
    const fs::path rel = target.lexically_normal().lexically_relative(root.lexically_normal());
    if (rel.empty() || rel.is_absolute()) return false;
    for (const auto& part : rel) if (part == "..") return false;
    return true;
}

bool safeId(const std::string& id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

bool shaValue(const std::string& sha) {
    return sha.size() == 64 && std::all_of(sha.begin(), sha.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

struct Zip {
    mz_zip_archive reader{};
    FILE* file = nullptr;
    bool initialized = false;
    explicit Zip(const fs::path& p) {
#if defined(_WIN32)
        file = _wfopen(p.c_str(), L"rb");
#else
        file = std::fopen(p.c_str(), "rb");
#endif
        if (file) initialized = mz_zip_reader_init_cfile(&reader, file, 0, 0) != 0;
    }
    ~Zip() {
        if (initialized) mz_zip_reader_end(&reader);
        if (file) std::fclose(file);
    }
};

struct ZipEntry {
    mz_uint index;
    fs::path path;
    unsigned mode;
    bool directory, symlink;
    std::string link;
    uint64_t size;
};

bool resolveZipPath(const std::vector<ZipEntry>& entries, const fs::path& path, fs::path& resolved) {
    std::map<fs::path, std::string> links;
    for (const auto& e : entries) if (e.symlink) links[e.path] = e.link;
    std::deque<fs::path> todo;
    for (const auto& p : path) todo.push_back(p);
    resolved.clear();
    unsigned followed = 0;
    while (!todo.empty()) {
        const fs::path component = todo.front(); todo.pop_front();
        if (component == "." || component.empty()) continue;
        if (component == "..") {
            if (resolved.empty()) return false;
            resolved = resolved.parent_path();
            continue;
        }
        resolved /= component;
        auto link = links.find(resolved);
        if (link != links.end()) {
            if (++followed > 128) return false;
            resolved = resolved.parent_path();
            std::vector<fs::path> parts;
            for (const auto& p : utf8ToPath(link->second)) parts.push_back(p);
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) todo.push_front(*it);
        }
    }
    return true;
}

// Validate the entire directory before writing anything. Links are created last,
// and an archive cannot write a file through one of its own links.
bool inspectZip(Zip& zip, std::vector<ZipEntry>& entries, uint64_t& bytes, std::string& error) {
    if (!zip.initialized) { error = "Cannot open the backend ZIP archive."; return false; }
    const mz_uint count = mz_zip_reader_get_num_files(&zip.reader);
    if (count == 0 || count > 200000) { error = "Invalid number of files in the backend ZIP."; return false; }
    std::map<fs::path, bool> kinds;
    std::set<std::string> folded;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip.reader, i, &st) || !st.m_is_supported || st.m_is_encrypted) {
            error = "The backend ZIP contains an unreadable or encrypted entry."; return false;
        }
        const mz_uint len = mz_zip_reader_get_filename(&zip.reader, i, nullptr, 0);
        if (len < 2 || len > 32768) { error = "Invalid ZIP filename."; return false; }
        std::string name(len, '\0');
        mz_zip_reader_get_filename(&zip.reader, i, name.data(), len);
        name.pop_back();
        if (!relativePath(name)) { error = "Unsafe ZIP path: " + name; return false; }
        fs::path p = utf8ToPath(name).lexically_normal();
        if (p.filename().empty()) p = p.parent_path();
        std::string key = pathToUtf8(p);
#if defined(_WIN32) || defined(__APPLE__)
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        if (!folded.insert(key).second) { error = "Duplicate ZIP path: " + name; return false; }
        const unsigned mode = st.m_external_attr >> 16;
        const unsigned type = mode & 0170000;
        const bool link = type == 0120000;
        if (type != 0 && type != 0100000 && type != 0040000 && !link) {
            error = "Unsupported special file in ZIP: " + name; return false;
        }
        ZipEntry e{i, p, mode, st.m_is_directory != 0, link, {}, st.m_uncomp_size};
        if (link) {
            if (st.m_uncomp_size == 0 || st.m_uncomp_size > 32768) { error = "Invalid symbolic link: " + name; return false; }
            e.link.resize(static_cast<size_t>(st.m_uncomp_size));
            if (!mz_zip_reader_extract_to_mem(&zip.reader, i, e.link.data(), e.link.size(), 0) ||
                e.link.find('\0') != std::string::npos || e.link.find('\\') != std::string::npos ||
                e.link.find(':') != std::string::npos || utf8ToPath(e.link).is_absolute() ||
                !contained(fs::path("bundle"), fs::path("bundle") / p.parent_path() / utf8ToPath(e.link))) {
                error = "Unsafe symbolic link in ZIP: " + name; return false;
            }
        }
        if (st.m_uncomp_size > (32ull << 30) || bytes > (32ull << 30) - st.m_uncomp_size) {
            error = "The backend ZIP exceeds the 32 GiB extraction limit."; return false;
        }
        bytes += st.m_uncomp_size;
        kinds[p] = e.directory && !link;
        entries.push_back(std::move(e));
    }
    for (const auto& e : entries) {
        for (fs::path p = e.path.parent_path(); !p.empty(); p = p.parent_path()) {
            auto it = kinds.find(p);
            if (it != kinds.end() && !it->second) { error = "ZIP path traverses a file or link: " + pathToUtf8(e.path); return false; }
        }
        fs::path target;
        if (e.symlink && !resolveZipPath(entries, e.path, target)) {
            error = "ZIP symbolic link escapes the payload or has a cycle: " + pathToUtf8(e.path); return false;
        }
    }
    return true;
}

struct Output {
    std::ofstream* file = nullptr;
    TaskProgress* progress;
};

size_t writeZip(void* opaque, mz_uint64, const void* buffer, size_t size) {
    auto& out = *static_cast<Output*>(opaque);
    if (out.progress->cancel) return 0;
    if (out.file) {
        out.file->write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
        if (!*out.file) return 0;
    }
    out.progress->done += size;
    return size;
}

bool unpackZip(Zip& zip, const std::vector<ZipEntry>& entries, const fs::path& dest,
               TaskProgress& progress, bool verifyOnly, std::string& error) {
    std::error_code ec;
    for (const auto& e : entries) {
        if (progress.cancel) { error = "Cancelled"; return false; }
        if (e.symlink) continue;
        const fs::path file = dest / e.path;
        if (!verifyOnly) {
            fs::create_directories(e.directory ? file : file.parent_path(), ec);
            if (ec) { error = "Cannot create " + pathToUtf8(file) + ": " + ec.message(); return false; }
        }
        if (e.directory) continue;
        std::ofstream stream;
        if (!verifyOnly) {
            stream.open(file, std::ios::binary | std::ios::trunc);
            if (!stream) { error = "Cannot write " + pathToUtf8(file) + ". Check free space and permissions."; return false; }
        }
        Output output{verifyOnly ? nullptr : &stream, &progress};
        if (!mz_zip_reader_extract_to_callback(&zip.reader, e.index, writeZip, &output, 0)) {
            error = progress.cancel ? "Cancelled" : "Could not extract/verify " + pathToUtf8(e.path) + ". The ZIP may be damaged or the disk full.";
            return false;
        }
        if (!verifyOnly) {
            stream.close();
            if (!stream) { error = "Failed to finish writing " + pathToUtf8(file); return false; }
#if !defined(_WIN32)
            fs::permissions(file, static_cast<fs::perms>((e.mode & 0777) | 0600), ec);
            if (ec) { error = "Cannot set permissions for " + pathToUtf8(file) + ": " + ec.message(); return false; }
#endif
        }
    }
    if (!verifyOnly) for (const auto& e : entries) if (e.symlink) {
        fs::create_directories((dest / e.path).parent_path(), ec);
        if (!ec) fs::create_symlink(utf8ToPath(e.link), dest / e.path, ec);
        if (ec) { error = "Cannot create symbolic link " + pathToUtf8(e.path) + ": " + ec.message(); return false; }
    }
    return true;
}

bool validExecutable(const fs::path& root, const fs::path& relative, const BackendDef& def, std::string& error) {
    std::error_code ec;
    const fs::path exe = fs::weakly_canonical(root / relative, ec);
    const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
    if (ec || !contained(canonicalRoot, exe) || !fs::is_regular_file(exe, ec)) {
        error = "Bundled executable is missing or leaves its install folder: " + pathToUtf8(relative); return false;
    }
    if (const BackendSource* src = def.sourceFor(currentPlatformKey()); src && !src->accepts(pathToUtf8(exe))) {
        error = "Bundled " + def.name + " does not carry the required build marker " + src->marker; return false;
    }
#if !defined(_WIN32)
    fs::permissions(exe, fs::perms::owner_exec, fs::perm_options::add, ec);
    if (ec) { error = "Cannot make " + pathToUtf8(relative) + " executable: " + ec.message(); return false; }
#endif
    return true;
}

bool installSystemFiles(const json& entry, const fs::path& root, std::string& error) {
    if (!entry.contains("system_files")) return true;
    std::error_code ec;
    const fs::path system = fs::weakly_canonical(paths().system, ec);
    for (const auto& mapping : entry["system_files"]) {
        const fs::path source = root / utf8ToPath(mapping["source"].get<std::string>());
        const fs::path destination = system / utf8ToPath(mapping["destination"].get<std::string>());
        auto copy = [&](const fs::path& from, const fs::path& to) {
            if (fs::is_symlink(from, ec)) { error = "System assets must not be symbolic links: " + pathToUtf8(from); return false; }
            const fs::path actual = fs::weakly_canonical(to, ec);
            if (ec || !contained(system, actual)) { error = "System asset destination leaves the data folder: " + pathToUtf8(to); return false; }
            if (fs::is_directory(from, ec)) fs::create_directories(to, ec);
            else if (fs::is_regular_file(from, ec)) {
                fs::create_directories(to.parent_path(), ec);
                if (!ec) fs::copy_file(from, to, fs::copy_options::skip_existing, ec);
            } else { error = "Bundled system asset missing: " + pathToUtf8(from); return false; }
            if (ec) { error = "Cannot install system asset " + pathToUtf8(to) + ": " + ec.message(); return false; }
            return true;
        };
        if (!copy(source, destination)) return false;
        if (fs::is_directory(source, ec)) {
            for (fs::recursive_directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
                if (!copy(it->path(), destination / it->path().lexically_relative(source))) return false;
            }
            if (ec) { error = "Cannot read bundled system assets: " + ec.message(); return false; }
        }
    }
    return true;
}
}  // namespace

fs::path backendBundleDirectory() {
    if (const char* env = std::getenv("RRM_BUNDLE_DIR"); env && *env) return utf8ToPath(env);
    return paths().resources.empty() ? fs::path() : paths().resources / "bundle";
}

BackendBundleReport importBackendBundle(const Catalog& catalog, Library& library, TaskProgress& progress,
                                       bool verifyOnly, const fs::path& directory) {
    BackendBundleReport report;
    const fs::path dir = directory.empty() ? backendBundleDirectory() : directory;
    std::error_code ec;
    const fs::path manifest = dir / "manifest.json";
    const bool explicitDir = !directory.empty() || (std::getenv("RRM_BUNDLE_DIR") && *std::getenv("RRM_BUNDLE_DIR"));
    if (dir.empty() || !fs::exists(manifest, ec)) {
        if (explicitDir) { report.errors.push_back("Backend bundle manifest not found: " + pathToUtf8(manifest)); progress.fail(report.errors.back()); }
        return report;
    }
    report.found = true;
    progress.running = true;
    struct Finish { TaskProgress& p; ~Finish() { p.running = false; } } finish{progress};
    auto fail = [&](const std::string& error) { report.errors.push_back(error); progress.addLog("error: " + error); };
    json document;
    std::string error;
    if (!readJsonFile(manifest, document, &error) || jget(document, "schema", 0) != 1 ||
        jget<std::string>(document, "platform", "") != currentPlatformKey() ||
        !document.contains("entries") || !document["entries"].is_array()) {
        fail("Invalid backend bundle manifest for " + currentPlatformKey() + ": " + pathToUtf8(manifest) +
             (error.empty() ? " (schema, platform or entries do not match)." : ": " + error));
    } else {
        std::set<std::string> seen;
        for (const auto& entry : document["entries"]) {
            if (progress.cancel) { fail("Backend setup cancelled; it will resume on the next start."); break; }
            const std::string id = jget<std::string>(entry, "id", "");
            const std::string version = jget<std::string>(entry, "version", "");
            const std::string archive = jget<std::string>(entry, "archive", "");
            const std::string sha = jget<std::string>(entry, "sha256", "");
            const std::string executable = jget<std::string>(entry, "executable", "");
            const BackendDef* def = catalog.backend(id);
            if (!safeId(id) || !seen.insert(id).second || !def || version.empty() || !shaValue(sha) ||
                !relativePath(archive) || !relativePath(executable) || utf8ToPath(archive).extension() != ".zip" ||
                (def->kind != "libretro" && !def->sourceFor(currentPlatformKey()))) {
                fail("Invalid backend bundle entry: " + (id.empty() ? std::string("unnamed backend") : id)); continue;
            }
            bool systemValid = true;
            if (entry.contains("system_files")) {
                systemValid = entry["system_files"].is_array();
                if (systemValid) for (const auto& mapping : entry["system_files"]) {
                    if (!relativePath(jget<std::string>(mapping, "source", "")) ||
                        !relativePath(jget<std::string>(mapping, "destination", ""))) systemValid = false;
                }
            }
            if (!systemValid) { fail(def->name + ": invalid system asset paths in manifest."); continue; }
            if (!verifyOnly) if (const BackendInstall* old = library.install(id)) {
                if (old->userProvided || (!old->executable.empty() && fs::is_regular_file(utf8ToPath(old->executable), ec))) {
                    report.skipped.push_back(id); progress.addLog("Keeping existing " + def->name); continue;
                }
            }
            fs::path archivePath = fs::weakly_canonical(dir / utf8ToPath(archive), ec);
            fs::path bundleRoot = fs::weakly_canonical(dir, ec);
            if (ec || !contained(bundleRoot, archivePath) || !fs::is_regular_file(archivePath, ec)) {
                fail(def->name + ": archive is missing or outside the bundle: " + archive); continue;
            }
            progress.setStage("Verifying bundled " + def->name);
            if (sha256File(pathToUtf8(archivePath)) != sha) {
                fail(def->name + ": checksum mismatch for " + archive + ". Download and extract the complete release again."); continue;
            }
            Zip zip(archivePath);
            std::vector<ZipEntry> entries;
            uint64_t bytes = 0;
            error.clear();
            if (!inspectZip(zip, entries, bytes, error)) { fail(def->name + ": " + error); continue; }
            const fs::path exeRelative = utf8ToPath(executable).lexically_normal();
            fs::path resolvedExe;
            if (!resolveZipPath(entries, exeRelative, resolvedExe) ||
                std::none_of(entries.begin(), entries.end(), [&](const ZipEntry& e) { return e.path == resolvedExe && !e.directory && !e.symlink; })) {
                fail(def->name + ": executable is absent from archive: " + executable); continue;
            }
            if (const BackendSource* src = def->sourceFor(currentPlatformKey()); src && !src->marker.empty()) {
                fs::path marker;
                if (!resolveZipPath(entries, exeRelative.parent_path() / utf8ToPath(src->marker), marker) ||
                    std::none_of(entries.begin(), entries.end(), [&](const ZipEntry& e) { return e.path == marker && !e.directory; })) {
                    fail(def->name + ": required build marker is absent from archive: " + src->marker); continue;
                }
            }
            if (entry.contains("system_files")) for (const auto& mapping : entry["system_files"]) {
                const fs::path source = utf8ToPath(mapping["source"].get<std::string>()).lexically_normal();
                bool exists = false;
                for (const auto& e : entries) if (e.path == source || contained(source, e.path)) {
                    exists = true;
                    if (e.symlink) systemValid = false;
                }
                if (!exists) systemValid = false;
            }
            if (!systemValid) { fail(def->name + ": system assets are missing or contain symbolic links."); continue; }
            progress.done = 0; progress.total = bytes;
            if (verifyOnly) {
                if (!unpackZip(zip, entries, {}, progress, true, error)) fail(def->name + ": " + error);
                else progress.addLog("Verified " + def->name + " " + version);
                continue;
            }
            fs::path parent = paths().backends / utf8ToPath(id);
            fs::create_directories(parent, ec);
            if (ec) { fail(def->name + ": cannot create writable backend directory: " + ec.message()); continue; }
            const fs::path ownedRoot = fs::weakly_canonical(paths().backends, ec);
            const fs::path ownedParent = fs::weakly_canonical(parent, ec);
            if (ec || !contained(ownedRoot, ownedParent)) {
                fail(def->name + ": backend directory redirects outside Marathon's data folder."); continue;
            }
            const fs::path dest = parent / ("bundle-" + sha.substr(0, 16));
            if (fs::is_symlink(dest, ec)) { fail(def->name + ": refusing to replace a linked backend directory."); continue; }
            if (!fs::exists(dest, ec)) {
                const auto space = fs::space(parent, ec);
                if (!ec && space.available < bytes + (16ull << 20)) {
                    fail(def->name + ": not enough free space in " + pathToUtf8(paths().data) + ". Needs " + std::to_string((bytes >> 20) + 16) + " MiB."); continue;
                }
                const fs::path stage = parent / (".bundle-stage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                fs::create_directory(stage, ec);
                if (ec) { fail(def->name + ": cannot create staging directory: " + ec.message()); continue; }
                progress.setStage("Setting up bundled " + def->name);
                if (!unpackZip(zip, entries, stage, progress, false, error) || !validExecutable(stage, exeRelative, *def, error)) {
                    fs::remove_all(stage, ec); fail(def->name + ": " + error); continue;
                }
                fs::rename(stage, dest, ec);
                if (ec) {
                    const std::string reason = ec.message(); fs::remove_all(stage, ec);
                    fail(def->name + ": cannot finish installation: " + reason); continue;
                }
            }
            if (!validExecutable(dest, exeRelative, *def, error)) { fail(def->name + ": " + error); continue; }
            if (!installSystemFiles(entry, dest, error)) { fail(def->name + ": " + error); continue; }
            BackendInstall install;
            install.backendId = id; install.version = version;
            install.root = pathToUtf8(dest); install.executable = pathToUtf8(dest / exeRelative);
            library.setInstall(install);
            report.imported.push_back(id);
            progress.addLog(def->name + " " + version + " ready");
        }
    }
    if (!report.ok()) {
        std::string message = "Some bundled backends could not be set up:\n";
        for (const auto& e : report.errors) message += "\n" + e;
        message += "\n\nYour existing installations were kept. Check free disk space, or extract the complete release again. Setup retries next time.";
        progress.fail(message);
    } else progress.setStage(verifyOnly ? "Backend bundle verified" : "Bundled backends ready");
    return report;
}
}  // namespace rrm
