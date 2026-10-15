// In-app player: game view, pause menu, hotkeys and the touch gamepad.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "app/Ui.h"
#include "core/Platform.h"
#include "libretro.h"

namespace rrm::ui {

namespace {

struct PlayerState {
    bool menuOpen = false;
    bool showStats = false;
    bool turboHeld = false;    // keyboard/gamepad hold
    bool turboToggle = false;  // touch button / menu toggle
    bool touchVisible = false;
    std::map<SDL_FingerID, ImVec2> fingers;  // normalised 0..1
    double lastTouch = -100;
    int stateSlot = 1;
} g_player;

std::string statePath(App& app, int slot) {
    std::string game = app.selectedGame.empty() ? "game" : app.selectedGame;
    fs::path p = paths().saves / utf8ToPath(game) / ("state" + std::to_string(slot) + ".state");
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    return pathToUtf8(p);
}

void applyTurbo(App& app) { app.host().setTurbo(g_player.turboHeld || g_player.turboToggle); }

void setMenu(App& app, bool open) {
    g_player.menuOpen = open;
    app.host().setPaused(open);
}

// ---- touch gamepad layout (in window points) -----------------------------------------------------
struct TouchLayout {
    ImVec2 dpad;
    float dpadR;
    ImVec2 face[4];  // B (bottom), A (right), Y (left), X (top) — libretro naming
    float faceR;
    ImVec4 l, r, l2, r2;   // x0,y0,x1,y1
    ImVec4 start, select, turbo, menu;
};

TouchLayout layoutFor(ImVec2 size) {
    TouchLayout t;
    float u = std::min(size.x, size.y);
    t.dpadR = u * 0.16f;
    t.dpad = ImVec2(size.x * 0.05f + t.dpadR, size.y - t.dpadR - u * 0.06f);
    t.faceR = u * 0.062f;
    ImVec2 c(size.x - size.x * 0.05f - t.dpadR, t.dpad.y);
    float d = t.faceR * 1.55f;
    t.face[0] = ImVec2(c.x, c.y + d);  // B (cross)
    t.face[1] = ImVec2(c.x + d, c.y);  // A (circle)
    t.face[2] = ImVec2(c.x - d, c.y);  // Y (square)
    t.face[3] = ImVec2(c.x, c.y - d);  // X (triangle)
    float bw = u * 0.2f, bh = u * 0.08f;
    t.l = ImVec4(size.x * 0.03f, size.y * 0.06f, size.x * 0.03f + bw, size.y * 0.06f + bh);
    t.l2 = ImVec4(t.l.x, t.l.w + u * 0.02f, t.l.z, t.l.w + u * 0.02f + bh);
    t.r = ImVec4(size.x - size.x * 0.03f - bw, t.l.y, size.x - size.x * 0.03f, t.l.w);
    t.r2 = ImVec4(t.r.x, t.l2.y, t.r.z, t.l2.w);
    float sw = u * 0.13f, sh = u * 0.06f;
    t.select = ImVec4(size.x * 0.5f - sw - u * 0.02f, size.y - sh - u * 0.03f, size.x * 0.5f - u * 0.02f, size.y - u * 0.03f);
    t.start = ImVec4(size.x * 0.5f + u * 0.02f, t.select.y, size.x * 0.5f + u * 0.02f + sw, t.select.w);
    t.turbo = ImVec4(size.x * 0.5f - sw * 0.5f, u * 0.03f, size.x * 0.5f + sw * 0.5f, u * 0.03f + sh);
    t.menu = ImVec4(size.x * 0.5f - sw * 0.5f, t.turbo.w + u * 0.02f, size.x * 0.5f + sw * 0.5f, t.turbo.w + u * 0.02f + sh);
    return t;
}

bool inRect(ImVec2 p, ImVec4 r) { return p.x >= r.x && p.x <= r.z && p.y >= r.y && p.y <= r.w; }
float dist(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

void updateTouchPad(App& app) {
    ImVec2 size = ImGui::GetMainViewport()->Size;
    TouchLayout t = layoutFor(size);
    lr::PadState pad;
    bool turbo = false;
    for (auto& [id, n] : g_player.fingers) {
        ImVec2 p(n.x * size.x, n.y * size.y);
        if (dist(p, t.dpad) < t.dpadR * 1.35f) {
            float dx = (p.x - t.dpad.x) / t.dpadR, dy = (p.y - t.dpad.y) / t.dpadR;
            pad.lx = (int16_t)std::clamp(dx * 32767.0f, -32767.0f, 32767.0f);
            pad.ly = (int16_t)std::clamp(dy * 32767.0f, -32767.0f, 32767.0f);
            if (dx < -0.35f) pad.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
            if (dx > 0.35f) pad.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
            if (dy < -0.35f) pad.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
            if (dy > 0.35f) pad.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
            continue;
        }
        const unsigned faceIds[4] = {RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y,
                                     RETRO_DEVICE_ID_JOYPAD_X};
        for (int i = 0; i < 4; ++i)
            if (dist(p, t.face[i]) < t.faceR * 1.25f) pad.buttons |= 1u << faceIds[i];
        if (inRect(p, t.l)) pad.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_L;
        if (inRect(p, t.r)) pad.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_R;
        if (inRect(p, t.l2)) pad.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_L2;
        if (inRect(p, t.r2)) pad.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_R2;
        if (inRect(p, t.start)) pad.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
        if (inRect(p, t.select)) pad.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
        if (inRect(p, t.turbo)) turbo = true;
    }
    app.host().setVirtualPad(pad);
    if (turbo != g_player.turboToggle) {
        g_player.turboToggle = turbo;
        applyTurbo(app);
    }
}

void drawTouchPad(App&) {
    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 o = ImGui::GetMainViewport()->Pos;
    TouchLayout t = layoutFor(size);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 fill = IM_COL32(255, 255, 255, 34), line = IM_COL32(255, 255, 255, 90), text = IM_COL32(255, 255, 255, 150);
    auto off = [&](ImVec2 p) { return ImVec2(o.x + p.x, o.y + p.y); };
    dl->AddCircleFilled(off(t.dpad), t.dpadR, fill, 48);
    dl->AddCircle(off(t.dpad), t.dpadR, line, 48, 2);
    dl->AddCircleFilled(off(t.dpad), t.dpadR * 0.35f, IM_COL32(255, 255, 255, 50), 32);
    const char* faceLabels[4] = {"X", "O", "[]", "/\\"};
    for (int i = 0; i < 4; ++i) {
        dl->AddCircleFilled(off(t.face[i]), t.faceR, fill, 32);
        dl->AddCircle(off(t.face[i]), t.faceR, line, 32, 2);
        ImVec2 ts = ImGui::CalcTextSize(faceLabels[i]);
        dl->AddText(off(ImVec2(t.face[i].x - ts.x * 0.5f, t.face[i].y - ts.y * 0.5f)), text, faceLabels[i]);
    }
    auto rect = [&](ImVec4 r, const char* label) {
        dl->AddRectFilled(off(ImVec2(r.x, r.y)), off(ImVec2(r.z, r.w)), fill, 10.0f);
        dl->AddRect(off(ImVec2(r.x, r.y)), off(ImVec2(r.z, r.w)), line, 10.0f, ImDrawFlags_None, 2.0f);
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(off(ImVec2((r.x + r.z - ts.x) * 0.5f, (r.y + r.w - ts.y) * 0.5f)), text, label);
    };
    rect(t.l, "L1");
    rect(t.l2, "L2");
    rect(t.r, "R1");
    rect(t.r2, "R2");
    rect(t.select, "SELECT");
    rect(t.start, "START");
    rect(t.turbo, ">> TURBO");
    rect(t.menu, "MENU");
}

}  // namespace

void onPlayerStarted(App& app) {
    g_player = PlayerState();
    g_player.touchVisible = app.touchUi;
}

bool playerMenuOpen() { return g_player.menuOpen; }

bool playerHandleEvent(App& app, const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.key == SDLK_ESCAPE) {
                setMenu(app, !g_player.menuOpen);
                return true;
            }
            if (e.key.key == SDLK_TAB) {
                g_player.turboHeld = true;
                applyTurbo(app);
                return true;
            }
            if (e.key.key == SDLK_F11) {
                bool fs = (SDL_GetWindowFlags(app.window()) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(app.window(), !fs);
                return true;
            }
            if (e.key.key == SDLK_F5) {
                app.toast(app.host().saveState(statePath(app, g_player.stateSlot)) ? "State saved" : "Saving state failed",
                          false);
                return true;
            }
            if (e.key.key == SDLK_F8) {
                app.toast(app.host().loadState(statePath(app, g_player.stateSlot)) ? "State loaded" : "No saved state", false);
                return true;
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (e.key.key == SDLK_TAB) {
                g_player.turboHeld = false;
                applyTurbo(app);
                return true;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
                setMenu(app, !g_player.menuOpen);
                return true;
            }
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK && !g_player.menuOpen) {
                g_player.turboHeld = true;
                applyTurbo(app);
                return true;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
                g_player.turboHeld = false;
                applyTurbo(app);
                return true;
            }
            break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
            g_player.touchVisible = true;
            g_player.lastTouch = app.now();
            if (!g_player.menuOpen) {
                ImVec2 size = ImGui::GetMainViewport()->Size;
                TouchLayout t = layoutFor(size);
                ImVec2 p(e.tfinger.x * size.x, e.tfinger.y * size.y);
                if (e.type == SDL_EVENT_FINGER_DOWN && inRect(p, t.menu)) {
                    setMenu(app, true);
                    return true;
                }
                g_player.fingers[e.tfinger.fingerID] = ImVec2(e.tfinger.x, e.tfinger.y);
                return true;
            }
            break;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            g_player.fingers.erase(e.tfinger.fingerID);
            if (!g_player.menuOpen) return true;
            break;
        default: break;
    }
    // While the pause menu is open, input goes to the UI, not the game.
    if (g_player.menuOpen) {
        switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                return false;  // App forwards to ImGui only (see playerMenuOpen)
            default: break;
        }
    }
    return false;
}

