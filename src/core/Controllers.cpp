#include "core/Controllers.h"

#include <algorithm>
#include <cmath>

namespace rrm {

std::array<uint32_t, 4> assignPlayers(const std::vector<ConnectedPad>& pads, const std::array<PlayerChoice, 4>& choices,
                                      const std::array<uint32_t, 4>& current) {
    std::array<uint32_t, 4> slots{};
    std::vector<bool> taken(pads.size(), false);
    auto indexOf = [&](uint32_t id) {
        for (size_t i = 0; i < pads.size(); ++i)
            if (pads[i].id == id) return static_cast<int>(i);
        return -1;
    };
    auto isGuid = [](const std::string& c) { return !c.empty() && c != "none"; };
    // 1. Slots that name a controller model take the first free pad of that model.
    for (size_t s = 0; s < slots.size(); ++s) {
        if (!isGuid(choices[s].controller)) continue;
        for (size_t i = 0; i < pads.size(); ++i) {
            if (taken[i] || pads[i].guid != choices[s].controller) continue;
            slots[s] = pads[i].id;
            taken[i] = true;
            break;
        }
    }
    // 2. The others keep the pad they hold while it is still connected and free.
    for (size_t s = 0; s < slots.size(); ++s) {
        if (slots[s] || choices[s].controller == "none" || !current[s]) continue;
        int i = indexOf(current[s]);
        if (i < 0 || taken[i]) continue;
        slots[s] = current[s];
        taken[i] = true;
    }
    // 3. Empty slots take the remaining pads in connection order.
    size_t next = 0;
    for (size_t s = 0; s < slots.size(); ++s) {
        if (slots[s] || choices[s].controller == "none") continue;
        while (next < pads.size() && taken[next]) ++next;
        if (next == pads.size()) break;
        slots[s] = pads[next].id;
        taken[next] = true;
    }
    return slots;
}

void applyAssignment(std::vector<ConnectedPad>& pads, const std::array<uint32_t, 4>& slots) {
    for (auto& p : pads) {
        p.player = -1;
        for (size_t s = 0; s < slots.size(); ++s)
            if (slots[s] && slots[s] == p.id) p.player = static_cast<int>(s);
    }
}

int deadzoneUnits(int percent) { return std::clamp(percent, 0, 100) * 32767 / 100; }

void applyStickDeadzone(int16_t& x, int16_t& y, int deadzoneUnits) {
    const double dz = std::clamp(deadzoneUnits, 0, 32766);
    const double mag = std::sqrt(static_cast<double>(x) * x + static_cast<double>(y) * y);
    if (mag <= dz) {
        x = y = 0;
        return;
    }
    if (dz <= 0) return;
    // Rescale the magnitude from (dz, 32767] to (0, 32767]; square-gate corners stay full.
    const double scale = (mag - dz) / (32767.0 - dz) * 32767.0 / mag;
    auto out = [scale](int16_t v) {
        return static_cast<int16_t>(std::clamp(std::lround(v * scale), -32768L, 32767L));
    };
    x = out(x);
    y = out(y);
}

std::string padFamily(const std::string& type) {
    if (type == "xbox360" || type == "xboxone") return "xbox";
    if (type == "ps3" || type == "ps4" || type == "ps5") return "playstation";
    if (type.rfind("switch", 0) == 0 || type.rfind("joycon", 0) == 0 || type == "gamecube") return "nintendo";
    return "generic";
}

std::string padTypeLabel(const std::string& type) {
    if (type == "xbox360") return "Xbox 360";
    if (type == "xboxone") return "Xbox One / Series";
    if (type == "ps3") return "DualShock 3";
    if (type == "ps4") return "DualShock 4";
    if (type == "ps5") return "DualSense";
    if (type == "switchpro") return "Switch Pro";
    if (type == "joyconpair") return "Joy-Con pair";
    if (type == "joyconleft" || type == "joyconright") return "Joy-Con";
    if (type == "gamecube") return "GameCube";
    if (type == "steam") return "Steam Controller";
    return "Controller";
}

// ---- quit chord --------------------------------------------------------------------------------

QuitChordMode quitChordMode(const std::string& value) {
    if (value == "back-start") return QuitChordMode::BackStart;
    if (value == "off") return QuitChordMode::Off;
    return QuitChordMode::GuideStart;
}

void QuitChordDetector::configure(QuitChordMode mode, int holdMs) {
    if (mode != mode_) holds_.clear();
    mode_ = mode;
    holdMs_ = std::max(1, holdMs);
}

bool QuitChordDetector::chordDown(const ChordButtons& b) const {
    switch (mode_) {
        case QuitChordMode::GuideStart: return b.start && (b.hasGuide ? b.guide : b.back);
        case QuitChordMode::BackStart: return b.back && b.start;
        case QuitChordMode::Off: return false;
    }
    return false;
}

bool QuitChordDetector::update(uint32_t padId, const ChordButtons& b, uint64_t nowMs) {
    Hold& h = holds_[padId];
    if (!chordDown(b)) {
        h = Hold{};  // releasing any chord button re-arms and restarts the hold
        return false;
    }
    if (!h.down) {
        h.down = true;
        h.since = nowMs;
    }
    if (h.blocked || nowMs - h.since < static_cast<uint64_t>(holdMs_)) return false;
    h.blocked = true;
    return true;
}

void QuitChordDetector::forget(uint32_t padId) { holds_.erase(padId); }

void QuitChordDetector::reset() {
    for (auto& [id, h] : holds_)
        if (h.down) h.blocked = true;
}

float QuitChordDetector::progress(uint64_t nowMs) const {
    float best = 0;
    for (auto& [id, h] : holds_) {
        if (!h.down || h.blocked) continue;
        const uint64_t held = nowMs > h.since ? nowMs - h.since : 0;
        best = std::max(best, std::min(1.0f, static_cast<float>(held) / static_cast<float>(holdMs_)));
    }
    return best;
}

}  // namespace rrm
