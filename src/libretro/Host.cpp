#include "libretro/Host.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>

#include "libretro.h"
#include "libretro/Audio.h"
#include "libretro/CoreOptions.h"
#include "libretro/GlFunctions.h"
#include "libretro/Input.h"
#include "libretro/Video.h"

namespace rrm::lr {

namespace {

constexpr double kDefaultLatency = 0.064;      // seconds of queued audio we aim for
constexpr double kDrcMaxSkew = 0.005;          // dynamic rate control: +-0.5%
constexpr double kVsyncLockTolerance = 0.01;   // core fps within 1% of the display: 1 frame per vsync
constexpr int kMaxImageRepeat = 4;             // longer gaps between new images are no frame rate (15 fps min)
constexpr uint64_t kNsPerMs = 1000000ull;
constexpr uint64_t kSramInterval = 30000 * kNsPerMs;

struct CoreApi {
    void(RETRO_CALLCONV* set_environment)(retro_environment_t) = nullptr;
    void(RETRO_CALLCONV* set_video_refresh)(retro_video_refresh_t) = nullptr;
    void(RETRO_CALLCONV* set_audio_sample)(retro_audio_sample_t) = nullptr;
    void(RETRO_CALLCONV* set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void(RETRO_CALLCONV* set_input_poll)(retro_input_poll_t) = nullptr;
    void(RETRO_CALLCONV* set_input_state)(retro_input_state_t) = nullptr;
    void(RETRO_CALLCONV* init)() = nullptr;
    void(RETRO_CALLCONV* deinit)() = nullptr;
    unsigned(RETRO_CALLCONV* api_version)() = nullptr;
    void(RETRO_CALLCONV* get_system_info)(retro_system_info*) = nullptr;
    void(RETRO_CALLCONV* get_system_av_info)(retro_system_av_info*) = nullptr;
    void(RETRO_CALLCONV* set_controller_port_device)(unsigned, unsigned) = nullptr;
    void(RETRO_CALLCONV* reset)() = nullptr;
    void(RETRO_CALLCONV* run)() = nullptr;
    size_t(RETRO_CALLCONV* serialize_size)() = nullptr;
    bool(RETRO_CALLCONV* serialize)(void*, size_t) = nullptr;
    bool(RETRO_CALLCONV* unserialize)(const void*, size_t) = nullptr;
    bool(RETRO_CALLCONV* load_game)(const retro_game_info*) = nullptr;
    void(RETRO_CALLCONV* unload_game)() = nullptr;
    void*(RETRO_CALLCONV* get_memory_data)(unsigned) = nullptr;
    size_t(RETRO_CALLCONV* get_memory_size)(unsigned) = nullptr;
};

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimSeparators(std::string s) {
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string stemOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

// Writes via a temporary file so a crash never leaves a truncated save behind.
bool writeFileAtomic(const std::string& path, const void* data, size_t size) {
    SDL_CreateDirectory(dirOf(path).c_str());
    std::string tmp = path + ".tmp";
    if (!SDL_SaveFile(tmp.c_str(), data, size)) return false;
    if (SDL_RenamePath(tmp.c_str(), path.c_str())) return true;
    SDL_RemovePath(path.c_str());  // some platforms refuse to rename over a file
    return SDL_RenamePath(tmp.c_str(), path.c_str());
}

// "4:3", "16:9", "1.5" -> ratio; 0 for unknown.
double parseAspect(const std::string& s) {
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        double a = SDL_atof(s.substr(0, colon).c_str()), b = SDL_atof(s.substr(colon + 1).c_str());
        return (a > 0 && b > 0) ? a / b : 0.0;
    }
    double v = SDL_atof(s.c_str());
    return v > 0 ? v : 0.0;
}

}  // namespace

struct Host::Impl {
    // ---- static trampolines (libretro has no user data pointers) ----------------------------
    static Impl* s_active;
    static char s_logTag[64];

    static bool RETRO_CALLCONV envCb(unsigned cmd, void* data) { return s_active && s_active->environment(cmd, data); }
    static void RETRO_CALLCONV videoCb(const void* data, unsigned w, unsigned h, size_t pitch) {
        if (s_active) s_active->onVideo(data, w, h, pitch);
    }
    static void RETRO_CALLCONV audioSampleCb(int16_t left, int16_t right) {
        const int16_t frame[2] = {left, right};
        if (s_active) s_active->audio.append(frame, 1);
    }
    static size_t RETRO_CALLCONV audioBatchCb(const int16_t* data, size_t frames) {
        if (s_active) s_active->audio.append(data, frames);
        return frames;
    }
    static void RETRO_CALLCONV inputPollCb() {
        if (s_active) s_active->input.poll();
    }
    static int16_t RETRO_CALLCONV inputStateCb(unsigned port, unsigned device, unsigned index, unsigned id) {
        return s_active ? s_active->input.state(port, device, index, id) : 0;
    }
    static uintptr_t RETRO_CALLCONV hwFramebufferCb() { return s_active ? s_active->video.hwFramebuffer() : 0; }
    static retro_proc_address_t RETRO_CALLCONV hwProcAddressCb(const char* sym) {
        return reinterpret_cast<retro_proc_address_t>(SDL_GL_GetProcAddress(sym));
    }
    static bool RETRO_CALLCONV rumbleCb(unsigned port, retro_rumble_effect effect, uint16_t strength) {
        return s_active && s_active->input.rumble(port, effect, strength);
    }
    static void RETRO_CALLCONV ledCb(int, int) {}
    static void RETRO_CALLCONV logCb(retro_log_level level, const char* fmt, ...);
    static retro_time_t RETRO_CALLCONV perfTimeUsec() { return static_cast<retro_time_t>(SDL_GetTicksNS() / 1000); }
    static retro_perf_tick_t RETRO_CALLCONV perfCounter() { return SDL_GetPerformanceCounter(); }
    static uint64_t RETRO_CALLCONV cpuFeatures();
    static void RETRO_CALLCONV perfRegister(retro_perf_counter* c) {
        if (c) c->registered = true;
    }
    static void RETRO_CALLCONV perfStart(retro_perf_counter* c) {
        if (c) {
            c->call_cnt++;
            c->start = SDL_GetPerformanceCounter();
        }
    }
    static void RETRO_CALLCONV perfStop(retro_perf_counter* c) {
        if (c) c->total += SDL_GetPerformanceCounter() - c->start;
    }
    static void RETRO_CALLCONV perfLog() {}

    // ---- state ----------------------------------------------------------------------------------
    SDL_Window* window = nullptr;
    HostConfig cfg;
    std::string systemDir, saveDir, contentStem;

    GlFunctions gl;
    bool gles = false, glCompat = false;
    int glMajor = 0, glMinor = 0;
    GLuint defaultFbo = 0;
    Video video;

    SDL_SharedObject* lib = nullptr;
    CoreApi api;
    bool initialized = false, gameLoaded = false, running = false;
    std::atomic<bool> shutdownRequested{false};
    std::string libraryName, libraryVersion;
    bool needFullpath = false, supportsNoGame = false;
    void* contentData = nullptr;
    size_t contentSize = 0;
    retro_system_av_info av{};
    retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_hw_render_callback hw{};
    bool hwRequested = false, hwReady = false;
    std::vector<std::vector<std::pair<std::string, unsigned>>> controllerTypes;

    CoreOptions options;
    Input input;
    Audio audio;
    retro_frame_time_callback frameTime{};
    retro_audio_buffer_status_callback_t audioStatusCb = nullptr;
    retro_core_options_update_display_callback_t optionsDisplayCb = nullptr;
    unsigned minLatencyMs = 0;
    std::set<unsigned> unknownCommands;

