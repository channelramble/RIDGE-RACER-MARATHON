// TEST-ONLY stand-ins for backend makers implemented by other modules.
// The private recomp test project links only the recomp/native-port adapters;
// createBackend() still references every maker, so the others return nullptr.
// Never add this file to the main build.
#include "backends/Adapters.h"

namespace rrm {

std::unique_ptr<Backend> makePcsx2(const BackendDef&) { return nullptr; }
std::unique_ptr<Backend> makeRpcs3(const BackendDef&) { return nullptr; }
std::unique_ptr<Backend> makeXenia(const BackendDef&) { return nullptr; }
std::unique_ptr<Backend> makePpssppStandalone(const BackendDef&) { return nullptr; }
std::unique_ptr<Backend> makeMameStandalone(const BackendDef&) { return nullptr; }
std::unique_ptr<Backend> makeLibretro(const BackendDef&) { return nullptr; }
std::unique_ptr<Backend> makeAndroidApp(const BackendDef&) { return nullptr; }

}  // namespace rrm
