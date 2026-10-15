// Drives the real backend adapters without the UI, against a throwaway data folder.
//
//   launch_tool matrix <games folder> [options]
//       Scans the folder, then for every recognised game and every backend option on
//       this OS calls Backend::launch() in dry-run mode and prints what would start:
//       the command line (external programs) or the core + core options (in-app cores).
//   launch_tool run <games folder> <game id> <backend id> [options] [--png out.png]
//       Launches one in-app core for real in a hidden window, plays --core-frames
//       frames (pressing Start/Cross periodically to get past menus) and saves a PNG.
//
// Options:
//   --catalog DIR       catalog folder (default: ./catalog)
//   --data DIR          Marathon data folder to use (default: a fresh temp folder); extra catalog
//                       files in DIR/catalog/*.json are merged as in Marathon (docs/CATALOG.md)
//   --cores DIR         link libretro cores found here (<core>_libretro.<ext>) instead of downloading
//   --link ID=PATH      link an existing install for backend ID (folder or executable)
//   --install ID        run backend ID's real installer (downloads) before launching
//   --set KEY=VALUE     a GameSettings field, e.g. --set aspect=32:9 --set resolution=4k (extra.KEY for extras)
//   --controller KEY=VALUE  a controller setting (docs/CONTROLLERS.md): steering=stick|dpad,
//                       pedals=triggers|buttons, vibration=true|false, deadzone=0..40, quitChord, quitHoldMs,
//                       playerN="" | none | <GUID> (N = 1..4)
//   --pad NAME[:GUID[:TYPE]]  a connected controller, repeatable, in connection order (players are
//                       assigned as in Marathon); none by default
//   --display WxH@HZ    display to pretend Marathon is on (default 3840x2160@144)
//   --only GAME_ID      matrix: just this game
//   --core-frames N     run: core frames to emulate (default 3600)
//   --size WxH          run: window size (default 1280x720)
//   --realtime          run: play at the configured speed with vsync instead of fast-forwarding
//   --vpad              run: input test mode. An SDL virtual gamepad (Xbox ids) is plugged in before
//                       the core starts and does the playing: Start and A (Cross) get past menus;
//                       from core frame 2700 RT is held at 60 % and the left stick sweeps left and
//                       right. The log goes to stderr ("[libretro] port 1 device ...", the pad's
//                       port), and at the end the tool prints what the core reads from port 1 and
//                       how often it asked the pad to vibrate.
//   --eval-interp DIR   run: afterwards, step frame by frame and score frame interpolation: for
//                       consecutive distinct frames F0 F1 F2, the midpoint generated from F0 and F2
//                       is compared with the real F1 (PSNR), next to a plain blend and repeating F0.
//                       PNGs of the first triplet go to DIR. --eval-count N triplets (default 6).
//   --spawn SECONDS     matrix: really start external programs, screenshot the screen after
//                       SECONDS (to --png, macOS screencapture) and close them again
#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "backends/Backend.h"
#include "backends/EmuCommon.h"
#include "core/Catalog.h"
#include "core/Controllers.h"
#include "core/Library.h"
#include "core/Platform.h"
#include "core/Process.h"
#include "core/Scanner.h"
#include "libretro.h"
#include "libretro/GlFunctions.h"
#include "libretro/Host.h"
#include "libretro/Interpolator.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_WARNINGS
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "stb/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

using namespace rrm;