    // pacing
    bool paused = false, turbo = false, muted = false, wasMuted = false;
    int speedPercent = 100, turboPercent = 300;
    double displayHz = 0;
    bool vsyncOn = false;
    bool displayPaced = false;  // presents follow the display refresh, not one per core frame
    uint64_t lastSettingsCheck = 0, lastPresent = 0, lastAccount = 0, lastSramCheck = 0;
    double presentEma = 0;
    int presentSamples = 0;
    double accum = 0, queueEma = -1;
    const char* pacing = "start";
    uint64_t lastPacingLog = 0;

    // stats / presentation
    HostStats statsCache;
    uint64_t statsStart = 0;
    int statsFrames = 0, statsPresents = 0;
    int lastRenderW = 0, lastRenderH = 0;
    bool blendLogged = false;
    // Image cadence for frame smoothing: games drawing at 30 fps show each image for two core
    // frames (as NULL dupes or identical resubmits), so in-between images span both.
    bool newImage = false;          // the running core frame delivered a new image
    int framesSinceImage = 0;       // core frames run since the newest image (0 = it came this frame)
    int imageIntervals[3] = {1, 1, 1};  // core frames between the last new images (ring)
    int imageIntervalPos = 0;
    // A hardware repeat check in flight: the guess made for the newest frame and the cadence
    // state from before it was counted.
    bool checkPending = false, checkRepeat = false;
    int checkSince = 0, checkPos = 0, checkInterval = 1;
    std::vector<uint8_t> sramShadow;

    std::string error;
    static std::mutex s_logMutex;
    static std::deque<std::string> s_coreErrors;

    // ---- lifecycle --------------------------------------------------------------------------------
    bool start(SDL_Window* win, const HostConfig& config, std::string& err);
    void stop();
    bool fail(std::string& err, const std::string& message) {
        err = message;
        error = message;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[libretro] %s", message.c_str());
        stop();
        return false;
    }
    bool loadSymbols(std::string& missing);
    void detectGl();
    void enterCore();
    void leaveCore();
    void restoreGl();

    // ---- callbacks -------------------------------------------------------------------------------
    bool environment(unsigned cmd, void* data);
    bool setHwRender(retro_hw_render_callback* cb);
    void onVideo(const void* data, unsigned w, unsigned h, size_t pitch);
    void onAvInfo(const retro_system_av_info& info);
    void onControllerInfo(const retro_controller_info* info);
    void applyPortDevices();

    // ---- frame loop -------------------------------------------------------------------------------
    bool runFrame();
    void runCoreFrame();
    void countFrame(bool isNewImage);
    void resolveRepeatCheck(bool wait);
    int runFrames(int count, uint64_t start, uint64_t budget);
    void refreshDisplaySettings(uint64_t now, bool force);
    double trackQueue(double target);
    void applyDrc(double baseRatio, double target);
    double latency() const { return std::max(kDefaultLatency, minLatencyMs / 1000.0); }
    double coreFps() const { return av.timing.fps > 1 ? av.timing.fps : 60.0; }
    double speedFactor() const { return (turbo ? turboPercent : speedPercent) / 100.0; }
    double smoothRate() const { return coreFps() * std::max(speedPercent, 1) / 100.0; }
    // Smoothing only helps when display-paced presents outnumber core frames; fast-forward and
    // pauses show frames as they are. (With V-Sync off the timer waits for each core frame.)
    // Presents must really outnumber core frames: the measured rate, not just the mode's, as a GPU
    // that cannot keep up halves the present rate on the same display.
    bool smoothingWanted() const {
        return video.smoothingMode() && !turbo && !paused && displayPaced && displayHz > smoothRate() * 1.05 &&
               presentSamples > 30 && presentEma > 0 && 1.0 / presentEma > smoothRate() * 1.05;
    }
    // Core frames per new image (1 = 60 fps game, 2 = 30 fps): the median of the last three
    // intervals, so one late image or a pause on a still screen doesn't throw it off.
    int imageRepeat() const {
        const int a = imageIntervals[0], b = imageIntervals[1], c = imageIntervals[2];
        return std::max(std::min(a, b), std::min(std::max(a, b), c));
    }
    void resetPacing();
    void render(int x, int y, int w, int h);
    double coreAspect() const;
    double contentAspect() const;  // 0 = fill the rectangle
    void fitRect(int& x, int& y, int& w, int& h) const;

