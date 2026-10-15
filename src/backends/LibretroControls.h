// Controller settings for the in-app libretro cores: which emulated controller each game gets, how
// the pad's sticks and triggers reach it, and what the game page shows about it.
#pragma once

#include <string>

#include "backends/Backend.h"
#include "core/Json.h"
#include "core/Settings.h"

namespace rrm {

// Core options for one game: Marathon's rrm_* keys (documented in libretro/Host.cpp) plus the core's
// own input options. Libretro::launch() merges them before the game's advanced coreOptions, which
// still win. Every value is a string.
json libretroControllerOptions(const std::string& core, const std::string& gameId, const ControllerSettings& c);
// The game page's CONTROLS panel for the same game and settings.
ControlsInfo libretroControls(const std::string& core, const std::string& gameId, const ControllerSettings& c);

}  // namespace rrm