namespace {

struct Options {
    std::string mode, folder, gameId, backendId, catalogDir = "catalog", dataDir, coresDir, png, only;
    std::map<std::string, std::string> links;
    std::vector<std::string> installs;
    json settings = json::object();
    json controllers = json::object();
    std::vector<std::string> pads;
    DisplayInfo display{3840, 2160, 144, 1.0f};
    unsigned long long coreFrames = 3600;
    int width = 1280, height = 720;
    int spawnSeconds = 0;
    bool realtime = false;
    bool vpad = false;
    std::string evalDir;
    int evalCount = 6;
};

std::string readFile(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int usage() {
    std::fprintf(stderr,
                 "usage: launch_tool matrix <games folder> [--catalog DIR] [--data DIR] [--cores DIR] [--link ID=PATH]...\n"
                 "                   [--set KEY=VALUE]... [--controller KEY=VALUE]... [--pad NAME[:GUID[:TYPE]]]...\n"
                 "                   [--display WxH@HZ] [--only GAME_ID]\n"
                 "       launch_tool run <games folder> <game id> <backend id> [same options] [--png out.png]\n"
                 "                   [--core-frames N] [--size WxH] [--vpad]\n");
    return 2;
}

// --controller KEY=VALUE into ControllerSettings JSON (fromJson validates it).
void setControllerField(json& c, const std::string& k, const std::string& v) {
    if (k.size() == 7 && k.rfind("player", 0) == 0 && k[6] >= '1' && k[6] <= '4') {
        if (!c.contains("players")) c["players"] = json::array({json::object(), json::object(), json::object(), json::object()});
        c["players"][static_cast<size_t>(k[6] - '1')]["controller"] = v;
    } else if (v == "true" || v == "false") {
        c[k] = v == "true";
    } else if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) {
        c[k] = std::stoi(v);
    } else {
        c[k] = v;
    }
}

// --pad NAME[:GUID[:TYPE]] specs as connected pads, numbered and given player slots like Marathon's.
std::vector<ConnectedPad> padsFrom(const std::vector<std::string>& specs, const ControllerSettings& c) {
    std::vector<ConnectedPad> pads;
    for (auto& spec : specs) {
        ConnectedPad p;
        p.id = static_cast<uint32_t>(pads.size() + 1);
        const size_t a = spec.find(':');
        p.name = spec.substr(0, a);
        if (a != std::string::npos) {
            const size_t b = spec.find(':', a + 1);
            p.guid = spec.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            if (b != std::string::npos) p.type = spec.substr(b + 1);
        }
        if (p.type.empty()) p.type = "standard";
        for (auto& q : pads)
            if (q.name == p.name) ++p.nameIndex;
        pads.push_back(p);
    }
    applyAssignment(pads, assignPlayers(pads, c.players));
    return pads;
}

bool parseArgs(int argc, char** argv, Options& o) {
    if (argc < 3) return false;
    o.mode = argv[1];
    o.folder = argv[2];
    int i = 3;
    if (o.mode == "run") {
        if (argc < 5) return false;
        o.gameId = argv[3];
        o.backendId = argv[4];
        i = 5;
    } else if (o.mode != "matrix") {
        return false;
    }
    for (; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        auto kv = [&](std::string s, std::string& k, std::string& v) {
            size_t eq = s.find('=');
            if (eq == std::string::npos) return false;
            k = s.substr(0, eq);
            v = s.substr(eq + 1);
            return true;
        };
        std::string k, v;
        if (a == "--catalog") o.catalogDir = next();
        else if (a == "--data") o.dataDir = next();
        else if (a == "--cores") o.coresDir = next();
        else if (a == "--png") o.png = next();
        else if (a == "--only") o.only = next();
        else if (a == "--spawn") o.spawnSeconds = std::atoi(next().c_str());
        else if (a == "--realtime") o.realtime = true;
        else if (a == "--vpad") o.vpad = true;
        else if (a == "--eval-interp") o.evalDir = next();
        else if (a == "--eval-count") o.evalCount = std::max(1, std::atoi(next().c_str()));
        else if (a == "--core-frames") o.coreFrames = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &o.width, &o.height);
        else if (a == "--display") std::sscanf(next().c_str(), "%dx%d@%d", &o.display.width, &o.display.height, &o.display.refreshHz);
        else if (a == "--link" && kv(next(), k, v)) o.links[k] = v;
        else if (a == "--install") o.installs.push_back(next());
        else if (a == "--controller" && kv(next(), k, v)) setControllerField(o.controllers, k, v);
        else if (a == "--pad") o.pads.push_back(next());
        else if (a == "--set" && kv(next(), k, v)) {
            // Numbers and booleans as JSON, everything else as a string.
            // "extra.<key>" goes into GameSettings::extra.
            json& target = k.rfind("extra.", 0) == 0 ? o.settings["extra"][k.substr(6)] : o.settings[k];
            if (v == "true" || v == "false") target = v == "true";
            else if ((k == "speed" || k == "turbo" || k == "customFps" || k.rfind("extra.", 0) == 0) && !v.empty() &&
                     v.find_first_not_of("0123456789") == std::string::npos)
                target = std::stoi(v);  // the only numeric fields; "fps=120" stays a string
            else target = v;
        } else return false;
    }
    return true;
}

