// Controller configuration for the external backends (docs/CONTROLLERS.md): pure generators
// that turn Marathon's controller settings (LaunchRequest::controllers and ::pads) into each
// program's own keys, file text or command-line flags. They read and write no files; the
// adapters do that, and tests/controller_configs_test.cpp checks the output line by line.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/ConfigFiles.h"
#include "core/Controllers.h"
#include "core/Settings.h"

namespace rrm::ctl {

using KeyValues = std::vector<std::pair<std::string, std::string>>;

// The one log line every adapter writes about controllers (tests look for it):
// "controllers: steering=stick pedals=triggers vibration=on deadzone=8% -> <what>".
std::string noteLine(const ControllerSettings& c, const std::string& what);
// The line for an install whose controller files belong to the user.
std::string linkedNote(const std::string& program);
// Dead zone as a fraction of the stick's travel: 8 -> "0.08", 0 -> "0" (decimalPoint: "0.0").
std::string fraction(int percent, bool decimalPoint = false);

// ---- RPCS3 (Ridge Racer 7) --------------------------------------------------------------------

// The whole input_configs/global/Default.yml. Player 1 and 2 are the pads holding those player
// slots (SDL handler, device "<SDL name> <n>"); without a pad Player 1 is the keyboard and Player
// 2 nobody. Players 3-6 are nobody. With `keyboardCopilot`, Player 7 is the keyboard as Player 1's
// copilot while Player 1 is a pad, so the keyboard keeps working next to it.
std::string rpcs3InputConfig(const ControllerSettings& c, const std::vector<ConnectedPad>& pads, bool keyboardCopilot);
// "Device" value RPCS3's SDL handler matches: "Xbox Series X Controller 1".
std::string rpcs3DeviceName(const ConnectedPad& pad);

// ---- PCSX2 (Ridge Racer V) --------------------------------------------------------------------

// Adds Marathon's SDL bindings to [Pad1] / [Pad2] of PCSX2.ini, keeping every binding that is not
// an SDL one (the keyboard defaults, other input sources), plus [InputSources] SDL and Back + Start
// for the pause menu. Running it again gives the same file.
void applyPcsx2Controllers(cfg::Ini& ini, const ControllerSettings& c);

// ---- PPSSPP standalone (Ridge Racer, Ridge Racer 2) ---------------------------------------------

inline constexpr int kPpssppControlsVersion = 1;
// The complete [ControlMapping] (keyboard included: PPSSPP drops the default of every key the
// section leaves out). `windows`: XInput (device 20) and DirectInput (10) codes, else SDL (10).
KeyValues ppssppControlMapping(const ControllerSettings& c, bool windows);
// "<steering>/<pedals>", kept in [Marathon] Profile.
std::string ppssppProfile(const ControllerSettings& c);
// True when Marathon's mapping is due: no [Marathon] marker, an older ControlsVersion or another
// profile. Otherwise the file keeps remaps the player made in PPSSPP since.
bool ppssppMappingDue(const cfg::Ini& ini, const ControllerSettings& c);
// Writes [ControlMapping] and the [Marathon] marker.
void applyPpssppMapping(cfg::Ini& ini, const ControllerSettings& c, bool windows);

// ---- MAME standalone (Rave Racer) ---------------------------------------------------------------

// ctrlr/marathon.cfg: Back + Start opens MAME's menu, Back + B quits, LB / RB and the d-pad shift
// (taken off the stick's up/down, so a diagonal stick no longer changes gear) for `setName`.
std::string mameControllerFile(const std::string& setName);
std::vector<std::string> mameControllerArgs(const ControllerSettings& c, const std::string& ctrlrDir);

// ---- Xenia Canary (Ridge Racer 6) ---------------------------------------------------------------

// xenia-canary.config.toml keys (dotted key -> TOML value) for cfg::tomlSet.
KeyValues xeniaHidKeys(const ControllerSettings& c);

// ---- PSXRecomp builds (Ridge Racer and R4 recomps) ------------------------------------------------

// settings.toml [controller] pN_device for one player: the chosen controller's GUID; "none" for
// Nobody (where `nobodyAllowed`); for Automatic "gamepad" (the first free controller) when the key is
// missing, when it still holds what Marathon wrote last time (`lastWritten`), or, with
// `keyboardToGamepad`, when it says "keyboard". Any other value the player chose in the game's own
// launcher stays (returned unchanged).
std::string psxDevice(const PlayerChoice& choice, const std::optional<std::string>& current,
                      const std::string& lastWritten, bool nobodyAllowed, bool keyboardToGamepad);
// The [controller] keys as TOML values. `r4`: also Player 2 and the analog/digital mode.
KeyValues psxControllerKeys(const ControllerSettings& c, bool r4, const std::optional<std::string>& currentP1,
                            const std::optional<std::string>& currentP2, const std::string& lastP1,
                            const std::string& lastP2);

// One mods/state.toml feature (PsxRecomp.cpp applies them).
struct ModSetting {
    std::string package, feature;
    bool enabled = false;
    KeyValues values;  // option id -> canonical text
};
// R4's "R4 Controls" mod: Modern scheme only with stick steering and trigger pedals.
std::vector<ModSetting> r4ControllerMods(const ControllerSettings& c);

// ---- Fukami (Ridge Racer V) ---------------------------------------------------------------------

struct IniKey {
    std::string section, key, value;
};
// [input] analog (only set with stick steering) and rumble.
std::vector<IniKey> fukamiInputKeys(const ControllerSettings& c);

// ---- Ridge Racer 6 recomp -----------------------------------------------------------------------

// --hid_mappings_file (when the package has a gamecontrollerdb.txt) and --rr6_log_input.
std::vector<std::string> rr6ControllerFlags(const std::string& mappingsFile, bool logInput);

// ---- Rage Racer PC -------------------------------------------------------------------------------

// --set values: "input.analog=true", "input.steering_deadzone=0.08".
std::vector<std::string> rageInputSets(const ControllerSettings& c);

// ---- Rave Racer native ---------------------------------------------------------------------------

// rr_controls.cfg keys (pad_deadzone in stick units).
KeyValues raveCfgKeys(const ControllerSettings& c);

}  // namespace rrm::ctl
