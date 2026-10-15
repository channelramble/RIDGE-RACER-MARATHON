// Dear ImGui screens. Everything here runs on the main thread except
// deliverDialog(), which SDL may call from a dialog thread.
#pragma once

#include <string>
#include <vector>

#include "app/App.h"
#include "imgui.h"

namespace rrm::ui {

// Frame entry points used by App.
void draw(App& app);
void drawGameView(App& app, int fbWidth, int fbHeight);
void onPlayerStarted(App& app);
bool playerHandleEvent(App& app, const SDL_Event& e);  // true = consumed by the player UI
bool playerMenuOpen();

// File dialogs
void deliverDialog(App& app, DialogResult&& r);
std::vector<DialogResult> takeDialogs(App& app);
void onDialogResult(App& app, const DialogResult& r);

// ---- shared widgets (UiCommon.cpp) ---------------------------------------------------------
ImU32 hexColor(const std::string& hex, float alpha = 1.0f);
ImVec4 hexColorV(const std::string& hex, float alpha = 1.0f);
float S(float v);  // scales a size by the UI scale

void titleText(App& app, const char* text, float size, ImU32 color);
bool primaryButton(const char* label, const ImVec2& size, ImU32 color, bool enabled = true);
bool secondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0), bool enabled = true);
void chip(const char* text, ImU32 bg, ImU32 fg);
void sectionHeader(App& app, const char* text);
void helpMarker(const char* text);
std::string ellipsizePath(const std::string& path, float maxWidth);
void drawCardArt(ImDrawList* dl, ImVec2 min, ImVec2 max, const GameDef& g, float rounding, bool hovered);
void progressLine(TaskProgress& p, float width);
std::string formatBytes(uint64_t bytes);
std::string formatDuration(int64_t seconds);
void beginPanel(const char* id, const ImVec2& size = ImVec2(0, 0));
void endPanel();

// Settings editor shared by the game page and global settings.
// caps == nullptr shows every option enabled. Returns true when something changed.
bool settingsEditor(App& app, GameSettings& s, const json* caps, const std::string& backendName);

// Screens (UiScreens.cpp)
void drawTopBar(App& app);
void drawLibrary(App& app);
void drawGame(App& app);
void drawAddGames(App& app);
void drawBackends(App& app);
void drawSettings(App& app);
void drawAbout(App& app);
void drawOverlays(App& app);  // running-game banner, modal, toasts

// Player (UiPlayer.cpp)
void drawPlayer(App& app);

}  // namespace rrm::ui