// Links a core from --cores for every libretro backend (no downloads in tests).
void linkCores(const Catalog& catalog, Library& lib, const std::string& coresDir) {
    if (coresDir.empty()) return;
    for (auto& def : catalog.backends()) {
        if (def.kind != "libretro" || def.core.empty()) continue;
        for (const char* ext : {".dylib", ".dll", ".so"}) {
            fs::path p = utf8ToPath(coresDir) / utf8ToPath(def.core + "_libretro" + ext);
            std::error_code ec;
            if (!fs::exists(p, ec)) continue;
            BackendInstall bi;
            bi.backendId = def.id;
            bi.version = "test";
            bi.root = coresDir;
            bi.executable = pathToUtf8(p);
            bi.userProvided = true;
            lib.setInstall(bi);
            break;
        }
    }
}

std::string describe(const LaunchResult& r) {
    std::string out;
    if (!r.ok) return "FAILED: " + r.error;
    switch (r.mode) {
        case LaunchResult::Mode::ExternalProcess: out = "process:"; break;
        case LaunchResult::Mode::InProcessCore: out = "in-app core " + r.corePath + "\n      content " + r.contentPath; break;
        case LaunchResult::Mode::ExternalApp: out = "android app:"; break;
    }
    for (auto& a : r.commandLine) out += "\n      " + a;
    if (r.mode == LaunchResult::Mode::InProcessCore) {
        out += "\n      present aspect: " + (r.presentAspect.empty() ? std::string("(settings)") : r.presentAspect);
        for (auto& [k, v] : r.coreOptions.items()) out += "\n      " + k + " = " + v.get<std::string>();
    }
    return out;
}

const char* stateName(InstallState s) {
    switch (s) {
        case InstallState::NotInstalled: return "not installed";
        case InstallState::Installed: return "installed";
        case InstallState::Unavailable: return "unavailable";
        case InstallState::UserProvided: return "linked";
    }
    return "?";
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double se = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (i % 4 == 3) continue;
        const double d = double(a[i]) - double(b[i]);
        se += d * d;
        ++n;
    }
    const double mse = n ? se / n : 0;
    return mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

void savePng(const std::string& dir, const std::string& name, const std::vector<uint8_t>& px, int w, int h) {
    const std::string path = dir + "/" + name + ".png";
    stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4);
}

// See --eval-interp in the header comment.
void evaluateInterpolation(const Options& o, lr::Host& host, int pw, int ph) {
    using namespace rrm::lr;
    GlFunctions gl;
    std::string missing, error;
    if (!gl.load(missing)) {
        std::fprintf(stderr, "eval: GL function %s missing\n", missing.c_str());
        return;
    }
    Interpolator interp;
    if (!interp.init(&gl, false, error)) {
        std::fprintf(stderr, "eval: interpolator: %s\n", error.c_str());
        return;
    }
    fs::create_directories(utf8ToPath(o.evalDir));
    auto grab = [&](std::vector<uint8_t>& px, int& w, int& h) {
        host.render(0, 0, pw, ph);
        return host.captureFrame(px, w, h);
    };
    // The next image that differs from `last` (games drawing at 30 fps repeat every frame).
    auto nextDistinct = [&](const std::vector<uint8_t>& last, std::vector<uint8_t>& px, int& w, int& h) {
        for (int i = 0; i < 8; ++i) {
            host.stepFrame();
            if (grab(px, w, h) && px != last) return true;
        }
        return false;
    };
    auto upload = [&](GLuint tex, const std::vector<uint8_t>& px, int w, int h) {
        gl.BindTexture(GL_TEXTURE_2D, tex);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    GLuint tex[4] = {};
    gl.GenTextures(4, tex);
    GLuint fbo = 0;
    gl.GenFramebuffers(1, &fbo);
    double sumI = 0, sumB = 0, sumR = 0;
    int done = 0;
    std::vector<uint8_t> f0, f1, f2, out;
    int w = 0, h = 0;
    for (int k = 0; k < o.evalCount; ++k) {
        for (int i = 0; i < 40; ++i) host.stepFrame();  // spread the samples out
        if (!grab(f0, w, h) || !nextDistinct(f0, f1, w, h) || !nextDistinct(f1, f2, w, h)) break;
        upload(tex[0], f0, w, h);
        upload(tex[2], f2, w, h);
        upload(tex[3], f0, w, h);  // output target, same size
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[3], 0);
        const auto t0 = SDL_GetTicksNS();
        interp.estimate(tex[0], tex[2], w, h);
        interp.draw(fbo, 0, 0, w, h, tex[0], tex[2], 0.5f);
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        out.assign(static_cast<size_t>(w) * h * 4, 0);
        gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        const double ms = (SDL_GetTicksNS() - t0) / 1e6;
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        std::vector<uint8_t> blend(f0.size());
        for (size_t i = 0; i < blend.size(); ++i) blend[i] = static_cast<uint8_t>((f0[i] + f2[i] + 1) / 2);
        const double pI = psnr(out, f1), pB = psnr(blend, f1), pR = psnr(f0, f1);
        std::printf("eval %d: %dx%d  interpolated %.2f dB  blend %.2f dB  repeat %.2f dB  (estimate+draw+readback %.1f ms)\n",
                    k, w, h, pI, pB, pR, ms);
        sumI += pI;
        sumB += pB;
        sumR += pR;
        ++done;
        if (k == 0) {
            savePng(o.evalDir, "f0", f0, w, h);
            savePng(o.evalDir, "f1-real", f1, w, h);
            savePng(o.evalDir, "f2", f2, w, h);
            savePng(o.evalDir, "f1-interpolated", out, w, h);
            savePng(o.evalDir, "f1-blend", blend, w, h);
        }
    }
    if (done)
        std::printf("eval mean over %d: interpolated %.2f dB  blend %.2f dB  repeat %.2f dB\n", done, sumI / done,
                    sumB / done, sumR / done);
    gl.DeleteFramebuffers(1, &fbo);
    gl.DeleteTextures(4, tex);
    interp.shutdown();
}

