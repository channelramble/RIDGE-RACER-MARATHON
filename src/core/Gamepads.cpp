#include "core/Gamepads.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace rrm {

namespace {

constexpr uint64_t kQuietStartMs = 1500;   // no plug messages for pads found while starting
constexpr uint64_t kNoticeGapMs = 2000;    // at most one plug message per controller per 2 s
constexpr int kUsedThreshold = 16384;      // an axis past 50 % marks its pad as last used

void setNameIndices(std::vector<ConnectedPad>& pads) {
    for (size_t i = 0; i < pads.size(); ++i) {
        pads[i].nameIndex = 1;
        for (size_t j = 0; j < i; ++j)
            if (pads[j].name == pads[i].name) ++pads[i].nameIndex;
    }
}

}  // namespace

ConnectedPad describePad(SDL_JoystickID id) {
    ConnectedPad p;
    p.id = id;
    if (const char* n = SDL_GetGamepadNameForID(id)) p.name = n;
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof(guid));
    p.guid = guid;
    for (char& c : p.guid) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const SDL_GamepadType type = SDL_GetGamepadTypeForID(id);
    const char* typeName = type != SDL_GAMEPAD_TYPE_UNKNOWN ? SDL_GetGamepadStringForType(type) : nullptr;
    p.type = typeName && *typeName ? typeName : "standard";
    p.vendor = SDL_GetGamepadVendorForID(id);
    p.product = SDL_GetGamepadProductForID(id);
    p.virtualPad = SDL_IsJoystickVirtual(id);
    // Button and rumble capabilities need an open handle; borrow one when nobody holds it yet.
    SDL_Gamepad* pad = SDL_GetGamepadFromID(id);
    const bool borrowed = !pad;
    if (borrowed) pad = SDL_OpenGamepad(id);
    if (pad) {
        p.hasGuide = SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_GUIDE);
        p.hasRumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        if (borrowed) SDL_CloseGamepad(pad);
    }
    return p;
}

std::vector<ConnectedPad> connectedPads() {
    std::vector<ConnectedPad> pads;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) pads.push_back(describePad(ids[i]));
    SDL_free(ids);
    setNameIndices(pads);
    return pads;
}

// ---- GamepadMonitor ---------------------------------------------------------------------------

void GamepadMonitor::open() {
    if (open_) return;
    open_ = true;
    started_ = false;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) add(ids[i]);
    SDL_free(ids);
    raw_.clear();  // pads that were already there are not news
}

void GamepadMonitor::close() {
    for (auto& p : pads_)
        if (p.handle) SDL_CloseGamepad(p.handle);
    pads_.clear();
    raw_.clear();
    notices_.clear();
    slots_ = {};
    chord_ = QuitChordDetector();
    lastUsed_ = 0;
    open_ = false;
}

void GamepadMonitor::add(SDL_JoystickID id) {
    if (connected(id)) return;
    SDL_Gamepad* handle = SDL_OpenGamepad(id);  // reference counted: ImGui and the cores open their own
    if (!handle) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "[pads] cannot open gamepad %u: %s", static_cast<unsigned>(id), SDL_GetError());
        return;
    }
    pads_.push_back({describePad(id), handle});
    reassign();
    const ConnectedPad& info = pads_.back().info;
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "[pads] connected: %s (%s, %s)%s", info.name.c_str(), info.type.c_str(),
                info.guid.c_str(), info.player >= 0 ? (" player " + std::to_string(info.player + 1)).c_str() : "");
    raw_.push_back({PadEvent::Kind::Connected, info});
}

void GamepadMonitor::remove(SDL_JoystickID id) {
    auto it = std::find_if(pads_.begin(), pads_.end(), [id](const Pad& p) { return p.info.id == id; });
    if (it == pads_.end()) return;
    ConnectedPad info = it->info;  // keeps the player slot it had
    if (it->handle) SDL_CloseGamepad(it->handle);
    pads_.erase(it);
    chord_.forget(id);
    if (lastUsed_ == id) lastUsed_ = 0;
    reassign();
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "[pads] disconnected: %s", info.name.c_str());
    raw_.push_back({PadEvent::Kind::Disconnected, info});
}