    void loadSram();
    void saveSram(bool force);
    bool saveState(const std::string& path);
    bool loadState(const std::string& path);
};

Host::Impl* Host::Impl::s_active = nullptr;
char Host::Impl::s_logTag[64] = "libretro";
std::mutex Host::Impl::s_logMutex;
std::deque<std::string> Host::Impl::s_coreErrors;

// ================================================================================================
// callbacks

void RETRO_CALLCONV Host::Impl::logCb(retro_log_level level, const char* fmt, ...) {
    if (!fmt) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t len = std::strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    SDL_LogPriority prio = SDL_LOG_PRIORITY_INFO;
    switch (level) {
    case RETRO_LOG_DEBUG: prio = SDL_LOG_PRIORITY_DEBUG; break;
    case RETRO_LOG_INFO: prio = SDL_LOG_PRIORITY_INFO; break;
    case RETRO_LOG_WARN: prio = SDL_LOG_PRIORITY_WARN; break;
    default: prio = SDL_LOG_PRIORITY_ERROR; break;
    }
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, prio, "[%s] %s", s_logTag, buf);
    if (level >= RETRO_LOG_ERROR) {
        std::lock_guard lock(s_logMutex);
        s_coreErrors.emplace_back(buf);
        if (s_coreErrors.size() > 4) s_coreErrors.pop_front();
    }
}

uint64_t RETRO_CALLCONV Host::Impl::cpuFeatures() {
    uint64_t f = 0;
    if (SDL_HasMMX()) f |= RETRO_SIMD_MMX;
    if (SDL_HasSSE()) f |= RETRO_SIMD_SSE;
    if (SDL_HasSSE2()) f |= RETRO_SIMD_SSE2;
    if (SDL_HasSSE3()) f |= RETRO_SIMD_SSE3;
    if (SDL_HasSSE41()) f |= RETRO_SIMD_SSE4;
    if (SDL_HasSSE42()) f |= RETRO_SIMD_SSE42;
    if (SDL_HasAVX()) f |= RETRO_SIMD_AVX;
    if (SDL_HasAVX2()) f |= RETRO_SIMD_AVX2;
    if (SDL_HasNEON()) f |= RETRO_SIMD_NEON | RETRO_SIMD_ASIMD;
    return f;
}

bool Host::Impl::environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_OVERSCAN:
        if (data) *static_cast<bool*>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (data) *static_cast<bool*>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        auto* m = static_cast<const retro_message*>(data);
        if (m && m->msg) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[%s] message: %s", s_logTag, m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
        auto* m = static_cast<const retro_message_ext*>(data);
        if (m && m->msg) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[%s] message: %s", s_logTag, m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SHUTDOWN:
        shutdownRequested = true;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char**>(data) = systemDir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = saveDir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:  // == GET_CONTENT_DIRECTORY (deprecated)
        *static_cast<const char**>(data) = systemDir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
        *static_cast<const char**>(data) = cfg.corePath.c_str();
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        auto fmt = *static_cast<const retro_pixel_format*>(data);
        if (fmt != RETRO_PIXEL_FORMAT_0RGB1555 && fmt != RETRO_PIXEL_FORMAT_XRGB8888 &&
            fmt != RETRO_PIXEL_FORMAT_RGB565)
            return false;
        pixelFormat = fmt;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
        return setHwRender(static_cast<retro_hw_render_callback*>(data));
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        *static_cast<unsigned*>(data) = gles ? RETRO_HW_CONTEXT_OPENGLES3 : RETRO_HW_CONTEXT_OPENGL_CORE;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
        return true;  // we share the app's context and reset GL state around every core call
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
    case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
        return false;  // Vulkan / Direct3D only

    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* var = static_cast<retro_variable*>(data);
        if (!var || !var->key) return false;
        var->value = options.get(var->key);
        return var->value != nullptr;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        options.defineLegacy(static_cast<const retro_variable*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLE: {
        auto* var = static_cast<const retro_variable*>(data);
        if (!var) return true;
        if (!var->key || !var->value) return false;
        options.set(var->key, var->value);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        if (data) *static_cast<bool*>(data) = options.consumeUpdated();
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned*>(data) = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        options.defineV1(static_cast<const retro_core_option_definition*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
        auto* intl = static_cast<const retro_core_options_intl*>(data);
        if (intl) options.defineV1(intl->us);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        options.defineV2(static_cast<const retro_core_options_v2*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
        auto* intl = static_cast<const retro_core_options_v2_intl*>(data);
        if (intl) options.defineV2(intl->us);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: {
        auto* d = static_cast<const retro_core_option_display*>(data);
        if (d) options.setVisible(d->key, d->visible);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: {
        auto* cb = static_cast<const retro_core_options_update_display_callback*>(data);
        optionsDisplayCb = cb ? cb->callback : nullptr;
        return true;
    }

    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        auto* g = static_cast<const retro_game_geometry*>(data);
        if (!g) return false;
        av.geometry.base_width = g->base_width;  // max size cannot change here
        av.geometry.base_height = g->base_height;
        av.geometry.aspect_ratio = g->aspect_ratio;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        if (!data) return false;
        onAvInfo(*static_cast<const retro_system_av_info*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_ROTATION:
        return false;  // the core rotates internally

    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = logCb;
        return true;
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
        auto* perf = static_cast<retro_perf_callback*>(data);
        perf->get_time_usec = perfTimeUsec;
        perf->get_cpu_features = cpuFeatures;
        perf->get_perf_counter = perfCounter;
        perf->perf_register = perfRegister;
        perf->perf_start = perfStart;
        perf->perf_stop = perfStop;
        perf->perf_log = perfLog;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        if (!input.config().rumble) return false;  // vibration is off in Marathon's controller settings
        static_cast<retro_rumble_interface*>(data)->set_rumble_state = rumbleCb;
        return true;
    case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
        if (data) static_cast<retro_led_interface*>(data)->set_led_state = ledCb;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
        *static_cast<uint64_t*>(data) = (1ull << RETRO_DEVICE_JOYPAD) | (1ull << RETRO_DEVICE_ANALOG);
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
        *static_cast<unsigned*>(data) = Input::kPorts;
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        onControllerInfo(static_cast<const retro_controller_info*>(data));
        return true;

    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        frameTime = data ? *static_cast<const retro_frame_time_callback*>(data) : retro_frame_time_callback{};
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
        auto* cb = static_cast<const retro_audio_buffer_status_callback*>(data);
        audioStatusCb = cb ? cb->callback : nullptr;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        minLatencyMs = data ? std::min(*static_cast<const unsigned*>(data), 512u) : 0;
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
        return false;  // async audio callback mode is not supported

    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        supportsNoGame = data && *static_cast<const bool*>(data);
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_GET_USERNAME:
        *static_cast<const char**>(data) = "Player";
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        if (data) *static_cast<int*>(data) = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        if (data) *static_cast<bool*>(data) = turbo;
        return true;
    case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
        if (data) *static_cast<float*>(data) = static_cast<float>(displayHz > 0 ? displayHz : coreFps());
        return true;
    case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
        auto* t = static_cast<retro_throttle_state*>(data);
        if (!t) return false;
        const double speed = speedFactor();
        if (paused) {
            t->mode = RETRO_THROTTLE_FRAME_STEPPING;
            t->rate = 0;
        } else if (turbo) {
            t->mode = RETRO_THROTTLE_FAST_FORWARD;
            t->rate = turboPercent > 0 ? static_cast<float>(coreFps() * speed) : 0.0f;
        } else if (speedPercent != 100) {
            t->mode = speedPercent < 100 ? RETRO_THROTTLE_SLOW_MOTION : RETRO_THROTTLE_FAST_FORWARD;
            t->rate = static_cast<float>(coreFps() * speed);
        } else {
            t->mode = RETRO_THROTTLE_NONE;
            t->rate = static_cast<float>(coreFps());
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_TVOS)
        if (data) *static_cast<bool*>(data) = false;
#else
        if (data) *static_cast<bool*>(data) = true;
#endif
        return true;
    case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
        if (data) *static_cast<int*>(data) = RETRO_SAVESTATE_CONTEXT_NORMAL;
        return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        *static_cast<unsigned*>(data) = 1;
        return true;
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        *static_cast<unsigned*>(data) = 1;
        return true;

    // Accepted and ignored.
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
    case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        return true;

    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
    case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
        return false;

    default:
        if (unknownCommands.insert(cmd).second) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "[libretro] unsupported environment command %u (0x%x)",
                         cmd & 0xFFFFu, cmd);
        }
        return false;
    }
}

bool Host::Impl::setHwRender(retro_hw_render_callback* cb) {
    if (!cb) return false;
    auto versionOk = [&](unsigned major, unsigned minor) {
        return glMajor > static_cast<int>(major) ||
               (glMajor == static_cast<int>(major) && glMinor >= static_cast<int>(minor));
    };
    bool ok = false;
    switch (cb->context_type) {
    case RETRO_HW_CONTEXT_OPENGL_CORE:
        ok = !gles && versionOk(cb->version_major, cb->version_minor);
        break;
    case RETRO_HW_CONTEXT_OPENGL:
        // Legacy GL 2.x semantics only work on a compatibility (or pre-3.2) context.
        ok = !gles && glCompat;
        break;
    case RETRO_HW_CONTEXT_OPENGLES2:
    case RETRO_HW_CONTEXT_OPENGLES3:
        ok = gles;
        break;
    case RETRO_HW_CONTEXT_OPENGLES_VERSION:
        ok = gles && versionOk(cb->version_major, cb->version_minor);
        break;
    default:
        ok = false;  // Vulkan / Direct3D: refusing makes cores fall back to GL or software
        break;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[libretro] core requests HW context type %d v%u.%u (depth %d stencil %d) -> %s",
                static_cast<int>(cb->context_type), cb->version_major, cb->version_minor, cb->depth, cb->stencil,
                ok ? "accepted" : "rejected");
    if (!ok) return false;
    cb->get_current_framebuffer = hwFramebufferCb;
    cb->get_proc_address = hwProcAddressCb;
    hw = *cb;
    hwRequested = true;
    return true;
}

void Host::Impl::onVideo(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;  // dupe: keep showing the previous frame
    if (data == RETRO_HW_FRAME_BUFFER_VALID ? video.submitHardware(w, h, hw.bottom_left_origin)
                                            : video.submitSoftware(data, w, h, pitch, pixelFormat))
        newImage = true;
    statsCache.baseWidth = static_cast<int>(w);
    statsCache.baseHeight = static_cast<int>(h);
}

void Host::Impl::onAvInfo(const retro_system_av_info& info) {
    const double oldRate = av.timing.sample_rate;
    av = info;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[libretro] av info: %ux%u (max %ux%u) aspect %.3f, %.3f fps, %.0f Hz",
                info.geometry.base_width, info.geometry.base_height, info.geometry.max_width,
                info.geometry.max_height, info.geometry.aspect_ratio, info.timing.fps, info.timing.sample_rate);
    if (hwReady) video.growHwTarget(info.geometry.max_width, info.geometry.max_height);
    if (audio.ok() && info.timing.sample_rate > 0 && info.timing.sample_rate != oldRate) {
        audio.setSampleRate(info.timing.sample_rate);
        audio.prefill(latency() * 0.75);
        queueEma = -1;
    }
    accum = 0;
}

void Host::Impl::onControllerInfo(const retro_controller_info* info) {
    controllerTypes.clear();
    for (; info && info->types; ++info) {
        std::vector<std::pair<std::string, unsigned>> types;
        for (unsigned i = 0; i < info->num_types; ++i) {
            if (info->types[i].desc) types.emplace_back(info->types[i].desc, info->types[i].id);
        }
        controllerTypes.push_back(std::move(types));
    }
}

// Marathon's own keys in HostConfig::coreOptions, set from the controller settings by
// backends/LibretroControls.cpp (cores ignore them). Values are strings; an absent key means the default.
//   key                               value                                                  default
//   rrm_port<N>_device (N 0..3)       a core-declared controller, by libretro id or by       "" (RetroPad)
//                                     (part of) its name, e.g. "negcon" or "dualshock"
//   rrm_player<N> (N 1..4)            "" = the next free controller, "none", or an SDL GUID  ""
//   rrm_input_deadzone                "0".."40": stick dead zone in percent, both sticks     "0"
//   rrm_input_stick_to_dpad           "1": the left stick presses the d-pad past half travel "0"
//   rrm_input_dpad_to_stick           "1": d-pad / arrow keys left-right steer at full lock  "0"
//   rrm_input_r2_as, rrm_input_l2_as  b|a|y|x|l|r|l3|r3|select|start: the trigger presses    "" (R2 / L2)
//                                     that button instead, its travel as the pressure
//   rrm_input_analog_triggers         "0": analog L2/R2 read 0 (the digital bits stay)       "1"
//   rrm_input_left_y                  "0": the left stick's Y axis reads 0                   "1"
//   rrm_rumble                        "0": no vibration (no rumble interface for the core)   "1"
//
// Every port gets the RetroPad unless rrm_portN_device picks another controller. Cores that declare
// fewer ports only hear about the first two.
void Host::Impl::applyPortDevices() {
    if (!api.set_controller_port_device) return;
    const size_t declared = std::max<size_t>(2, controllerTypes.size());
    for (unsigned port = 0; port < Input::kPorts; ++port) {
        unsigned device = RETRO_DEVICE_JOYPAD;
        const std::string want = options.peek("rrm_port" + std::to_string(port) + "_device");
        if (!want.empty()) {
            if (want.find_first_not_of("0123456789") == std::string::npos) {
                device = static_cast<unsigned>(std::stoul(want));
            } else if (port < controllerTypes.size()) {
                for (const auto& [desc, id] : controllerTypes[port]) {
                    if (lower(desc).find(lower(want)) != std::string::npos) {
                        device = id;
                        break;
                    }
                }
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[libretro] port %u device %u (%s)", port + 1, device,
                        want.c_str());
        }
        if (port < declared) api.set_controller_port_device(port, device);
    }
}

// ================================================================================================
// lifecycle

bool Host::Impl::loadSymbols(std::string& missing) {
#define RRM_SYM(name)                                                                              \
    api.name = reinterpret_cast<decltype(api.name)>(SDL_LoadFunction(lib, "retro_" #name));       \
    if (!api.name) {                                                                               \
        missing = "retro_" #name;                                                                  \
        return false;                                                                              \
    }
    RRM_SYM(set_environment)
    RRM_SYM(set_video_refresh)
    RRM_SYM(set_audio_sample)
    RRM_SYM(set_audio_sample_batch)
    RRM_SYM(set_input_poll)
    RRM_SYM(set_input_state)
    RRM_SYM(init)
    RRM_SYM(deinit)
    RRM_SYM(api_version)
    RRM_SYM(get_system_info)
    RRM_SYM(get_system_av_info)
    RRM_SYM(set_controller_port_device)
    RRM_SYM(reset)
    RRM_SYM(run)
    RRM_SYM(serialize_size)
    RRM_SYM(serialize)
    RRM_SYM(unserialize)
    RRM_SYM(load_game)
    RRM_SYM(unload_game)
    RRM_SYM(get_memory_data)
    RRM_SYM(get_memory_size)
#undef RRM_SYM
    return true;
}

void Host::Impl::detectGl() {
    const char* version = reinterpret_cast<const char*>(gl.GetString(GL_VERSION));
    gles = version && std::strstr(version, "OpenGL ES") != nullptr;
#if defined(RRM_GLES) && RRM_GLES
    gles = true;
#endif
    GLint major = 0, minor = 0;
    gl.GetIntegerv(GL_MAJOR_VERSION, &major);
    gl.GetIntegerv(GL_MINOR_VERSION, &minor);
    if (major <= 0 && version) {
        const char* p = version;
        while (*p && (*p < '0' || *p > '9')) ++p;
        std::sscanf(p, "%d.%d", &major, &minor);
    }
    glMajor = major;
    glMinor = minor;
    glCompat = false;
    if (!gles) {
        if (major > 3 || (major == 3 && minor >= 2)) {
            GLint mask = 0;
            gl.GetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
            glCompat = (mask & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) != 0;
        } else {
            glCompat = true;  // no profiles before 3.2
        }
    }
    const bool samplers = gles ? major >= 3 : (major > 3 || (major == 3 && minor >= 3));
    if (!samplers) gl.BindSampler = nullptr;
    if (gles) {
        gl.PolygonMode = nullptr;  // some EGL drivers return stubs for any name
        gl.BindFragDataLocation = nullptr;
    }

    GLint fbo = 0;
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    defaultFbo = static_cast<GLuint>(fbo);
#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_TVOS)
    Sint64 uikitFbo = SDL_GetNumberProperty(SDL_GetWindowProperties(window),
                                            SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0);
    if (uikitFbo > 0) defaultFbo = static_cast<GLuint>(uikitFbo);
#endif
    while (gl.GetError() != GL_NO_ERROR) {}
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[libretro] GL %d.%d %s%s, default framebuffer %u: %s / %s", glMajor,
                glMinor, gles ? "ES" : (glCompat ? "compatibility" : "core"), "", defaultFbo,
                version ? version : "?", reinterpret_cast<const char*>(gl.GetString(GL_RENDERER)));
}

void Host::Impl::enterCore() {
    if (video.hwFramebuffer()) gl.BindFramebuffer(GL_FRAMEBUFFER, video.hwFramebuffer());
}

void Host::Impl::leaveCore() { restoreGl(); }

// Puts back the state Dear ImGui's OpenGL backend (and our presenter) expect after a core
// has rendered with the shared context.
void Host::Impl::restoreGl() {
    auto& g = gl;
    g.BindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
    int fbw = 0, fbh = 0;
    SDL_GetWindowSizeInPixels(window, &fbw, &fbh);
    g.Viewport(0, 0, fbw, fbh);
    g.Disable(GL_DEPTH_TEST);
    g.Disable(GL_STENCIL_TEST);
    g.Disable(GL_SCISSOR_TEST);
    g.Disable(GL_BLEND);
    g.Disable(GL_CULL_FACE);
    g.Disable(GL_POLYGON_OFFSET_FILL);
    g.Disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    g.Disable(GL_SAMPLE_COVERAGE);
    g.Disable(GL_RASTERIZER_DISCARD);
    if (gles) {
        g.Disable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    } else {
        g.Disable(GL_FRAMEBUFFER_SRGB);
        g.Disable(GL_PRIMITIVE_RESTART);
        if (g.PolygonMode) g.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    g.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g.DepthMask(GL_TRUE);
    g.StencilMask(0xFFFFFFFFu);
    g.FrontFace(GL_CCW);
    g.UseProgram(0);
    g.BindVertexArray(0);
    g.ActiveTexture(GL_TEXTURE0);
    g.BindTexture(GL_TEXTURE_2D, 0);
    if (g.BindSampler) g.BindSampler(0, 0);
    g.BindBuffer(GL_ARRAY_BUFFER, 0);
    g.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    g.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    g.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    g.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    g.PixelStorei(GL_PACK_ALIGNMENT, 4);
    g.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    g.PixelStorei(GL_PACK_SKIP_ROWS, 0);
    g.PixelStorei(GL_PACK_SKIP_PIXELS, 0);
    // Errors raised by the core are not the app's business.
    for (int i = 0; i < 32 && g.GetError() != GL_NO_ERROR; ++i) {}
}

bool Host::Impl::start(SDL_Window* win, const HostConfig& config, std::string& err) {
    if (s_active && s_active != this) {
        err = error = "another libretro core is already running";
        return false;
    }
    if (initialized || lib || running) stop();
    err.clear();
    error.clear();
    window = win;
    cfg = config;
    // Some full-path cores accept nonexistent content and boot a BIOS/demo instead.
    // Keep intentional no-content launches, but never silently lose a requested disc.
    if (!cfg.contentPath.empty()) {
        SDL_PathInfo info{};
        if (!SDL_GetPathInfo(cfg.contentPath.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) {
            err = error = "Game file is missing or unavailable: " + cfg.contentPath +
                          ". Reconnect the drive or add/scan the game again at its current location.";
            return false;
        }
    }
    if (!window || !SDL_GL_GetCurrentContext()) {
        err = error = "no current OpenGL context";
        return false;
    }
    std::string missing;
    if (!gl.load(missing)) {
        err = error = "OpenGL entry point missing: " + missing;
        return false;
    }
    detectGl();
    {
        std::string videoError;
        if (!video.init(&gl, gles, videoError)) {
            video.shutdown();
            restoreGl();
            err = error = videoError;
            return false;
        }
        video.setSmoothing(cfg.smoothing);
    }

    contentStem = cfg.contentPath.empty() ? "game" : stemOf(cfg.contentPath);
    const std::string contentDir = cfg.contentPath.empty() ? std::string(".") : dirOf(cfg.contentPath);
    systemDir = trimSeparators(cfg.systemDir.empty() ? contentDir : cfg.systemDir);
    saveDir = trimSeparators(cfg.saveDir.empty() ? contentDir : cfg.saveDir);
    SDL_CreateDirectory(systemDir.c_str());
    SDL_CreateDirectory(saveDir.c_str());

    lib = SDL_LoadObject(cfg.corePath.c_str());
    if (!lib) return fail(err, "cannot load core " + cfg.corePath + ": " + SDL_GetError());
    if (!loadSymbols(missing)) return fail(err, cfg.corePath + " is not a libretro core (missing " + missing + ")");
    if (api.api_version() != RETRO_API_VERSION)
        return fail(err, "core API version " + std::to_string(api.api_version()) + " is not supported");

    retro_system_info info{};
    api.get_system_info(&info);
    libraryName = info.library_name ? info.library_name : "core";
    libraryVersion = info.library_version ? info.library_version : "";
    needFullpath = info.need_fullpath;
    SDL_strlcpy(s_logTag, libraryName.c_str(), sizeof s_logTag);
    {
        std::lock_guard lock(s_logMutex);
        s_coreErrors.clear();
    }

    options.clear();
    options.setOverrides(cfg.coreOptions);
    input.configure(inputConfigFromOptions(cfg.coreOptions));  // before the core asks for rumble
    {
        const InputConfig& ic = input.config();
        std::string players;
        for (const auto& p : ic.players)
            players += (players.empty() ? "" : ",") + (p.controller.empty() ? std::string("auto") : p.controller);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[libretro] input: players %s, dead zone %d, stick to d-pad %d, d-pad to stick %d, "
                    "R2/L2 as %d/%d, analog triggers %d, left Y %d, rumble %d",
                    players.c_str(), ic.deadzone, ic.stickToDpad, ic.dpadToStick, ic.r2As, ic.l2As,
                    ic.analogTriggers, ic.leftY, ic.rumble);
    }
    speedPercent = std::clamp(cfg.speedPercent, 5, 1000);
    turboPercent = std::clamp(cfg.turboPercent, 0, 5000);
    paused = turbo = false;
    shutdownRequested = false;

    s_active = this;  // cores call back during retro_set_environment already
    api.set_environment(envCb);
    api.set_video_refresh(videoCb);
    api.set_audio_sample(audioSampleCb);
    api.set_audio_sample_batch(audioBatchCb);
    api.set_input_poll(inputPollCb);
    api.set_input_state(inputStateCb);
    api.init();
    initialized = true;

    retro_game_info game{};
    const bool noGame = cfg.contentPath.empty();
    if (noGame && !supportsNoGame) return fail(err, libraryName + " needs a game file");
    if (!noGame) {
        game.path = cfg.contentPath.c_str();
        if (!needFullpath) {
            contentData = SDL_LoadFile(cfg.contentPath.c_str(), &contentSize);
            if (!contentData) return fail(err, "cannot read " + cfg.contentPath + ": " + SDL_GetError());
            game.data = contentData;
            game.size = contentSize;
        }
    }
    enterCore();
    const bool loaded = api.load_game(noGame ? nullptr : &game);
    leaveCore();
    if (!loaded) {
        std::string message = libraryName + " could not load " + (noGame ? std::string("(no game)") : cfg.contentPath);
        {
            std::lock_guard lock(s_logMutex);  // released before fail() lets the core log again
            if (!s_coreErrors.empty()) message += ": " + s_coreErrors.back();
        }
        return fail(err, message);
    }
    gameLoaded = true;
    shutdownRequested = false;

    retro_system_av_info avInfo{};
    api.get_system_av_info(&avInfo);
    onAvInfo(avInfo);
    applyPortDevices();

    if (hwRequested) {
        std::string fboError;
        const unsigned w = std::max(av.geometry.max_width, av.geometry.base_width);
        const unsigned h = std::max(av.geometry.max_height, av.geometry.base_height);
        if (!video.createHwTarget(w ? w : 640, h ? h : 480, hw.depth, hw.stencil, fboError))
            return fail(err, fboError);
        hwReady = true;
        enterCore();
        if (hw.context_reset) hw.context_reset();
        leaveCore();
    }

    if (audio.open(av.timing.sample_rate)) audio.prefill(latency() * 0.75);
    input.open();
    loadSram();

    running = true;
    resetPacing();
    refreshDisplaySettings(SDL_GetTicksNS(), true);
    statsCache = HostStats{};
    statsStart = 0;
    lastRenderW = lastRenderH = 0;
    blendLogged = false;
    framesSinceImage = imageIntervalPos = 0;
    imageIntervals[0] = imageIntervals[1] = imageIntervals[2] = 1;
    checkPending = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[libretro] started %s %s (%s), %s rendering", libraryName.c_str(),
                libraryVersion.c_str(), cfg.contentPath.c_str(), hwRequested ? "OpenGL" : "software");
    return true;
}

void Host::Impl::stop() {
    // stop() must run while the app's GL context is still alive; if it is already gone we
    // skip every GL call (and context_destroy) rather than crash.
    const bool glAlive = gl.GetError && SDL_GL_GetCurrentContext();
    if (gameLoaded) saveSram(true);
    if (hwReady && hw.context_destroy && glAlive) {
        enterCore();
        hw.context_destroy();
        leaveCore();
    }
    hwReady = false;
    if (gameLoaded && api.unload_game) {
        if (glAlive) enterCore();
        api.unload_game();
        if (glAlive) leaveCore();
    }
    gameLoaded = false;
    if (initialized && api.deinit) api.deinit();
    initialized = false;
    if (lib) SDL_UnloadObject(lib);
    lib = nullptr;
    api = CoreApi{};
    if (contentData) SDL_free(contentData);
    contentData = nullptr;
    contentSize = 0;

    video.shutdown(glAlive);
    if (glAlive) restoreGl();
    audio.close();
    input.close();
    options.clear();
    controllerTypes.clear();
    unknownCommands.clear();
    hw = retro_hw_render_callback{};
    hwRequested = false;
    av = retro_system_av_info{};
    pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
    frameTime = retro_frame_time_callback{};
    audioStatusCb = nullptr;
    optionsDisplayCb = nullptr;
    minLatencyMs = 0;
    supportsNoGame = false;
    sramShadow.clear();
    running = false;
    if (s_active == this) s_active = nullptr;
    SDL_strlcpy(s_logTag, "libretro", sizeof s_logTag);
}

// ================================================================================================
// frame loop

void Host::Impl::resetPacing() {
    const uint64_t now = SDL_GetTicksNS();
    lastPresent = 0;
    lastAccount = now;
    presentSamples = 0;
    presentEma = 0;
    displayPaced = false;
    accum = 0;
    queueEma = -1;
    wasMuted = muted = false;
}

void Host::Impl::refreshDisplaySettings(uint64_t now, bool force) {
    if (!force && now - lastSettingsCheck < 500 * kNsPerMs) return;
    lastSettingsCheck = now;
    double hz = 0;
    if (SDL_DisplayID display = SDL_GetDisplayForWindow(window)) {
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display)) {
            hz = (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
                     ? static_cast<double>(mode->refresh_rate_numerator) / mode->refresh_rate_denominator
                     : mode->refresh_rate;
        }
    }
    if (std::fabs(hz - displayHz) > 0.01) {
        displayHz = hz;
        presentSamples = 0;
        accum = 0;
    }
    int interval = 0;
    vsyncOn = SDL_GL_GetSwapInterval(&interval) ? interval != 0 : cfg.vsync;
}

void Host::Impl::countFrame(bool isNewImage) {
    if (isNewImage) {
        const int interval = framesSinceImage + 1;
        imageIntervals[imageIntervalPos] = interval <= kMaxImageRepeat ? interval : 1;  // a still screen isn't a frame rate
        imageIntervalPos = (imageIntervalPos + 1) % 3;
        framesSinceImage = 0;
    } else if (framesSinceImage <= kMaxImageRepeat) {
        ++framesSinceImage;
    }
}

// Called before the next core frame runs (or a present), so the guessed frame is still the newest.
void Host::Impl::resolveRepeatCheck(bool wait) {
    if (!checkPending) return;
    const int result = video.repeatCheckResult(wait);
    if (result < 0 && !wait) return;
    checkPending = false;
    if (result < 0 || (result == 1) == checkRepeat) return;
    // Guessed wrong (the cadence changed): count the frame again with the real answer. A repeat
    // that already went into the smoothing history shows as a brief stop until the next image.
    framesSinceImage = checkSince;
    imageIntervalPos = checkPos;
    imageIntervals[checkPos] = checkInterval;
    countFrame(result == 0);
    video.holdFrame(result == 1);
}

void Host::Impl::runCoreFrame() {
    input.poll();
    if (frameTime.callback) frameTime.callback(frameTime.reference > 0 ? frameTime.reference
                                                                        : static_cast<retro_usec_t>(1e6 / coreFps()));
    if (audioStatusCb) {
        const double fill = audio.ok() && !muted ? audio.queuedSeconds() / latency() : 1.0;
        const unsigned occupancy = static_cast<unsigned>(std::clamp(fill * 100.0, 0.0, 100.0));
        audioStatusCb(audio.ok() && !muted, occupancy, occupancy < 25);
    }
    // The previous frame's answer, if the GPU has it (normally since the last present). Never wait
    // for it here: without an answer the guess stands.
    resolveRepeatCheck(false);
    if (checkPending) {
        checkPending = false;
        video.cancelRepeatCheck();
    }
    newImage = false;
    enterCore();
    api.run();
    leaveCore();
    // Identical hardware frames are found on the GPU while smoothing needs the image cadence. Until
    // the answer arrives, the frame is assumed to follow the cadence (30 fps: every other frame
    // repeats); a wrong guess is corrected then (see resolveRepeatCheck).
    if (newImage && hwReady && smoothingWanted()) {
        if (video.beginRepeatCheck()) {
            checkPending = true;
            checkSince = framesSinceImage;
            checkPos = imageIntervalPos;
            checkInterval = imageIntervals[imageIntervalPos];
            checkRepeat = framesSinceImage + 1 < imageRepeat();
            newImage = !checkRepeat;
            video.holdFrame(checkRepeat);
        }
        restoreGl();
    }
    countFrame(newImage);
    audio.submit(muted || !audio.ok());
    ++statsFrames;
    ++statsCache.framesRun;
}

int Host::Impl::runFrames(int count, uint64_t start, uint64_t budget) {
    int done = 0;
    while (done < count && !shutdownRequested) {
        runCoreFrame();
        ++done;
        if (SDL_GetTicksNS() - start > budget) break;  // keep the UI responsive if the core can't keep up
    }
    return done;
}

// Smoothed audio queue (seconds). The device drains in chunks, so the raw value is jumpy.
double Host::Impl::trackQueue(double target) {
    double queued = audio.queuedSeconds();
    if (queued > target * 4) {  // far behind (stall, mode switch): resync instead of crawling back
        audio.clear();
        audio.prefill(target);
        queued = target;
        queueEma = -1;
    } else if (queued < target * 0.15) {
        audio.prefill(target * 0.5 - queued);  // underrun: re-pad rather than crackle every frame
        queued = target * 0.5;
    }
    queueEma = queueEma < 0 ? queued : queueEma + (queued - queueEma) * 0.05;
    return queueEma;
}

void Host::Impl::applyDrc(double baseRatio, double target) {
    const double deviation = std::clamp((trackQueue(target) - target) / target, -1.0, 1.0);
    audio.setRatio(baseRatio * (1.0 + kDrcMaxSkew * deviation));  // too full -> play slightly faster
}

bool Host::Impl::runFrame() {
    if (!running || shutdownRequested) return false;
    const uint64_t now = SDL_GetTicksNS();
    refreshDisplaySettings(now, false);

    if (lastPresent) {
        const double dt = (now - lastPresent) * 1e-9;
        if (dt < 0.25) {
            presentEma = presentSamples == 0 ? dt : presentEma + (dt - presentEma) * 0.05;
            ++presentSamples;
        }
    }
    lastPresent = now;

    if (!statsStart) statsStart = now;
    ++statsPresents;
    if (now - statsStart >= 500 * kNsPerMs) {
        const double secs = (now - statsStart) * 1e-9;
        statsCache.measuredFps = statsFrames / secs;
        statsCache.presentFps = statsPresents / secs;
        statsFrames = statsPresents = 0;
        statsStart = now;
    }
    if (now - lastSramCheck >= kSramInterval) {
        lastSramCheck = now;
        saveSram(false);
    }

    if (paused) {
        lastAccount = now;
        accum = 0;
        return true;
    }

    const double fps = coreFps();
    const bool unlimited = turbo && turboPercent <= 0;
    const double speed = unlimited ? 1.0 : speedFactor();
    // Above 2x the audio would only pile up (and sound awful): mute and pace by the clock.
    muted = unlimited || speed > 2.0001;
    if (muted != wasMuted) {
        audio.clear();
        if (!muted) audio.prefill(latency() * 0.75);
        queueEma = -1;
        wasMuted = muted;
    }
    const bool audioPaced = audio.ok() && !muted;
    const double target = latency();
    const bool vsyncReliable = vsyncOn && displayHz > 0 && presentSamples > 30 &&
                               std::fabs(presentEma * displayHz - 1.0) < 0.12;

    if (unlimited) {
        pacing = "unlimited";
        displayPaced = false;
        do {
            runCoreFrame();
        } while (SDL_GetTicksNS() - now < 12 * kNsPerMs && !shutdownRequested);
        lastAccount = SDL_GetTicksNS();
        accum = 0;
    } else if (vsyncReliable) {
        // Presents are locked to the display: advance by whole core frames per vsync.
        const double step = fps * speed / displayHz;
        double emulatedRate;
        int frames;
        pacing = "vsync";
        displayPaced = true;
        if (std::fabs(step - 1.0) < kVsyncLockTolerance) {
            pacing = "vsync-locked";
            frames = 1;  // display ~= core rate: one frame per present, audio absorbs the skew
            accum = 0;
            emulatedRate = displayHz;
        } else {
            accum += step;  // e.g. 120 Hz: 0,1,0,1...; 300% at 60 Hz: 3 per present
            frames = static_cast<int>(accum);
            accum -= frames;
            if (frames > 8) {
                frames = 8;
                accum = 0;
            }
            emulatedRate = fps * speed;
        }
        runFrames(frames, now, 50 * kNsPerMs);
        lastAccount = SDL_GetTicksNS();
        if (audioPaced) applyDrc(emulatedRate / fps, target);
    } else {
        // No usable vsync: a wall-clock frame timer. With audio, the smoothed queue fill nudges
        // the timer by up to 2%, so the audio device clock is the long-term master without the
        // jitter of its chunked consumption. Muted (>200%) or without a device: plain clock.
        double rate = fps * speed;
        pacing = audioPaced ? "timer+audio" : "timer";
        if (audioPaced) {
            audio.setRatio(speed);
            rate *= 1.0 + 0.02 * std::clamp((target - trackQueue(target)) / target, -1.0, 1.0);
        }
        uint64_t t = SDL_GetTicksNS();
        accum += (t - lastAccount) * 1e-9 * rate;
        lastAccount = t;
        // Sleep only if presents aren't throttled by the display already.
        const bool unthrottled = !vsyncOn || displayHz <= 0 || presentEma * displayHz < 0.5;
        displayPaced = !unthrottled;  // else one present per core frame: nothing to smooth
        bool slept = false;
        if (unthrottled && accum < 1.0) {
            SDL_DelayPrecise(static_cast<uint64_t>((1.0 - accum) / rate * 1e9));
            t = SDL_GetTicksNS();
            accum += (t - lastAccount) * 1e-9 * rate;
            lastAccount = t;
            slept = true;
        }
        int frames = static_cast<int>(accum + (slept ? 0.05 : 0.0));  // timer wake-up slop
        accum -= frames;
        const int cap = std::max(4, static_cast<int>(std::ceil(rate / std::max(displayHz, 30.0))) + 2);
        if (frames > cap) {  // can't keep up: drop the backlog instead of spiralling
            frames = cap;
            accum = 0;
        }
        runFrames(frames, now, 50 * kNsPerMs);
    }
    if (now - lastPacingLog >= 5000 * kNsPerMs) {
        lastPacingLog = now;
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                     "[libretro] pacing %s: display %.2f Hz, presents %.1f/s, core %.2f/s, audio queue %.1f ms, ratio %.4f, "
                     "new image every %d core frame(s)",
                     pacing, displayHz, statsCache.presentFps, statsCache.measuredFps, audio.queuedSeconds() * 1000.0,
                     audio.ratio(), imageRepeat());
    }
    return !shutdownRequested;
}

// ================================================================================================
// presentation

double Host::Impl::coreAspect() const {
    if (av.geometry.aspect_ratio > 0) return av.geometry.aspect_ratio;
    if (video.hasFrame()) return static_cast<double>(video.frameWidth()) / video.frameHeight();
    if (av.geometry.base_width && av.geometry.base_height)
        return static_cast<double>(av.geometry.base_width) / av.geometry.base_height;
    return 4.0 / 3.0;
}

double Host::Impl::contentAspect() const {
    const std::string mode = lower(cfg.aspect);
    if (mode == "stretch" || mode == "fit" || mode == "fill") return 0;
    if (mode.empty() || mode == "auto" || mode == "core") return coreAspect();
    const double forced = parseAspect(mode);
    return forced > 0 ? forced : coreAspect();
}

void Host::Impl::fitRect(int& x, int& y, int& w, int& h) const {
    const double aspect = contentAspect();
    if (aspect <= 0) return;
    int dw = 0, dh = 0;
    const unsigned fh = video.frameHeight();
    if (cfg.integerScale && fh) {
        const int scale = static_cast<int>(std::min(static_cast<double>(h) / fh, w / (fh * aspect)));
        if (scale >= 1) {
            dh = static_cast<int>(fh) * scale;
            dw = static_cast<int>(std::lround(dh * aspect));
        }
    }
    if (!dw || !dh) {
        if (static_cast<double>(w) / h > aspect) {
            dh = h;
            dw = static_cast<int>(std::lround(h * aspect));
        } else {
            dw = w;
            dh = static_cast<int>(std::lround(w / aspect));
        }
    }
    x += (w - dw) / 2;
    y += (h - dh) / 2;
    w = std::max(dw, 1);
    h = std::max(dh, 1);
}

void Host::Impl::render(int x, int y, int w, int h) {
    if (!running || w <= 0 || h <= 0) return;
    int fbw = 0, fbh = 0;
    SDL_GetWindowSizeInPixels(window, &fbw, &fbh);
    const int glY = fbh - (y + h);  // callers use top-left origin, GL uses bottom-left
    restoreGl();
    gl.Enable(GL_SCISSOR_TEST);
    gl.Scissor(x, glY, w, h);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Disable(GL_SCISSOR_TEST);
    if (video.hasFrame()) {
        int rx = x, ry = glY, rw = w, rh = h;
        fitRect(rx, ry, rw, rh);
        resolveRepeatCheck(false);  // if the GPU has answered, the newest frame's real status
        float mix = -1.0f;  // < 0: not smoothing
        const int mode = video.smoothingMode();
        const double rate = smoothRate();
        if (smoothingWanted()) {
            const int repeat = imageRepeat();
            // A frame-rate cap: n images per new core image (at most smoothFps a second). Below 2
            // there is no room for an in-between image, so the newest frame is shown as it is.
            const double n = cfg.smoothFps > 0 ? std::round(cfg.smoothFps * repeat / rate) : 0.0;
            if (cfg.smoothFps <= 0 || n >= 2) {
                // Position between the previous image and the newest one on the emulated timeline:
                // after runFrame, accum is the part of a core frame of display time past the newest
                // core frame. (Wall-clock time since the core ran would step unevenly whenever
                // core frames land on an uneven 2/3-refresh cadence, e.g. 60 fps on 144 Hz.)
                double phase = (framesSinceImage + std::clamp(accum, 0.0, 1.0)) / repeat;
                phase = std::min(phase, 1.0);
                if (n >= 2) phase = std::floor(phase * n) / n;
                mix = static_cast<float>(phase);
                if (!blendLogged) {
                    blendLogged = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[libretro] %s frames: display %.1f Hz, core %.2f fps, cap %d%s",
                                mode == 2 ? "interpolating" : "blending", displayHz, rate, cfg.smoothFps,
                                mode < cfg.smoothing ? " (motion interpolation unavailable on this GPU)" : "");
                }
            }
        }
        video.draw(defaultFbo, rx, ry, rw, rh, lower(cfg.filter) == "linear", mix);
        lastRenderW = rw;
        lastRenderH = rh;
    }
    restoreGl();
}