// --vpad: the virtual gamepad and what it was asked to do.
struct TestPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* js = nullptr;
    int rumbles = 0;
    static bool SDLCALL onRumble(void* user, Uint16 low, Uint16 high) {
        if (low || high) ++static_cast<TestPad*>(user)->rumbles;
        return true;
    }
    bool attach() {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.vendor_id = 0x045E;
        desc.product_id = 0x0B12;
        desc.name = "launch_tool virtual pad";
        desc.userdata = this;
        desc.Rumble = onRumble;
        id = SDL_AttachVirtualJoystick(&desc);
        js = id ? SDL_OpenJoystick(id) : nullptr;
        return js != nullptr;
    }
    // Same script for every game: menus first, then throttle and steering.
    void drive(unsigned long long f) {
        const unsigned long long t = f % 150;
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, f > 900 && t < 8);
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_SOUTH, f > 900 && t >= 75 && t < 83);
        const bool racing = f > 2700;
        const int rt = racing ? static_cast<int>(0.6 * 32767) : 0;  // virtual triggers span the whole axis
        SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, static_cast<Sint16>(rt * 2 - 32768));
        SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
        SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX,
                                   static_cast<Sint16>(racing ? 16000 * std::sin(static_cast<double>(f) / 120.0) : 0));
        SDL_UpdateJoysticks();
    }
};

void SDLCALL logToStderr(void*, int, SDL_LogPriority priority, const char* message) {
    std::fprintf(stderr, "%s %s\n", priority >= SDL_LOG_PRIORITY_WARN ? "WARN" : "INFO", message);
}

