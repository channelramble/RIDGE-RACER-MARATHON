// Drives Marathon's real UI (the App singleton, its window and Dear ImGui) with SDL virtual
// gamepads, the way a player would with a controller, and checks what happens:
//   1. RB / LB move between the tabs (wrapping), RT switches Settings to its Controllers page,
//      B goes back (Controllers -> General -> Library);
//   2. on the library the d-pad moves between cards, Y opens the focused game's page, B returns;
//   3. Start on a game that is not ready opens its page and launches nothing;
//   4. Guide + Start held on the library (the quit chord) launches nothing;
//   5. on the Controllers page "Test" starts the pad test, a short B does nothing, B held 1.1 s ends it;
//   6. plugging in a second pad shows "Controller connected", unplugging "Controller disconnected".
// No network, no backend is started; the data folder is a fresh /tmp/rrm-ui-pad-<ticks>.
//
//   ui_pad_tool [--png out.png] [--keep]
//   ui_pad_tool --ingame GAME_FILE --cores DIR [--png out.png] [--keep]
//
// --ingame also plays a game inside Marathon (opt-in: it starts a libretro core from DIR, linked,
// never downloaded; the game file is only read): Start on its card races, Guide alone opens the
// pause menu when let go, B resumes, unplugging Player 1's controller pauses with a message,
// and Guide + Start held quits to Marathon.
//
// Run it from the repository root (catalog/ and assets/ are read from the current folder unless
// RRM_RESOURCES says otherwise). HOME (and CFFIXED_USER_HOME on macOS) are pointed at a /tmp folder
// unless they already are. --png saves the Controllers page (out.png) and the running pad test
// (out-test.png); --keep keeps the data folder.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/App.h"
#include "app/Ui.h"
#include "core/Platform.h"
#include "core/Scanner.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

using namespace rrm;

namespace {

int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failures;                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

const char* screenName(Screen s) {
    switch (s) {
        case Screen::Library: return "Library";
        case Screen::Game: return "Game";
        case Screen::AddGames: return "AddGames";
        case Screen::Backends: return "Backends";
        case Screen::Settings: return "Settings";
        case Screen::About: return "About";
        case Screen::Playing: return "Playing";
    }
    return "?";
}

struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* js = nullptr;
};

bool SDLCALL onRumble(void*, Uint16, Uint16) { return true; }

std::string g_png;  // --png: base name for the screenshots

void frame();

void screenshot(const char* suffix) {
    if (g_png.empty()) return;
    std::string path = g_png;
    if (path.size() > 4 && path.substr(path.size() - 4) == ".png") path.resize(path.size() - 4);
    path += std::string(suffix) + ".png";
    app().requestScreenshot(path);
    frame();
    std::printf("  saved %s\n", path.c_str());
}