// ================================================================================================
// saves

void Host::Impl::loadSram() {
    void* data = api.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t size = api.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!data || !size) return;
    const std::string path = saveDir + "/" + contentStem + ".srm";
    size_t fileSize = 0;
    if (void* file = SDL_LoadFile(path.c_str(), &fileSize)) {
        std::memcpy(data, file, std::min(size, fileSize));
        SDL_free(file);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[libretro] loaded %s", path.c_str());
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    sramShadow.assign(bytes, bytes + size);
}

void Host::Impl::saveSram(bool force) {
    if (!gameLoaded || !api.get_memory_data) return;
    void* data = api.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t size = api.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!data || !size) return;
    const bool changed = sramShadow.size() != size || std::memcmp(sramShadow.data(), data, size) != 0;
    if (!changed && !(force && sramShadow.empty())) return;
    const std::string path = saveDir + "/" + contentStem + ".srm";
    if (writeFileAtomic(path, data, size)) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        sramShadow.assign(bytes, bytes + size);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[libretro] saved %s", path.c_str());
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[libretro] cannot write %s: %s", path.c_str(), SDL_GetError());
    }
}

bool Host::Impl::saveState(const std::string& path) {
    if (!running) return false;
    const size_t size = api.serialize_size();
    if (!size) return false;
    std::vector<uint8_t> buffer(size);
    enterCore();
    const bool ok = api.serialize(buffer.data(), size);
    leaveCore();
    return ok && writeFileAtomic(path, buffer.data(), size);
}

