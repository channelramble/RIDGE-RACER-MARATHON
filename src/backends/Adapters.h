// Constructors for every backend implementation, dispatched by the catalog "adapter" (default:
// the id) in createBackend() (Backend.cpp). Each lives in its own .cpp under src/backends/.
#pragma once

#include <memory>

#include "backends/Backend.h"

namespace rrm {

// Native recompilations / source ports (desktop).
std::unique_ptr<Backend> makePsxRecomp(const BackendDef& def);      // rr1-recomp, r4-recomp
std::unique_ptr<Backend> makeRageRacerPc(const BackendDef& def);    // rage-racer-pc
std::unique_ptr<Backend> makeRaveRacerNative(const BackendDef& def); // rave-racer-native
std::unique_ptr<Backend> makeFukami(const BackendDef& def);         // fukami (Ridge Racer V)
std::unique_ptr<Backend> makeRr6Recomp(const BackendDef& def);      // rr6-recomp

// External emulators (desktop).
std::unique_ptr<Backend> makePcsx2(const BackendDef& def);
std::unique_ptr<Backend> makeRpcs3(const BackendDef& def);
std::unique_ptr<Backend> makeXenia(const BackendDef& def);
std::unique_ptr<Backend> makePpssppStandalone(const BackendDef& def);
std::unique_ptr<Backend> makeMameStandalone(const BackendDef& def);

// In-process libretro cores (all platforms) and Android hand-off.
std::unique_ptr<Backend> makeLibretro(const BackendDef& def);       // kind == "libretro"
std::unique_ptr<Backend> makeAndroidApp(const BackendDef& def);     // kind == "android-app"

}  // namespace rrm
