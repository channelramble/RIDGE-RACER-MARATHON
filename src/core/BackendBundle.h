// Verified, offline installation of the backend payloads shipped with a release.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rrm {
class Catalog;
class Library;
struct TaskProgress;

struct BackendBundleReport {
    bool found = false;
    std::vector<std::string> imported, skipped, errors;
    bool ok() const { return errors.empty(); }
};

// Resources/bundle on macOS, bundle beside the Windows exe, and
// share/ridge-racer-marathon/bundle on Linux. RRM_BUNDLE_DIR overrides discovery.
std::filesystem::path backendBundleDirectory();

// Run on a worker with a scratch Library, then merge imported install records on
// the main thread. Never saves the Library or changes existing user installations.
// Missing bundles are normal for a launcher-only/developer build. An explicitly
// supplied directory must contain manifest.json. Verification writes no payloads.
BackendBundleReport importBackendBundle(const Catalog& catalog, Library& library, TaskProgress& progress,
                                       bool verifyOnly = false, const std::filesystem::path& directory = {});
}  // namespace rrm