VirtualPad attach(const char* name, Uint16 vendor, Uint16 product) {
    VirtualPad v;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.name = name;
    desc.Rumble = onRumble;
    v.id = SDL_AttachVirtualJoystick(&desc);
    if (!v.id) std::fprintf(stderr, "attach %s: %s\n", name, SDL_GetError());
    v.js = SDL_OpenJoystick(v.id);
    // Virtual gamepad triggers use the full range: -32768 is released.
    SDL_SetJoystickVirtualAxis(v.js, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    SDL_SetJoystickVirtualAxis(v.js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    return v;
}

void detach(VirtualPad& v) {
    SDL_CloseJoystick(v.js);
    SDL_DetachVirtualJoystick(v.id);
    v = {};
}

bool g_running = true;

void frame() {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (!app().handleEvent(e)) g_running = false;
    if (!app().iterate()) g_running = false;
}

void runFor(int ms) {
    const uint64_t end = SDL_GetTicks() + static_cast<uint64_t>(ms);
    do frame();
    while (g_running && SDL_GetTicks() < end);
}

void press(VirtualPad& p, SDL_GamepadButton b, int holdMs = 100) {
    SDL_SetJoystickVirtualButton(p.js, b, true);
    runFor(holdMs);
    SDL_SetJoystickVirtualButton(p.js, b, false);
    runFor(150);
}

void pullTrigger(VirtualPad& p, SDL_GamepadAxis axis) {
    SDL_SetJoystickVirtualAxis(p.js, axis, 32767);
    runFor(100);
    SDL_SetJoystickVirtualAxis(p.js, axis, -32768);
    runFor(150);
}

bool toastContains(const std::string& text) {
    for (auto& t : app().toasts)
        if (t.text.find(text) != std::string::npos) return true;
    return false;
}

void expectScreen(Screen want, const char* step) {
    const bool ok = app().screen == want;
    std::printf("  %-48s screen %-9s %s\n", step, screenName(app().screen), ok ? "ok" : "WRONG");
    CHECK(ok);
}

bool launchedSomething() { return app().running() != nullptr || app().host().running(); }

void setEnv(const char* key, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

void key(SDL_Keycode code, SDL_Scancode scan, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.key = code;
    event.key.scancode = scan;
    event.key.down = down;
    event.key.windowID = SDL_GetWindowID(app().window());
    app().handleEvent(event);
}

void activatePauseItem(const char* label) {
    auto* window = ImGui::FindWindowByName("##pause");
    CHECK(window);
    if (!window) return;
    ImGui::FocusWindow(window);
    ImGui::SetNavID(window->GetID(label), ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
    ImGui::ActivateItemByID(window->GetID(label));
    frame();
}

void checkPlaybackControls(App& a) {
    CHECK(ui::playerMenuOpen());
    activatePauseItem("Fast-forward (toggle)");
    CHECK(a.host().turbo());
    ui::closePlayerMenu(a);
    runFor(500);
    CHECK(a.host().turbo());
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, true);
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, false);
    frame();
    activatePauseItem("Fast-forward (toggle)");
    CHECK(!a.host().turbo());
    runFor(100);
    CHECK(!a.host().turbo());

    // Measure actual emulation after each transition: toggling off must restore
    // normal pacing, rather than just clearing the UI indicator.
    ui::closePlayerMenu(a);
    auto rate = [&]() {
        const auto frames = a.host().stats().framesRun;
        const auto start = SDL_GetTicks();
        runFor(800);
        return 1000.0 * (a.host().stats().framesRun - frames) / (SDL_GetTicks() - start);
    };
    const double normal = rate();
    a.host().setTurbo(true);
    const double fast = rate();
    a.host().setTurbo(false);
    const double restored = rate();
    a.host().setSpeedPercent(200);
    const double doubled = rate();
    a.host().setSpeedPercent(100);
    const double restoredSpeed = rate();
    std::printf("  emulated fps normal %.1f, turbo %.1f, off %.1f, 200%% %.1f, 100%% %.1f\n",
                normal, fast, restored, doubled, restoredSpeed);
    CHECK(normal > 40 && normal < 80);
    CHECK(fast > normal * 1.5);
    CHECK(restored > normal * 0.8 && restored < normal * 1.2);
    CHECK(doubled > normal * 1.5);
    CHECK(restoredSpeed > normal * 0.8 && restoredSpeed < normal * 1.2);
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, true);
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, false);
    frame();

    // Edit the actual pause-menu slider over several frames, then deactivate it.
    const int before = a.settingsFor(a.selectedGame).speed;
    activatePauseItem("Game speed");
    key(SDLK_RIGHT, SDL_SCANCODE_RIGHT, true);
    runFor(150);
    key(SDLK_RIGHT, SDL_SCANCODE_RIGHT, false);
    frame();
    activatePauseItem("Game speed");
    runFor(100);
    const int after = a.settingsFor(a.selectedGame).speed;
    std::printf("  speed slider persisted: %d -> %d\n", before, after);
    CHECK(after > before);
    CHECK(a.library.find(a.selectedGame)->customSettings);
    // Restore through the same control, not a direct edit of the library.
    activatePauseItem("Game speed");
    key(SDLK_LEFT, SDL_SCANCODE_LEFT, true);
    runFor(150);
    key(SDLK_LEFT, SDL_SCANCODE_LEFT, false);
    frame();
    activatePauseItem("Game speed");
    runFor(100);
    CHECK(a.settingsFor(a.selectedGame).speed == before);

    // Tab navigates the pause menu instead of silently engaging fast-forward.
    key(SDLK_TAB, SDL_SCANCODE_TAB, true);
    frame();
    key(SDLK_TAB, SDL_SCANCODE_TAB, false);
    frame();
    CHECK(!a.host().turbo());
    ui::closePlayerMenu(a);
    key(SDLK_TAB, SDL_SCANCODE_TAB, true);
    CHECK(a.host().turbo());
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    a.handleEvent(focus);
    CHECK(!a.host().turbo());
    key(SDLK_TAB, SDL_SCANCODE_TAB, false);
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, true);
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, false);
    frame();
    CHECK(ui::playerMenuOpen());
}

