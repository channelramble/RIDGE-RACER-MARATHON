// Smoke test for the in-process libretro host: opens an SDL window with an OpenGL
// context, runs a core for N presented frames, saves a PNG of the output, prints stats.
//
// lr_smoke <core> <content> --system <dir> --save <dir> [--frames N] [--seconds S] [--core-frames N]
//          [--png out.png] [--opt key=value]... [--hidden] [--vsync 0|1] [--speed P]
//          [--turbo P] [--aspect A] [--filter nearest|linear] [--integer] [--size WxH]
//          [--press FRAME:BUTTON[:COUNT]]... [--cycles N] [--state file] [--list-options] [--debug]
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "libretro.h"
#include "libretro/GlFunctions.h"
#include "libretro/Host.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_WARNINGS
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "stb/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

struct Press {
    int frame = 0, count = 6;
    unsigned button = 0;
};

int buttonId(const std::string& name) {
    static const struct {
        const char* name;
        unsigned id;
    } kNames[] = {{"b", RETRO_DEVICE_ID_JOYPAD_B},         {"y", RETRO_DEVICE_ID_JOYPAD_Y},
                  {"select", RETRO_DEVICE_ID_JOYPAD_SELECT}, {"start", RETRO_DEVICE_ID_JOYPAD_START},
                  {"up", RETRO_DEVICE_ID_JOYPAD_UP},         {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
                  {"left", RETRO_DEVICE_ID_JOYPAD_LEFT},     {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
                  {"a", RETRO_DEVICE_ID_JOYPAD_A},           {"x", RETRO_DEVICE_ID_JOYPAD_X},
                  {"l", RETRO_DEVICE_ID_JOYPAD_L},           {"r", RETRO_DEVICE_ID_JOYPAD_R},
                  {"l2", RETRO_DEVICE_ID_JOYPAD_L2},         {"r2", RETRO_DEVICE_ID_JOYPAD_R2},
                  {"l3", RETRO_DEVICE_ID_JOYPAD_L3},         {"r3", RETRO_DEVICE_ID_JOYPAD_R3}};
    for (const auto& n : kNames) {
        if (name == n.name) return static_cast<int>(n.id);
    }
    return -1;
}

// Plain stderr logging (the default NSLog output on macOS is rate limited and drops lines).
void SDLCALL logToStderr(void*, int category, SDL_LogPriority priority, const char* message) {
    static const char* kPrio[] = {"", "TRACE", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"};
    const int p = static_cast<int>(priority);
    std::fprintf(stderr, "%s [%d] %s\n", p >= 0 && p < 8 ? kPrio[p] : "?", category, message);
}

int usage() {
    std::fprintf(stderr,
                 "usage: lr_smoke <core> <content> --system <dir> --save <dir> [--frames N] [--seconds S] [--core-frames N]\n"
                 "       [--png out.png] [--opt key=value]... [--hidden] [--vsync 0|1] [--speed P] [--turbo P]\n"
                 "       [--aspect A] [--filter nearest|linear] [--integer] [--size WxH]\n"
                 "       [--press FRAME:BUTTON[:COUNT]]... [--cycles N] [--state file] [--list-options] [--debug]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    SDL_SetLogOutputFunction(logToStderr, nullptr);
    rrm::lr::HostConfig cfg;
    cfg.corePath = argv[1];
    cfg.contentPath = argv[2];
    int frames = 600, cycles = 1, width = 1280, height = 720;
    unsigned long long coreFrames = 0;
    double seconds = 0;
    int turbo = -1;
    bool hidden = false, listOptions = false, vsync = true;
    std::string png, statePath;
    std::vector<Press> presses;

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--system") cfg.systemDir = next();
        else if (a == "--save") cfg.saveDir = next();
        else if (a == "--frames") frames = std::atoi(next().c_str());
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--core-frames") coreFrames = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--png") png = next();
        else if (a == "--hidden") hidden = true;
        else if (a == "--vsync") vsync = std::atoi(next().c_str()) != 0;
        else if (a == "--speed") cfg.speedPercent = std::atoi(next().c_str());
        else if (a == "--turbo") turbo = std::atoi(next().c_str());
        else if (a == "--aspect") cfg.aspect = next();
        else if (a == "--filter") cfg.filter = next();
        else if (a == "--integer") cfg.integerScale = true;
        else if (a == "--cycles") cycles = std::max(1, std::atoi(next().c_str()));
        else if (a == "--state") statePath = next();
        else if (a == "--list-options") listOptions = true;
        else if (a == "--debug") SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &width, &height);
        else if (a == "--opt") {
            std::string kv = next();
            size_t eq = kv.find('=');
            if (eq == std::string::npos) return usage();
            cfg.coreOptions[kv.substr(0, eq)] = kv.substr(eq + 1);
        } else if (a == "--press") {
            std::string spec = next();
            char name[32] = {};
            Press p;
            int n = std::sscanf(spec.c_str(), "%d:%31[a-z0-9]:%d", &p.frame, name, &p.count);
            int id = n >= 2 ? buttonId(name) : -1;
            if (id < 0) return usage();
            p.button = static_cast<unsigned>(id);
            presses.push_back(p);
        } else {
            return usage();
        }
    }
    cfg.vsync = vsync;
    if (turbo >= 0) cfg.turboPercent = turbo;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (hidden) flags |= SDL_WINDOW_HIDDEN;
    SDL_Window* window = SDL_CreateWindow("lr_smoke", width, height, flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        std::fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, context);
    SDL_GL_SetSwapInterval(vsync ? 1 : 0);
    rrm::lr::GlFunctions gl;
    std::string missing;
    if (!gl.load(missing)) {
        std::fprintf(stderr, "GL function missing: %s\n", missing.c_str());
        return 1;
    }
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    std::printf("GL: %s | %s | display %.2f Hz | vsync %s\n", reinterpret_cast<const char*>(gl.GetString(GL_VERSION)),
                reinterpret_cast<const char*>(gl.GetString(GL_RENDERER)), mode ? mode->refresh_rate : 0.0f,
                vsync ? "on" : "off");

    int exitCode = 0;
    bool quit = false;
    for (int cycle = 0; cycle < cycles && !quit; ++cycle) {
        rrm::lr::Host host;
        std::string error;
        const uint64_t t0 = SDL_GetTicksNS();
        if (!host.start(window, cfg, error)) {
            std::fprintf(stderr, "start failed: %s\n", error.c_str());
            exitCode = 1;
            break;
        }
        std::printf("cycle %d: started in %.2f s\n", cycle + 1, (SDL_GetTicksNS() - t0) * 1e-9);
        if (listOptions) std::printf("%s\n", host.coreOptionDefinitions().dump(2).c_str());
        if (turbo >= 0) host.setTurbo(true);

        const uint64_t runStart = SDL_GetTicksNS();
        int presented = 0;
        bool coreQuit = false;
        while (!quit) {
            if (coreFrames ? host.stats().framesRun >= coreFrames
                           : seconds > 0 ? (SDL_GetTicksNS() - runStart) * 1e-9 >= seconds : presented >= frames)
                break;
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) quit = true;
                host.handleEvent(e);
            }
            rrm::lr::PadState pad;
            for (const auto& p : presses) {
                if (presented >= p.frame && presented < p.frame + p.count) pad.buttons |= 1u << p.button;
            }
            host.setVirtualPad(pad);
            if (!host.runFrame()) {
                coreQuit = true;
                break;
            }
            int fbw = 0, fbh = 0;
            SDL_GetWindowSizeInPixels(window, &fbw, &fbh);
            gl.Viewport(0, 0, fbw, fbh);
            gl.ClearColor(0.1f, 0.1f, 0.12f, 1.0f);
            gl.Clear(GL_COLOR_BUFFER_BIT);
            host.render(0, 0, fbw, fbh);
            SDL_GL_SwapWindow(window);
            ++presented;
        }
        const double wall = (SDL_GetTicksNS() - runStart) * 1e-9;
        rrm::lr::HostStats s = host.stats();
        std::printf("core: %s %s | hw render: %s | core fps %.3f | rate %.0f Hz | frame %dx%d | aspect %.3f\n",
                    s.coreName.c_str(), s.coreVersion.c_str(), s.hwRender ? "yes" : "no", s.coreFps, s.sampleRate,
                    s.baseWidth, s.baseHeight, host.displayAspect());
        std::printf("presented %d frames, emulated %llu frames in %.2f s -> %.1f emulated fps (last window: %.1f "
                    "emu fps, %.1f present fps)%s\n",
                    presented, static_cast<unsigned long long>(s.framesRun), wall, s.framesRun / wall,
                    s.measuredFps, s.presentFps, coreQuit ? " [core requested shutdown]" : "");

        if (!png.empty()) {
            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            if (host.captureFrame(rgba, w, h)) {
                std::string path = png;
                if (cycles > 1) {
                    size_t dot = path.rfind('.');
                    path.insert(dot == std::string::npos ? path.size() : dot, "_" + std::to_string(cycle + 1));
                }
                double sum = 0;
                size_t lit = 0;
                for (size_t i = 0; i < rgba.size(); i += 4) {
                    int l = rgba[i] + rgba[i + 1] + rgba[i + 2];
                    sum += l;
                    if (l > 24) ++lit;
                }
                const size_t pixels = rgba.size() / 4;
                if (stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4))
                    std::printf("wrote %s (%dx%d, mean luma %.1f, %.1f%% non-black)\n", path.c_str(), w, h,
                                sum / (3.0 * pixels), 100.0 * lit / pixels);
                else
                    std::fprintf(stderr, "cannot write %s\n", path.c_str());
            } else {
                std::fprintf(stderr, "captureFrame failed (no frame yet?)\n");
                exitCode = 1;
            }
        }
        if (!statePath.empty()) {
            bool saved = host.saveState(statePath);
            bool loaded = saved && host.loadState(statePath);
            for (int i = 0; loaded && i < 30; ++i) host.runFrame();  // keeps running after a load
            std::printf("savestate %s: save %s, load %s\n", statePath.c_str(), saved ? "ok" : "FAILED",
                        loaded ? "ok" : "FAILED");
            if (!saved || !loaded) exitCode = 1;
        }
        host.stop();
    }

    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