bool Host::Impl::loadState(const std::string& path) {
    if (!running) return false;
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) return false;
    enterCore();
    const bool ok = api.unserialize(data, size);
    leaveCore();
    SDL_free(data);
    if (ok) {
        audio.clear();
        if (!muted) audio.prefill(latency() * 0.75);
        accum = 0;
    }
    return ok;
}

// ================================================================================================
// public API

Host::Host() : impl_(new Impl) {}

Host::~Host() {
    stop();
    delete impl_;
}

bool Host::start(SDL_Window* window, const HostConfig& config, std::string& error) {
    return impl_->start(window, config, error);
}

void Host::stop() {
    if (impl_->initialized || impl_->lib || impl_->running) impl_->stop();
}

bool Host::running() const { return impl_->running; }

bool Host::handleEvent(const SDL_Event& e) {
    if (!impl_->running) return false;
    if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) impl_->refreshDisplaySettings(SDL_GetTicksNS(), true);
    return impl_->input.handleEvent(e);
}

void Host::setVirtualPad(const PadState& pad) { impl_->input.setVirtualPad(pad); }

int16_t Host::inputState(unsigned port, unsigned device, unsigned index, unsigned id) const {
    return impl_->input.state(port, device, index, id);
}

SDL_JoystickID Host::portGamepad(unsigned port) const { return impl_->running ? impl_->input.padOn(port) : 0; }