void checkTextInputLifecycle(App& a) {
    // Gameplay/menu keys must never opt into macOS's press-and-hold accent UI.
    CHECK(!SDL_TextInputActive(a.window()));
    if (SDL_GetKeyboardFocus() != a.window()) {
        std::printf("text input field check skipped: native window has no keyboard focus\n");
        return;
    }
    char text[32]{};
    auto textFrame = [&](bool focus, bool showField) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("Text input regression");
        if (showField) {
            if (focus) ImGui::SetKeyboardFocusHere();
            ImGui::InputText("Text", text, sizeof(text));
        }
        ImGui::End();
        ImGui::Render();
    };
    textFrame(true, true);
    textFrame(false, true);
    CHECK(SDL_TextInputActive(a.window()));
    SDL_Event entered{};
    entered.type = SDL_EVENT_TEXT_INPUT;
    entered.text.windowID = SDL_GetWindowID(a.window());
    entered.text.text = "caf\xC3\xA9";
    a.handleEvent(entered);
    textFrame(false, true);
    CHECK(std::string(text) == "caf\xC3\xA9");
    textFrame(false, false);
    textFrame(false, false);
    CHECK(!SDL_TextInputActive(a.window()));
    std::printf("text input: inactive on menus, active for a field, UTF-8 entered, inactive after leaving\n");
}

