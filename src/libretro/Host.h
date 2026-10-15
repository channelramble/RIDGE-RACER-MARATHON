// In-process libretro host. Runs PS1 (SwanStation / PCSX-ReARMed / Beetle),
// PSP (PPSSPP) and arcade (MAME) cores inside Marathon's own window so the
// same code path works on desktop, Android and iOS (where launching other
// emulators is impossible).
//
// libretro is a C API with global callbacks, so only one Host may be active.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/Json.h"

namespace rrm::lr {

struct HostConfig {
    std::string corePath;      // .dylib/.so/.dll; on iOS a bundled framework binary
    std::string contentPath;   // game file (cue/iso/zip/romset dir resolved by the backend)
    std::string systemDir;     // BIOS / core system files
    std::string saveDir;       // memory cards / NVRAM
    json coreOptions = json::object();  // "core_option_key": "value"
    int speedPercent = 100;    // base speed (100 = authentic)
    int turboPercent = 300;    // fast-forward speed; 0 = as fast as possible
    std::string aspect = "auto";   // auto|4:3|16:9|21:9|fit|stretch
    std::string filter = "nearest"; // nearest|linear (final scaling)
    bool vsync = true;
    bool integerScale = false;
    // Above the core's frame rate (e.g. 60 fps content on a 144 Hz display), show in-between images
    // instead of repeating each core frame: 0 = off, 1 = crossfade, 2 = motion-compensated
    // interpolation. Smoother motion with up to one frame of extra latency; game speed is unchanged.
    int smoothing = 0;
    // At most this many distinct images per second (0 = one per display refresh).
    int smoothFps = 0;
};

struct HostStats {
    double coreFps = 0;        // core's nominal fps (av_info)
    double sampleRate = 0;
    double measuredFps = 0;    // emulated frames per wall second
    double presentFps = 0;     // presented frames per wall second
    int baseWidth = 0, baseHeight = 0;  // last frame size from the core
    bool hwRender = false;
    std::string coreName, coreVersion;
    uint64_t framesRun = 0;    // core frames emulated since start()
};

// Buttons for virtual (touch) and remapped input, RETRO_DEVICE_ID_JOYPAD_* bit order.
struct PadState {
    uint32_t buttons = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;   // analog sticks, -32768..32767
};

class Host {
public:
    Host();
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // The window must own a current OpenGL (desktop 3.2+ core or GLES 3.0)
    // context created by the app; the core renders into an FBO of ours.
    bool start(SDL_Window* window, const HostConfig& config, std::string& error);
    void stop();  // call before destroying the GL context (unloads the core completely)
    bool running() const;

    // Feed SDL events (gamepads, keyboard). Returns true if consumed.
    bool handleEvent(const SDL_Event& e);
    // Merges on-screen controls with physical input for port 0.
    void setVirtualPad(const PadState& pad);
    // What the core reads from input_state right now (tests; RETRO_DEVICE_* arguments).
    int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) const;
    // The gamepad playing on a port (0-based; 0 = none), e.g. for the pause menu's player names.
    SDL_JoystickID portGamepad(unsigned port) const;

    // Advances emulation for one presented frame (handles pacing, base speed
    // and turbo). Returns false when the core asked to shut down.
    bool runFrame();
    // Tests: runs exactly one core frame, without pacing.
    void stepFrame();
    // Draws the latest frame into the given rectangle of the default framebuffer.
    // x/y/w/h are drawable pixels (SDL_GetWindowSizeInPixels), origin top-left like
    // SDL/ImGui; the rectangle is cleared to black and the image letterboxed inside.
    void render(int x, int y, int w, int h);

    void setPaused(bool paused);
    bool paused() const;
    void setTurbo(bool on);
    bool turbo() const;
    void setSpeedPercent(int percent);
    void setTurboPercent(int percent);
    void reset();

    bool saveState(const std::string& path);
    bool loadState(const std::string& path);
    // Changes a core option at runtime (core sees it via GET_VARIABLE_UPDATE).
    void setCoreOption(const std::string& key, const std::string& value);
    // Core-declared options (key -> {desc, values[], value}) for an advanced UI.
    json coreOptionDefinitions() const;

    HostStats stats() const;
    std::string lastError() const;
    // Display aspect ratio of the current frame (after config.aspect override).
    float displayAspect() const;
    // Reads back the last presented frame (RGBA8, top-down) for screenshots/tests.
    bool captureFrame(std::vector<uint8_t>& rgba, int& w, int& h);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rrm::lr