int runCore(const Options& o, const LaunchResult& r, const GameSettings& s) {
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (o.vpad) {
        // As in Marathon: pads are read while the (here hidden) window has no focus.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetLogOutputFunction(logToStderr, nullptr);
        SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
        SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_INFO);
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    TestPad testPad;
    if (o.vpad && !testPad.attach()) {
        std::fprintf(stderr, "virtual gamepad: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_Window* win = SDL_CreateWindow("launch_tool", o.width, o.height, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx) {
        std::fprintf(stderr, "GL context: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(o.realtime ? 1 : 0);

    lr::HostConfig cfg;
    cfg.corePath = r.corePath;
    cfg.contentPath = r.contentPath;
    cfg.systemDir = r.systemDir;
    cfg.saveDir = r.saveDir;
    cfg.coreOptions = r.coreOptions;
    cfg.aspect = r.presentAspect.empty() ? s.aspect : r.presentAspect;
    cfg.filter = s.filter == "linear" ? "linear" : "nearest";
    cfg.vsync = o.realtime;
    cfg.speedPercent = s.speed;
    cfg.turboPercent = 0;
    cfg.smoothing = resolveFps(s) == 0 ? 0 : s.fpsMethod == "blend" ? 1 : 2;
    cfg.smoothFps = std::max(0, resolveFps(s));
    lr::Host host;
    std::string error;
    if (!host.start(win, cfg, error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    host.setTurbo(!o.realtime);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    // Start then Cross every 2.5 s of game time (after 15 s) walks most menus into a race.
    unsigned long long presented = 0;
    while (host.stats().framesRun < o.coreFrames) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) host.handleEvent(e);
        const unsigned long long f = host.stats().framesRun;
        lr::PadState pad;
        if (o.vpad) {
            testPad.drive(f);
        } else {
            if (f > 900 && (f % 150) < 8) pad.buttons |= 1u << 3;            // RETRO_DEVICE_ID_JOYPAD_START
            if (f > 900 && (f % 150) >= 75 && (f % 150) < 83) pad.buttons |= 1u << 0;  // B = Cross
        }
        host.setVirtualPad(pad);
        if (!host.runFrame()) break;
        host.render(0, 0, pw, ph);
        SDL_GL_SwapWindow(win);
        ++presented;
    }
    if (!o.evalDir.empty()) evaluateInterpolation(o, host, pw, ph);
    if (o.vpad) {
        auto in = [&](unsigned device, unsigned index, unsigned id) { return host.inputState(0, device, index, id); };
        const int r2 = in(RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2);
        const int b = in(RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_B);
        std::printf("port 1 reads: left X %d, analog R2 %d (%.0f%%), analog B %d (%.0f%%), buttons 0x%04x; "
                    "vibration requests: %d\n",
                    in(RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X), r2, 100.0 * r2 / 32767,
                    b, 100.0 * b / 32767, in(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) & 0xFFFF, testPad.rumbles);
    }
    lr::HostStats st = host.stats();
    std::printf("core: %s %s | %dx%d | aspect %.3f | %llu core frames, %llu presented\n", st.coreName.c_str(),
                st.coreVersion.c_str(), st.baseWidth, st.baseHeight, host.displayAspect(),
                static_cast<unsigned long long>(st.framesRun), presented);
    if (!o.png.empty()) {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (host.captureFrame(rgba, w, h) && stbi_write_png(o.png.c_str(), w, h, 4, rgba.data(), w * 4))
            std::printf("wrote %s (%dx%d)\n", o.png.c_str(), w, h);
        else
            std::fprintf(stderr, "could not capture a frame\n");
    }
    host.stop();
    if (testPad.js) {
        SDL_CloseJoystick(testPad.js);
        SDL_DetachVirtualJoystick(testPad.id);
    }
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) return usage();

    if (o.dataDir.empty()) {
        // (SDL's tick counter only starts with SDL_Init, so the wall clock names the folder.)
        fs::path tmp = fs::temp_directory_path() /
                       ("rrm-launch-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        o.dataDir = pathToUtf8(tmp);
    }
    initPaths(o.dataDir.c_str());
    const bool dry = o.mode == "matrix" && o.spawnSeconds <= 0;
    emu::dryRun() = dry;
    if (dry) SDL_setenv_unsafe("RRM_RECOMP_DRY_RUN", "1", 1);

    Catalog catalog;
    std::string error;
    const std::string dir = o.catalogDir + "/";
    if (!catalog.loadFromStrings(readFile(dir + "games.json"), readFile(dir + "backends.json"), error,
                                 readFile(dir + "recomps.json"))) {
        std::fprintf(stderr, "catalog: %s\n", error.c_str());
        return 1;
    }
    catalog.loadExtraFiles(extraCatalogDir());  // a broken file is skipped with a warning
    Library lib;
    lib.load();
    for (auto& hit : scanFolder(catalog, o.folder))
        if (!hit.gameId.empty() && !lib.find(hit.gameId)) lib.entry(hit.gameId).files = hit.files;
    linkCores(catalog, lib, o.coresDir);
    for (auto& [id, path] : o.links) {
        const BackendDef* def = catalog.backend(id);
        auto b = def ? createBackend(*def) : nullptr;
        if (!b || !b->linkExisting(lib, path, error)) {
            std::fprintf(stderr, "link %s: %s\n", id.c_str(), b ? error.c_str() : "unknown backend");
            return 1;
        }
    }
    for (auto& id : o.installs) {
        const BackendDef* def = catalog.backend(id);
        auto b = def ? createBackend(*def) : nullptr;
        if (!b) {
            std::fprintf(stderr, "install %s: unknown backend\n", id.c_str());
            return 1;
        }
        TaskProgress progress;
        const bool ok = b->install(lib, progress);
        {
            std::lock_guard<std::mutex> lock(progress.mutex);
            for (auto& line : progress.log) std::printf("  [install %s] %s\n", id.c_str(), line.c_str());
        }
        if (!ok) return 1;
        lib.save();
        std::printf("installed %s -> %s\n", id.c_str(), b->installedExecutable(lib).c_str());
    }
    GameSettings settings = GameSettings::fromJson(o.settings);
    const ControllerSettings controllers = ControllerSettings::fromJson(o.controllers);
    const std::vector<ConnectedPad> pads = padsFrom(o.pads, controllers);

    std::printf("data folder: %s\nplatform: %s\n", o.dataDir.c_str(), currentPlatformKey().c_str());
    std::printf("controllers: %s\n", controllers.withOverrides(settings.extra).toJson().dump().c_str());
    for (auto& p : pads)
        std::printf("pad: %s %d (%s, %s) -> %s\n", p.name.c_str(), p.nameIndex, p.guid.empty() ? "no GUID" : p.guid.c_str(),
                    p.type.c_str(), p.player < 0 ? "no player" : ("Player " + std::to_string(p.player + 1)).c_str());
    int failures = 0;
    for (auto& g : catalog.games()) {
        if (o.mode == "run" && g.id != o.gameId) continue;
        if (!o.only.empty() && g.id != o.only) continue;
        const LibraryEntry* entry = lib.find(g.id);
        std::printf("\n== %s: %s\n", g.title.c_str(), entry ? entry->files.path.c_str() : "(no files found)");
        for (const GameBackendOption* opt : catalog.optionsFor(g, currentOs())) {
            if (o.mode == "run" && opt->backend != o.backendId) continue;
            const BackendDef* def = catalog.backend(opt->backend);
            auto b = def ? createBackend(*def) : nullptr;
            if (!b) {
                std::printf("  -- %s: NO ADAPTER\n", opt->backend.c_str());
                ++failures;
                continue;
            }
            LaunchRequest req;
            req.game = &g;
            req.option = opt;
            req.backend = def;
            req.entry = entry;
            req.library = &lib;
            req.settings = settings;
            req.display = o.display;
            req.controllers = controllers.withOverrides(settings.extra);
            req.pads = pads;
            InstallState st = b->installState(lib);
            std::printf("  -- %s [%s, priority %d]\n", def->id.c_str(), stateName(st), opt->priority);
            if (st != InstallState::Installed && st != InstallState::UserProvided) continue;
            for (auto& m : b->missingRequirements(req)) std::printf("     needs: %s\n", m.c_str());
            LaunchResult r = b->launch(req);
            std::printf("     %s\n", describe(r).c_str());
            if (!r.ok) ++failures;
            if (r.ok && r.pid > 0 && o.spawnSeconds > 0) {
                SDL_Delay(static_cast<Uint32>(o.spawnSeconds) * 1000);
                if (!processRunning(r.pid)) std::printf("     exited before the screenshot (see the backend log)\n");
                if (!o.png.empty()) {
                    ProcessSpec shot;
                    shot.argv = {"/usr/sbin/screencapture", "-x", o.png};
                    std::printf("     screenshot %s: exit %d\n", o.png.c_str(), runProcess(shot));
                }
                // terminateProcess() forces the end on a background thread after 3 s; wait for it, since
                // the tool may exit next (a program that ignores SIGTERM, such as RPCS3, outlived it).
                terminateProcess(r.pid);
                for (int i = 0; i < 60 && processRunning(r.pid); ++i) SDL_Delay(100);
                if (processRunning(r.pid)) std::printf("     still running after it was stopped (pid %lld)\n", (long long)r.pid);
            }
            if (o.mode == "run") {
                if (!r.ok || r.mode != LaunchResult::Mode::InProcessCore) return 1;
                return runCore(o, r, settings);
            }
        }
    }
    if (o.mode == "run") {
        std::fprintf(stderr, "no installed option %s for %s\n", o.backendId.c_str(), o.gameId.c_str());
        return 1;
    }
    std::printf("\n%d launch failure(s)\n", failures);
    return failures ? 1 : 0;
}