void GamepadMonitor::reassign() {
    std::vector<ConnectedPad> infos;
    for (auto& p : pads_) infos.push_back(p.info);
    slots_ = assignPlayers(infos, choices_, slots_);
    applyAssignment(infos, slots_);
    for (size_t i = 0; i < pads_.size(); ++i) pads_[i].info.player = infos[i].player;
}

void GamepadMonitor::handleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            if (open_) add(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (open_) remove(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            lastUsed_ = e.gbutton.which;
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (std::abs(static_cast<int>(e.gaxis.value)) > kUsedThreshold) lastUsed_ = e.gaxis.which;
            break;
        default: break;
    }
}

std::vector<PadEvent> GamepadMonitor::update(uint64_t nowMs, const ControllerSettings& c) {
    std::vector<PadEvent> out;
    if (!started_) {
        started_ = true;
        quietUntil_ = nowMs + kQuietStartMs;
    }
    if (c.players != choices_) {
        choices_ = c.players;
        reassign();
    }
    chord_.configure(quitChordMode(c.quitChord), c.quitHoldMs);

    // Plug messages. A Bluetooth pad that drops out and comes back within 2 s says nothing;
    // otherwise the newest state is reported once the 2 s have passed.
    auto emit = [&](Notice& n, PadEvent ev) {
        if (ev.kind == PadEvent::Kind::Connected) {
            for (auto& p : pads_)
                if (p.info.id == ev.pad.id) ev.pad.player = p.info.player;
        }
        out.push_back(ev);
        n.lastAt = nowMs;
        n.hasLast = true;
        n.pending = false;
    };
    for (auto& ev : raw_) {
        if (nowMs < quietUntil_) continue;
        Notice& n = notices_[ev.pad.guid + "|" + ev.pad.name];
        if (n.pending && n.event.kind != ev.kind) {
            n.pending = false;
            continue;
        }
        if (n.hasLast && nowMs - n.lastAt < kNoticeGapMs) {
            n.pending = true;
            n.event = ev;
            continue;
        }
        emit(n, ev);
    }
    raw_.clear();
    for (auto& [key, n] : notices_)
        if (n.pending && nowMs - n.lastAt >= kNoticeGapMs) emit(n, n.event);

    // Quit chord.
    for (auto& p : pads_) {
        ChordButtons b;
        b.guide = SDL_GetGamepadButton(p.handle, SDL_GAMEPAD_BUTTON_GUIDE);
        b.back = SDL_GetGamepadButton(p.handle, SDL_GAMEPAD_BUTTON_BACK);
        b.start = SDL_GetGamepadButton(p.handle, SDL_GAMEPAD_BUTTON_START);
        b.hasGuide = p.info.hasGuide;
        if (!chord_.update(p.info.id, b, nowMs)) continue;
        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "[pads] quit chord on %s", p.info.name.c_str());
        out.push_back({PadEvent::Kind::QuitChord, p.info});
        // A short, light pulse confirms it (about 40 %).
        if (c.vibration && p.info.hasRumble) SDL_RumbleGamepad(p.handle, 0x6666, 0x6666, 150);
    }
    return out;
}

std::vector<ConnectedPad> GamepadMonitor::pads() const {
    std::vector<ConnectedPad> out;
    for (auto& p : pads_) out.push_back(p.info);
    setNameIndices(out);
    return out;
}

SDL_Gamepad* GamepadMonitor::gamepad(uint32_t id) const {
    for (auto& p : pads_)
        if (p.info.id == id) return p.handle;
    return nullptr;
}

uint32_t GamepadMonitor::lastUsed() const {
    if (lastUsed_ && connected(lastUsed_)) return lastUsed_;
    return pads_.empty() ? 0 : pads_.front().info.id;
}

bool GamepadMonitor::rumble(uint32_t id, uint16_t low, uint16_t high, uint32_t ms) {
    SDL_Gamepad* pad = gamepad(id);
    return pad && SDL_RumbleGamepad(pad, low, high, ms);
}

void GamepadMonitor::resetChord() { chord_.reset(); }

float GamepadMonitor::chordProgress(uint64_t nowMs) const { return chord_.progress(nowMs); }

uint32_t GamepadMonitor::playerPad(int player) const {
    return player >= 0 && player < static_cast<int>(slots_.size()) ? slots_[player] : 0;
}

bool GamepadMonitor::connected(uint32_t id) const { return gamepad(id) != nullptr; }

}  // namespace rrm
