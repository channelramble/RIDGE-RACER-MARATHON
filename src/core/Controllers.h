// Gamepad logic shared by the app, the in-app cores and the adapters: player slots, stick
// dead zones, pad families and the quit chord. Pure functions, no SDL calls (see Gamepads.h).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/Settings.h"

namespace rrm {

struct ConnectedPad {
    uint32_t id = 0;              // SDL_JoystickID (0 in hand-made test data)
    std::string name;             // SDL_GetGamepadNameForID
    std::string guid;             // SDL_GUIDToString(SDL_GetGamepadGUIDForID): 32 lowercase hex
    std::string type;             // SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID) or "standard"
    uint16_t vendor = 0, product = 0;
    int nameIndex = 1;            // 1-based among connected pads with the same name, connection order ("<name> <n>")
    int player = -1;              // 0-based player slot after assignPlayers, -1 = none
    bool hasGuide = true;         // SDL_GamepadHasButton(GUIDE)
    bool hasRumble = false;       // SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN
    bool virtualPad = false;      // SDL_IsJoystickVirtual (tests)
};

// Player slots -> joystick id (0 = nobody). Rules, in order:
//  1. A slot that names a GUID takes the first connected pad with that GUID no earlier slot took
//     (if the slot's current holder is another pad, that pad loses the slot and is placed by rule 3).
//  2. Other slots keep the pad they hold in `current` while it is connected.
//  3. Empty slots that are not "none" are filled in slot order with unassigned pads in connection order.
// Holders never move because another pad left; a returning pad fills the lowest empty slot.
std::array<uint32_t, 4> assignPlayers(const std::vector<ConnectedPad>& pads,
                                      const std::array<PlayerChoice, 4>& choices,
                                      const std::array<uint32_t, 4>& current = {});
// Sets ConnectedPad::player from an assignment.
void applyAssignment(std::vector<ConnectedPad>& pads, const std::array<uint32_t, 4>& slots);

int deadzoneUnits(int percent);                                  // percent -> 0..32767
// Scaled radial dead zone: inside -> 0; outside, rescaled so the edge still reaches full deflection.
void applyStickDeadzone(int16_t& x, int16_t& y, int deadzoneUnits);

std::string padFamily(const std::string& type);                  // "xbox" | "playstation" | "nintendo" | "generic"
std::string padTypeLabel(const std::string& type);               // "Xbox 360", "Xbox One / Series", "DualShock 3",
                                                                 // "DualShock 4", "DualSense", "Switch Pro", "Joy-Con pair", "Controller"

// ---- quit chord --------------------------------------------------------------------------------

enum class QuitChordMode { GuideStart, BackStart, Off };
QuitChordMode quitChordMode(const std::string& value);           // "guide-start" | "back-start" | "off"

struct ChordButtons {
    bool guide = false, back = false, start = false, hasGuide = true;
};

// Guide + Start (Back + Start on pads without a Guide button), or Back + Start, held for a while.
class QuitChordDetector {
public:
    void configure(QuitChordMode mode, int holdMs);
    // Call for every connected pad each frame; true exactly once per continuous hold of holdMs.
    bool update(uint32_t padId, const ChordButtons& b, uint64_t nowMs);
    void forget(uint32_t padId);
    void reset();                         // pads holding the chord now must release it before it can fire again
    float progress(uint64_t nowMs) const; // 0..1 for the pad closest to firing

private:
    struct Hold {
        bool down = false;     // the chord is held
        bool blocked = false;  // fired (or reset) during this hold: wait for a release
        uint64_t since = 0;
    };
    bool chordDown(const ChordButtons& b) const;

    QuitChordMode mode_ = QuitChordMode::GuideStart;
    int holdMs_ = 1000;
    std::map<uint32_t, Hold> holds_;
};

}  // namespace rrm