void drawGameView(App& app, int fbWidth, int fbHeight) {
    app.host().render(0, 0, fbWidth, fbHeight);
}

void drawPlayer(App& app) {
    if (g_player.touchVisible && !g_player.menuOpen) {
        updateTouchPad(app);
        drawTouchPad(app);
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (app.host().turbo()) {
        GameSettings s = app.settingsFor(app.selectedGame);
        char buf[48];
        snprintf(buf, sizeof(buf), s.turbo == 0 ? ">> FAST-FORWARD" : ">> %d%%", s.turbo);
        fg->AddText(app.fonts.display, S(30), ImVec2(vp->Pos.x + S(20), vp->Pos.y + S(14)), IM_COL32(245, 196, 0, 230), buf);
    }
    if (g_player.showStats) {
        lr::HostStats st = app.host().stats();
        char buf[200];
        snprintf(buf, sizeof(buf), "%s  |  core %.2f fps  |  emu %.1f fps  |  present %.1f fps  |  %dx%d%s", st.coreName.c_str(),
                 st.coreFps, st.measuredFps, st.presentFps, st.baseWidth, st.baseHeight, st.hwRender ? "  |  GPU" : "");
        fg->AddRectFilled(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - S(30)), ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                          IM_COL32(0, 0, 0, 150));
        fg->AddText(ImVec2(vp->Pos.x + S(12), vp->Pos.y + vp->Size.y - S(26)), IM_COL32(230, 230, 230, 255), buf);
    }
    if (!g_player.menuOpen) return;

    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(22)));
    ImGui::Begin("##pause", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    const GameDef* g = app.catalog.game(app.selectedGame);
    titleText(app, g ? g->title.c_str() : "Paused", S(36), IM_COL32(255, 255, 255, 255));
    ImGui::TextDisabled("Paused");
    ImVec2 bs(S(320), S(48));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (primaryButton("RESUME", bs, IM_COL32(245, 196, 0, 255))) setMenu(app, false);
    bool turbo = g_player.turboToggle;
    if (ImGui::Checkbox("Fast-forward (toggle)", &turbo)) {
        g_player.turboToggle = turbo;
        applyTurbo(app);
    }
    GameSettings s = app.settingsFor(app.selectedGame);
    int speed = s.speed;
    ImGui::SetNextItemWidth(S(220));
    if (ImGui::SliderInt("Game speed", &speed, 50, 300, "%d%%")) app.host().setSpeedPercent(speed);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        s.speed = speed;
        app.setGameSettings(app.selectedGame, s, true);
    }
    ImGui::Checkbox("Show performance stats", &g_player.showStats);
    if (app.touchUi) ImGui::Checkbox("Touch controls", &g_player.touchVisible);
    ImGui::SeparatorText("Save states");
    ImGui::SetNextItemWidth(S(120));
    ImGui::SliderInt("Slot", &g_player.stateSlot, 1, 9);
    if (secondaryButton("Save state", ImVec2(S(155), 0)))
        app.toast(app.host().saveState(statePath(app, g_player.stateSlot)) ? "State saved" : "Saving state failed");
    ImGui::SameLine();
    if (secondaryButton("Load state", ImVec2(S(155), 0))) {
        bool ok = app.host().loadState(statePath(app, g_player.stateSlot));
        app.toast(ok ? "State loaded" : "No saved state in this slot", !ok);
        if (ok) setMenu(app, false);
    }
    ImGui::Separator();
    if (secondaryButton("Reset game", ImVec2(bs.x, 0))) {
        app.host().reset();
        setMenu(app, false);
    }
    if (secondaryButton("Quit to Marathon", ImVec2(bs.x, 0))) {
        g_player.menuOpen = false;
        app.exitPlayer();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace rrm::ui