bool Host::runFrame() { return impl_->runFrame(); }

void Host::render(int x, int y, int w, int h) { impl_->render(x, y, w, h); }
void Host::stepFrame() {
    if (impl_->running) impl_->runCoreFrame();
}

void Host::setPaused(bool paused) {
    Impl& d = *impl_;
    if (d.paused == paused) return;
    d.paused = paused;
    if (!d.running) return;
    d.audio.clear();
    d.audio.setPaused(paused);
    if (paused) {
        d.input.stopRumble();
        d.input.clearKeys();
    } else {
        // Pads plugged in or out and focus changes while the pause menu was open never reached handleEvent().
        d.input.refresh(!d.window || (SDL_GetWindowFlags(d.window) & SDL_WINDOW_INPUT_FOCUS));
        if (!d.muted) d.audio.prefill(d.latency() * 0.75);
        d.resetPacing();
    }
}

bool Host::paused() const { return impl_->paused; }

void Host::setTurbo(bool on) {
    Impl& d = *impl_;
    if (d.turbo == on) return;
    d.turbo = on;
    d.accum = 0;
    d.queueEma = -1;
}

bool Host::turbo() const { return impl_->turbo; }

void Host::setSpeedPercent(int percent) {
    impl_->speedPercent = std::clamp(percent, 5, 1000);
    impl_->accum = 0;
}

