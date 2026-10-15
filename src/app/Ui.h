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
void closePlayerMenu(App& app);

// ---- gamepads (UiControllers.cpp, docs/CONTROLLERS.md) ---------------------------------------
// Buttons the UI acts on itself; moving and confirming are ImGui's own navigation.
enum class PadAction : uint8_t { Confirm, Back, TabPrev, TabNext, PagePrev, PageNext, Play, GameSettings };
enum class PadGlyph { Confirm, Back, North /*Y/Triangle*/, LB, RB, LT, RT, Start, Guide, BackButton };
// Draws a button the way the pad's family labels it (family from padFamily()).
void drawPadGlyph(ImDrawList*, ImVec2 center, float size, PadGlyph, const std::string& family);
float padGlyphWidth(PadGlyph glyph, float size, const std::string& family);
void drawPadHints(App&, const std::vector<std::pair<PadGlyph, const char*>>& hints);  // footer; nullptr text joins glyphs
std::vector<std::pair<PadGlyph, const char*>> padHintsFor(App&);                     // the current screen's hints
void drawControllersPage(App&);
bool padHintsVisible(App&);   // a gamepad is connected (and not touch-only)
std::string padFamilyNow(App&);  // family of the pad used last ("generic" without one)
void drawGameControls(App&, const GameDef& g, Backend* backend, const GameBackendOption* option);  // game page panel
// Applies app.padActions after the screen is drawn. popupWasOpen / itemWasActive describe the end
// of the previous frame, before ImGui's own navigation reacted to the same press.
void handlePadActions(App&, bool popupWasOpen, bool itemWasActive);
// Focus for gamepad users: after a screen change driven by the pad, the first caller gets true once
// and should call ImGui::SetKeyboardFocusHere() before its item.
bool takeFocusRequest();
void requestFocus();
bool navCursorVisible();
void focusOutline(float rounding = 0.0f);  // 3 px yellow outline around the last item while it has the pad's focus
// "(?)" after a widget the backend cannot use, reachable with the pad (shown while the pad's
// focus is visible). text may end in "##id" to keep markers apart.
void unsupportedMarker(const char* text);

}  // namespace rrm::ui