void checkLibrarySetup(App& a) {
    const fs::path root = paths().data / "scan-fixture";
    const fs::path bios = root / "a/b/c/d/e/f/g/renamed-bios";
    fs::create_directories(bios.parent_path());
    std::string bytes(512 * 1024, '\0');
    bytes.replace(0x108, 4, "CEX-");
    std::ofstream(bios, std::ios::binary) << bytes;
    auto selectedBiosIs = [&](const fs::path& expected) {
        std::error_code error;
        // Windows adds the drive to a root-relative /tmp fixture during scanning.
        // Compare the file itself rather than its spelling.
        return fs::equivalent(utf8ToPath(a.library.systemFile("ps1-bios")), expected, error);
    };
    a.scanFolder(pathToUtf8(root));
    runFor(500);
    CHECK(selectedBiosIs(bios));
    const fs::path selected = paths().data / "manual-bios.bin";
    std::ofstream(selected, std::ios::binary) << bytes;
    a.library.setSystemFile("ps1-bios", pathToUtf8(selected));
    a.scanFolder(pathToUtf8(root));
    runFor(500);
    CHECK(a.library.systemFile("ps1-bios") == pathToUtf8(selected));
    fs::remove(selected);
    a.scanFolder(pathToUtf8(root));
    runFor(500);
    CHECK(selectedBiosIs(bios));
    a.library.setSystemFile("ps1-bios", ""); // synthetic kernel must never be given to a real core

    const fs::path exe = root / "r4-runtime";
    std::ofstream(exe) << "fixture";
    std::ofstream(root / "README-SETUP.txt") << "fixture setup kit";
    std::ofstream(root / "r4.cue") << "fixture disc";
    a.library.setInstall({"r4-recomp", "test", pathToUtf8(root), pathToUtf8(exe), false});
    auto& entry = a.library.entry("r4");
    entry.files.path = pathToUtf8(root / "r4.cue");
    entry.settings.backend = "r4-recomp";
    CHECK(a.statusOf(*a.catalog.game("r4")) == App::GameStatus::Ready);
    a.library.removeInstall("r4-recomp");
    entry = LibraryEntry{};
    entry.gameId = "r4";

    for (const char* id : {"ridge-racer", "rage-racer", "r4", "ridge-racer-v"}) {
        auto& e = a.library.entry(id);
        e.customSettings = true;
        e.settings.aspect = "31:9";
        const auto* option = a.chosenOption(*a.catalog.game(id));
        CHECK(option && option->backend == (std::string(id) == "ridge-racer-v" ? "pcsx2" : "lr-swanstation"));
        e = LibraryEntry{};
        e.gameId = id;
    }
    a.library.save();
    std::printf("library: deep BIOS discovery, valid selection preserved, stale path repaired, R4 wizard ready, 31:9 Auto routing\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string& png = g_png;
    std::string ingame, coresDir;
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--png" && i + 1 < argc) png = argv[++i];
        else if (a == "--ingame" && i + 1 < argc) ingame = argv[++i];
        else if (a == "--cores" && i + 1 < argc) coresDir = argv[++i];
        else if (a == "--keep") keep = true;
        else {
            std::fprintf(stderr, "usage: ui_pad_tool [--png out.png] [--keep] [--ingame GAME_FILE --cores DIR]\n");
            return 2;
        }
    }
    if (!ingame.empty() && coresDir.empty()) {
        std::fprintf(stderr, "--ingame needs --cores DIR (libretro cores to link)\n");
        return 2;
    }
    const std::string stamp = std::to_string(SDL_GetTicksNS() ^ static_cast<uint64_t>(std::time(nullptr)));
    const std::string data = "/tmp/rrm-ui-pad-" + stamp;
    // Never touch the real home folder: SDL, ImGui and macOS only ever see this one.
    const char* home = std::getenv("HOME");
    const std::string tmpHome = data + "-home";
    const bool ownHome = !home || std::string(home).rfind("/tmp/", 0) != 0;
    if (ownHome) {
        std::filesystem::create_directories(tmpHome);
        setEnv("HOME", tmpHome);
        setEnv("CFFIXED_USER_HOME", tmpHome);
    }
    if (!std::getenv("RRM_RESOURCES")) setEnv("RRM_RESOURCES", std::filesystem::current_path().string());
    if (!std::filesystem::exists(std::filesystem::path(std::getenv("RRM_RESOURCES")) / "catalog" / "games.json")) {
        std::fprintf(stderr, "run from the repository root (catalog/games.json not found)\n");
        return 2;
    }

    std::vector<std::string> args = {"ui_pad_tool", "--data", data, "--size", "1280x800"};
    std::vector<char*> cargs;
    for (auto& a : args) cargs.push_back(a.data());
    if (!app().init(static_cast<int>(cargs.size()), cargs.data())) {
        std::fprintf(stderr, "Marathon did not start\n");
        return 1;
    }
    App& a = app();
    checkLibrarySetup(a);
    CHECK(!SDL_TextInputActive(a.window()));  // also catches an untracked startup request
    VirtualPad pad = attach("Marathon UI Test Pad", 0x045E, 0x0B12);  // Xbox Series X ids
    SDL_RaiseWindow(a.window());
    runFor(800);
    // The menus follow the pad only while Marathon's window has the input focus. A tool started
    // from a terminal or a script may not get it (macOS keeps the active app), so then the test
    // routes the pad as if it had.
    const bool focused = (SDL_GetWindowFlags(a.window()) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (!focused) a.assumeFocus = true;
    runFor(200);  // (the routing changes on the next frame)
    std::printf("window focus: %s, pads: %zu\n", focused ? "yes" : "no (testing as if focused)", a.padSnapshot().size());
    CHECK(a.padSnapshot().size() == 1);
    CHECK(ui::padHintsVisible(a));
    checkTextInputLifecycle(a);
    runFor(200);

    std::printf("1. tabs and Back\n");
    expectScreen(Screen::Library, "start");
    press(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    expectScreen(Screen::AddGames, "RB");
    press(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    expectScreen(Screen::Backends, "RB");
    press(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    expectScreen(Screen::Settings, "RB");
    CHECK(a.settingsPage == App::SettingsPage::General);
    pullTrigger(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    std::printf("  %-48s page %s\n", "RT", a.settingsPage == App::SettingsPage::Controllers ? "Controllers ok" : "General WRONG");
    CHECK(a.settingsPage == App::SettingsPage::Controllers);
    press(pad, SDL_GAMEPAD_BUTTON_EAST);
    std::printf("  %-48s page %s\n", "B", a.settingsPage == App::SettingsPage::General ? "General ok" : "Controllers WRONG");
    CHECK(a.settingsPage == App::SettingsPage::General);
    expectScreen(Screen::Settings, "B (still Settings)");
    press(pad, SDL_GAMEPAD_BUTTON_EAST);
    expectScreen(Screen::Library, "B");
    press(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    expectScreen(Screen::About, "LB from Library wraps");
    press(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    expectScreen(Screen::Library, "RB wraps back");

    std::printf("2. library focus, Y and B\n");
    runFor(200);
    const std::string first = a.focusedGame;
    press(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    const std::string second = a.focusedGame;
    std::printf("  focused card: %s -> d-pad right -> %s\n", first.c_str(), second.c_str());
    screenshot("-library");
    CHECK(!first.empty());
    CHECK(!second.empty() && second != first);
    press(pad, SDL_GAMEPAD_BUTTON_NORTH);
    expectScreen(Screen::Game, "Y");
    runFor(200);
    screenshot("-game");
    if (!png.empty()) {
        // The left stick scrolls the page (here down to the CONTROLS panel).
        SDL_SetJoystickVirtualAxis(pad.js, SDL_GAMEPAD_AXIS_LEFTY, 32767);
        runFor(1500);
        SDL_SetJoystickVirtualAxis(pad.js, SDL_GAMEPAD_AXIS_LEFTY, 0);
        runFor(100);
        screenshot("-game-scrolled");
    }
    std::printf("  game page: %s\n", a.selectedGame.c_str());
    CHECK(a.selectedGame == second);
    press(pad, SDL_GAMEPAD_BUTTON_EAST);
    expectScreen(Screen::Library, "B");
    runFor(200);
    std::printf("  focus back on: %s\n", a.focusedGame.c_str());
    CHECK(a.focusedGame == second);

    std::printf("3. Start on a game without files\n");
    const GameDef* g = a.catalog.game(a.focusedGame);
    CHECK(g && a.statusOf(*g) != App::GameStatus::Ready);
    press(pad, SDL_GAMEPAD_BUTTON_START);
    expectScreen(Screen::Game, "Start");
    CHECK(!launchedSomething());
    press(pad, SDL_GAMEPAD_BUTTON_EAST);
    expectScreen(Screen::Library, "B");

    std::printf("4. Guide + Start held on the library\n");
    SDL_SetJoystickVirtualButton(pad.js, SDL_GAMEPAD_BUTTON_GUIDE, true);
    runFor(100);
    SDL_SetJoystickVirtualButton(pad.js, SDL_GAMEPAD_BUTTON_START, true);
    runFor(1300);
    SDL_SetJoystickVirtualButton(pad.js, SDL_GAMEPAD_BUTTON_START, false);
    runFor(100);
    SDL_SetJoystickVirtualButton(pad.js, SDL_GAMEPAD_BUTTON_GUIDE, false);
    runFor(300);
    expectScreen(Screen::Library, "held 1.3 s, released");
    CHECK(!launchedSomething());

    std::printf("5. Controllers page test\n");
    for (int i = 0; i < 3; ++i) press(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    pullTrigger(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    expectScreen(Screen::Settings, "RB x3, RT");
    CHECK(a.settingsPage == App::SettingsPage::Controllers);
    runFor(300);
    screenshot("");
    press(pad, SDL_GAMEPAD_BUTTON_SOUTH);  // the focus starts on the pad's Test button
    std::printf("  A on Test: test %s\n", a.padTestActive ? "running ok" : "NOT running");
    CHECK(a.padTestActive);
    CHECK(a.padTestId == pad.id);
    press(pad, SDL_GAMEPAD_BUTTON_EAST, 150);
    std::printf("  short B: test %s\n", a.padTestActive ? "still running ok" : "STOPPED");
    CHECK(a.padTestActive);
    expectScreen(Screen::Settings, "short B does not navigate");
    CHECK(a.settingsPage == App::SettingsPage::Controllers);
    if (!png.empty()) {
        SDL_SetJoystickVirtualAxis(pad.js, SDL_GAMEPAD_AXIS_LEFTX, -14000);
        SDL_SetJoystickVirtualAxis(pad.js, SDL_GAMEPAD_AXIS_LEFTY, 4000);
        SDL_SetJoystickVirtualAxis(pad.js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);  // half way
        SDL_SetJoystickVirtualButton(pad.js, SDL_GAMEPAD_BUTTON_SOUTH, true);
        runFor(200);
        screenshot("-test");
        SDL_SetJoystickVirtualAxis(pad.js, SDL_GAMEPAD_AXIS_LEFTX, 0);
        SDL_SetJoystickVirtualAxis(pad.js, SDL_GAMEPAD_AXIS_LEFTY, 0);
        SDL_SetJoystickVirtualAxis(pad.js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
        SDL_SetJoystickVirtualButton(pad.js, SDL_GAMEPAD_BUTTON_SOUTH, false);
        runFor(200);
        CHECK(a.padTestActive);  // the pad drove nothing
        expectScreen(Screen::Settings, "pad used during the test");
    }
    press(pad, SDL_GAMEPAD_BUTTON_EAST, 1100);
    std::printf("  B held 1.1 s: test %s\n", a.padTestActive ? "STILL RUNNING" : "stopped ok");
    CHECK(!a.padTestActive);
    runFor(300);
    expectScreen(Screen::Settings, "after the test");
    CHECK(a.settingsPage == App::SettingsPage::Controllers);
    // Leaving the page another way (here as a mouse click on a tab would) ends the test too.
    press(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(a.padTestActive);
    a.screen = Screen::Library;
    runFor(200);
    std::printf("  test running, page left: test %s\n", a.padTestActive ? "STILL RUNNING" : "stopped ok");
    CHECK(!a.padTestActive);
    press(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    expectScreen(Screen::AddGames, "the pad drives the menus again");

    std::printf("6. plugging in and out\n");
    VirtualPad second2 = attach("Marathon UI Test DualSense", 0x054C, 0x0CE6);
    runFor(300);
    const bool connected = toastContains("Controller connected: Marathon UI Test DualSense");
    std::printf("  connected toast: %s\n", connected ? "yes" : "NO");
    CHECK(connected);
    CHECK(toastContains("(Player 2)"));
    CHECK(a.padSnapshot().size() == 2);
    detach(second2);
    runFor(2300);  // (a second message for the same pad waits until 2 s after the first)
    const bool disconnected = toastContains("Controller disconnected: Marathon UI Test DualSense");
    std::printf("  disconnected toast: %s\n", disconnected ? "yes" : "NO");
    CHECK(disconnected);
    CHECK(a.padSnapshot().size() == 1);
    CHECK(!launchedSomething());

    if (!ingame.empty()) {
        std::printf("7. in a game inside Marathon\n");
        // The game's files and the cores, as Marathon would have them after a scan and an install.
        ScanHit hit;
        const bool known = identifyPath(a.catalog, ingame, hit) && !hit.gameId.empty();
        std::printf("  %s: %s\n", ingame.c_str(), known ? hit.gameId.c_str() : "NOT RECOGNISED");
        CHECK(known);
        for (auto& def : a.catalog.backends()) {
            if (def.kind != "libretro" || def.core.empty()) continue;
            for (const char* ext : {".dylib", ".dll", ".so"}) {
                const std::filesystem::path core = utf8ToPath(coresDir) / utf8ToPath(def.core + "_libretro" + ext);
                std::error_code ec2;
                if (!std::filesystem::exists(core, ec2)) continue;
                BackendInstall bi;
                bi.backendId = def.id;
                bi.version = "test";
                bi.root = coresDir;
                bi.executable = pathToUtf8(core);
                bi.userProvided = true;
                a.library.setInstall(bi);
                break;
            }
        }
        const GameDef* game = known ? a.catalog.game(hit.gameId) : nullptr;
        if (game) {
            a.acceptHit(hit);
            a.library.save();
            // Back to the library with its card selected, then Start races.
            press(pad, SDL_GAMEPAD_BUTTON_EAST);
            press(pad, SDL_GAMEPAD_BUTTON_EAST);
            a.selectedGame = game->id;
            press(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            press(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            expectScreen(Screen::Library, "back on the library");
            runFor(200);
            std::printf("  focused: %s, status %s\n", a.focusedGame.c_str(), a.statusText(*game).c_str());
            CHECK(a.focusedGame == game->id);
            CHECK(a.statusOf(*game) == App::GameStatus::Ready);
            press(pad, SDL_GAMEPAD_BUTTON_START);
            runFor(3000);
            expectScreen(Screen::Playing, "Start races");
            CHECK(a.host().running());
            press(pad, SDL_GAMEPAD_BUTTON_GUIDE, 300);  // (SDL reports a Guide release 250 ms after the press at the earliest)
            std::printf("  Guide alone: pause menu %s\n", ui::playerMenuOpen() ? "open ok" : "CLOSED");
            CHECK(ui::playerMenuOpen());
            checkPlaybackControls(a);
            screenshot("-pause");
            press(pad, SDL_GAMEPAD_BUTTON_EAST);
            std::printf("  B: pause menu %s\n", ui::playerMenuOpen() ? "STILL OPEN" : "closed ok");
            CHECK(!ui::playerMenuOpen());
            // Player 1's controller unplugged: paused, with a message; then plugged in again.
            detach(pad);
            runFor(300);
            std::printf("  Player 1 unplugged: pause menu %s, message %s\n", ui::playerMenuOpen() ? "open ok" : "CLOSED",
                        toastContains("Player 1's controller was disconnected") ? "ok" : "MISSING");
            CHECK(ui::playerMenuOpen());
            CHECK(toastContains("Player 1's controller was disconnected"));
            pad = attach("Marathon UI Test Pad", 0x045E, 0x0B12);
            runFor(500);
            press(pad, SDL_GAMEPAD_BUTTON_EAST);
            CHECK(!ui::playerMenuOpen());
            // Guide + Start held: the ring fills, the menu does not flicker, Marathon comes back.
            SDL_SetJoystickVirtualButton(pad.js, SDL_GAMEPAD_BUTTON_GUIDE, true);
            runFor(100);
            SDL_SetJoystickVirtualButton(pad.js, SDL_GAMEPAD_BUTTON_START, true);
            runFor(600);
            const float progress = a.gamepads().chordProgress(SDL_GetTicks());
            std::printf("  holding Guide + Start: %.0f %%\n", progress * 100);
            CHECK(progress > 0.25f && progress < 1.0f);
            CHECK(a.screen == Screen::Playing);
            screenshot("-chord");
            runFor(700);
            SDL_SetJoystickVirtualButton(pad.js, SDL_GAMEPAD_BUTTON_START, false);
            SDL_SetJoystickVirtualButton(pad.js, SDL_GAMEPAD_BUTTON_GUIDE, false);
            runFor(300);
            expectScreen(Screen::Game, "Guide + Start held 1.3 s quits");
            CHECK(!a.host().running());
            CHECK(!ui::playerMenuOpen());
        }
    }

    detach(pad);
    a.shutdown();
    std::error_code ec;
    if (!keep) std::filesystem::remove_all(data, ec);
    if (!keep && ownHome) std::filesystem::remove_all(tmpHome, ec);
    std::printf("ui_pad_tool: %d checks, %d failures%s\n", g_checks, g_failures, keep ? (" (data in " + data + ")").c_str() : "");
    return g_failures ? 1 : 0;
}