void Host::setTurboPercent(int percent) {
    impl_->turboPercent = std::clamp(percent, 0, 5000);
    impl_->accum = 0;
}

void Host::reset() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.enterCore();
    d.api.reset();
    d.leaveCore();
}

bool Host::saveState(const std::string& path) { return impl_->saveState(path); }

bool Host::loadState(const std::string& path) { return impl_->loadState(path); }

void Host::setCoreOption(const std::string& key, const std::string& value) {
    impl_->options.set(key, value);
    if (impl_->running && impl_->optionsDisplayCb) impl_->optionsDisplayCb();
}

json Host::coreOptionDefinitions() const { return impl_->options.definitions(); }

HostStats Host::stats() const {
    const Impl& d = *impl_;
    HostStats s = d.statsCache;
    s.coreFps = d.av.timing.fps;
    s.sampleRate = d.av.timing.sample_rate;
    s.hwRender = d.hwReady;
    s.coreName = d.libraryName;
    s.coreVersion = d.libraryVersion;
    return s;
}

std::string Host::lastError() const { return impl_->error; }

float Host::displayAspect() const {
    const Impl& d = *impl_;
    double a = d.contentAspect();
    if (a <= 0) a = (d.lastRenderW > 0 && d.lastRenderH > 0) ? static_cast<double>(d.lastRenderW) / d.lastRenderH
                                                             : d.coreAspect();
    return static_cast<float>(a);
}

bool Host::captureFrame(std::vector<uint8_t>& rgba, int& w, int& h) {
    Impl& d = *impl_;
    if (!d.running || !d.video.hasFrame()) return false;
    w = d.lastRenderW > 0 ? d.lastRenderW : static_cast<int>(d.video.frameWidth());
    h = d.lastRenderH > 0 ? d.lastRenderH : static_cast<int>(d.video.frameHeight());
    d.restoreGl();
    const bool ok = d.video.capture(w, h, lower(d.cfg.filter) == "linear", rgba);
    d.restoreGl();
    return ok;
}

}  // namespace rrm::lr
