// rrm_framegrab: a macOS dylib, injected with DYLD_INSERT_LIBRARIES into a backend
// Marathon starts (RPCS3, Fukami, local recomp/port builds), that saves presented
// frames as PNG and logs the presentation rate. Used to verify ultrawide and frame
// pacing on machines where screen recording is not allowed. Hardened-runtime apps
// ignore DYLD_INSERT_LIBRARIES; ad-hoc signed builds accept it.
//
// Environment:
//   RRM_GRAB_DIR    folder for PNGs (required for captures)
//   RRM_GRAB_AT     comma-separated seconds after the first present, e.g. "20,45"
//   RRM_GRAB_EVERY  also capture every N seconds
//   RRM_GRAB_FPS=1  log presents per second to stderr ("[framegrab] fps 59.9 ...")
//   RRM_FRAMEGEN_FPS  frame generation: present up to this many images per second by inserting
//                   motion-compensated in-between frames (framegen.mm) between the app's own
//                   frames; RRM_FRAMEGEN_GAMEFPS = the game's own rate (default 60). Capped by the
//                   display's maximum refresh (of the screen showing the game) unless
//                   RRM_FRAMEGEN_FORCE=1. The images are shown on a layer of our own on top of the
//                   app's. Works with apps that present through a command buffer and with
//                   MoltenVK (which presents the drawable from a scheduled handler of the command
//                   buffer it commits). RRM_FRAMEGEN_DUMP=<dir> saves the real frames and the
//                   generated images of RRM_FRAMEGEN_DUMP_COUNT (default 2) game frames from
//                   RRM_FRAMEGEN_DUMP_AT (default 600). RRM_FRAMEGEN_TRACE=<frame> logs stage
//                   timestamps of 16 frames.
//   RRM_KEYS        timed key presses after the first present, "secs:key[:holdms],...", e.g.
//                   "40:x,42:return,50:left:800". Keys: a-z, 0-9, return, space, escape, tab,
//                   backspace, left, right, up, down. Delivered to the app's largest window, which
//                   is brought to the front first (no accessibility permission involved).
//
// Hooks: -[MTLCommandBuffer presentDrawable:(atTime:/afterMinimumDuration:)], addScheduledHandler:
// and commit on every command buffer class and its private base classes (a blit into a shared
// buffer is encoded just before the present), -[CAMetalDrawable present(AtTime:/
// AfterMinimumDuration:)] for apps that present drawables directly, CGLFlushDrawable for OpenGL,
// and CAMetalLayer's nextDrawable and framebufferOnly (forced off so drawables can be read back).
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include "framegen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

// ---- configuration and timing ---------------------------------------------------------------

struct Config {
    std::string dir;
    std::vector<double> at;
    double every = 0;
    bool fps = false;
};

// The objects below are never destroyed: the game's threads may still present while it exits.
Config& config() {
    static Config& c = *new Config([] {
        Config k;
        if (const char* d = getenv("RRM_GRAB_DIR")) k.dir = d;
        if (const char* a = getenv("RRM_GRAB_AT")) {
            std::string s = a;
            size_t start = 0;
            while (start < s.size()) {
                size_t comma = s.find(',', start);
                if (comma == std::string::npos) comma = s.size();
                double v = atof(s.substr(start, comma - start).c_str());
                if (v > 0) k.at.push_back(v);
                start = comma + 1;
            }
        }
        std::sort(k.at.begin(), k.at.end());
        if (const char* e = getenv("RRM_GRAB_EVERY")) k.every = atof(e);
        if (const char* f = getenv("RRM_GRAB_FPS")) k.fps = *f && *f != '0';
        return k;
    }());
    return c;
}

std::mutex& g_mutex = *new std::mutex;
double g_t0 = -1;          // first present (seconds, CACurrentMediaTime)
size_t g_nextAt = 0;       // index into config().at
double g_nextEvery = 0;
uint64_t g_presents = 0;
auto& g_windowPresents = *new std::vector<std::pair<std::string, uint64_t>>;  // per api, this second
double g_windowStart = 0;
std::atomic<int> g_saved{0};

double now() { return CACurrentMediaTime(); }

// ---- simulated key presses --------------------------------------------------------------------

struct KeyPress {
    double at = 0;
    std::string key;
    int holdMs = 120;
};

std::vector<KeyPress> parseKeys() {
    std::vector<KeyPress> out;
    const char* env = getenv("RRM_KEYS");
    if (!env) return out;
    std::string s = env;
    size_t start = 0;
    while (start < s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) comma = s.size();
        std::string item = s.substr(start, comma - start);
        start = comma + 1;
        size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        KeyPress k;
        k.at = atof(item.substr(0, c1).c_str());
        size_t c2 = item.find(':', c1 + 1);
        k.key = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        if (c2 != std::string::npos) k.holdMs = atoi(item.substr(c2 + 1).c_str());
        out.push_back(k);
    }
    return out;
}

// macOS virtual key code and the characters AppKit reports for it.
bool keyInfo(const std::string& k, unsigned short& code, NSString* __strong& chars, NSEventModifierFlags& flags) {
    static const struct {
        const char* name;
        unsigned short code;
    } kLetters[] = {{"a", 0},  {"s", 1},  {"d", 2},  {"f", 3},  {"h", 4},  {"g", 5},  {"z", 6},  {"x", 7},
                    {"c", 8},  {"v", 9},  {"b", 11}, {"q", 12}, {"w", 13}, {"e", 14}, {"r", 15}, {"y", 16},
                    {"t", 17}, {"1", 18}, {"2", 19}, {"3", 20}, {"4", 21}, {"6", 22}, {"5", 23}, {"9", 25},
                    {"7", 26}, {"8", 28}, {"0", 29}, {"o", 31}, {"u", 32}, {"i", 34}, {"p", 35}, {"l", 37},
                    {"j", 38}, {"k", 40}, {"n", 45}, {"m", 46}};
    flags = 0;
    for (auto& l : kLetters)
        if (k == l.name) {
            code = l.code;
            chars = [NSString stringWithUTF8String:l.name];
            return true;
        }
    struct Special {
        const char* name;
        unsigned short code;
        unichar ch;
        bool function;
    };
    static const Special kSpecial[] = {{"return", 36, '\r', false},       {"space", 49, ' ', false},
                                       {"escape", 53, 0x1b, false},       {"tab", 48, '\t', false},
                                       {"backspace", 51, 0x7f, false},    {"left", 123, 0xF702, true},
                                       {"right", 124, 0xF703, true},      {"down", 125, 0xF701, true},
                                       {"up", 126, 0xF700, true}};
    for (auto& sp : kSpecial)
        if (k == sp.name) {
            code = sp.code;
            chars = [NSString stringWithCharacters:&sp.ch length:1];
            if (sp.function) flags = NSEventModifierFlagFunction | NSEventModifierFlagNumericPad;
            return true;
        }
    return false;
}

NSWindow* targetWindow() {
    NSWindow* best = nil;
    CGFloat bestArea = 0;
    for (NSWindow* w in [NSApp windows]) {
        if (!w.visible) continue;
        CGFloat area = w.frame.size.width * w.frame.size.height;
        if (area > bestArea) {
            bestArea = area;
            best = w;
        }
    }
    return best;
}

void sendKey(const KeyPress& k) {
    unsigned short code = 0;
    NSString* chars = nil;
    NSEventModifierFlags flags = 0;
    if (!keyInfo(k.key, code, chars, flags)) {
        fprintf(stderr, "[framegrab] unknown key '%s'\n", k.key.c_str());
        return;
    }
    NSWindow* win = targetWindow();
    if (!win) {
        fprintf(stderr, "[framegrab] no window for key '%s'\n", k.key.c_str());
        return;
    }
    [NSApp activateIgnoringOtherApps:YES];
    [win makeKeyAndOrderFront:nil];
    auto make = [&](NSEventType type) {
        return [NSEvent keyEventWithType:type
                                location:NSZeroPoint
                           modifierFlags:flags
                               timestamp:[[NSProcessInfo processInfo] systemUptime]
                            windowNumber:win.windowNumber
                                 context:nil
                              characters:chars
             charactersIgnoringModifiers:chars
                               isARepeat:NO
                                 keyCode:code];
    };
    // Through the application's event queue, which every toolkit's event loop drains (SDL reads
    // keys there and never sees events sent to the window directly).
    [NSApp postEvent:make(NSEventTypeKeyDown) atStart:NO];
    fprintf(stderr, "[framegrab] key %s down\n", k.key.c_str());
    NSEvent* up = make(NSEventTypeKeyUp);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)k.holdMs * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      [NSApp postEvent:up atStart:NO];
    });
}

// Scheduled once, at the first present.
void scheduleKeys() {
    for (const KeyPress& k : parseKeys()) {
        KeyPress copy = k;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(k.at * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
          sendKey(copy);
        });
    }
}

// Called once per present. Returns a capture label ("" = no capture this frame); `capture` false
// only counts the present (images frame generation inserts) and leaves a due capture to the next.
std::string onPresent(const char* api, bool capture = true) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const double t = now();
    if (g_t0 < 0) {
        g_t0 = t;
        g_windowStart = t;
        g_nextEvery = config().every;
        fprintf(stderr, "[framegrab] first present via %s\n", api);
        dispatch_async(dispatch_get_main_queue(), ^{
          scheduleKeys();
        });
    }
    ++g_presents;
    auto it = std::find_if(g_windowPresents.begin(), g_windowPresents.end(), [&](auto& p) { return p.first == api; });
    if (it == g_windowPresents.end()) g_windowPresents.emplace_back(api, 1);
    else ++it->second;
    if (config().fps && t - g_windowStart >= 1.0) {
        // e.g. "fps 59.9 (metal)" or, with frame generation, "fps 119.8 (framegen) 59.9 (metal)"
        std::string line;
        char part[64];
        std::sort(g_windowPresents.begin(), g_windowPresents.end());
        for (auto& p : g_windowPresents) {
            snprintf(part, sizeof(part), " %.1f (%s)", p.second / (t - g_windowStart), p.first.c_str());
            line += part;
        }
        fprintf(stderr, "[framegrab] fps%s, %llu presents total\n", line.c_str(), (unsigned long long)g_presents);
        g_windowPresents.clear();
        g_windowStart = t;
    }
    if (config().dir.empty() || !capture) return {};
    const double since = t - g_t0;
    char label[64] = {};
    if (g_nextAt < config().at.size() && since >= config().at[g_nextAt]) {
        snprintf(label, sizeof(label), "t%06.1f", since);
        while (g_nextAt < config().at.size() && config().at[g_nextAt] <= since) ++g_nextAt;
    } else if (config().every > 0 && since >= g_nextEvery) {
        snprintf(label, sizeof(label), "t%06.1f", since);
        while (g_nextEvery <= since) g_nextEvery += config().every;
    }
    return label;
}

// ---- pixel conversion and PNG -----------------------------------------------------------------

inline uint8_t clamp8(float v) { return (uint8_t)std::lround(std::fmin(std::fmax(v, 0.f), 1.f) * 255.f); }

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else {
            float f = std::ldexp((float)mant, -24);
            return sign ? -f : f;
        }
    } else if (exp == 31) bits = sign | 0x7f800000u | (mant << 13);
    else bits = sign | ((exp + 112) << 23) | (mant << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Linear -> sRGB for float formats (extended-range drawables hold linear values).
inline float toSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f; }

// Converts one row of a drawable format to RGBA8. Returns false for formats we don't know.
bool convertRow(MTLPixelFormat fmt, const uint8_t* src, uint8_t* dst, size_t w) {
    switch (fmt) {
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB:
            for (size_t x = 0; x < w; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
            return true;
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatRGBA8Unorm_sRGB:
            for (size_t x = 0; x < w; ++x) {
                memcpy(dst + x * 4, src + x * 4, 3);
                dst[x * 4 + 3] = 255;
            }
            return true;
        case MTLPixelFormatRGB10A2Unorm:
        case MTLPixelFormatBGR10A2Unorm:
            for (size_t x = 0; x < w; ++x) {
                uint32_t p;
                memcpy(&p, src + x * 4, 4);
                uint32_t a = p & 0x3ff, b = (p >> 10) & 0x3ff, c = (p >> 20) & 0x3ff;
                uint32_t r = fmt == MTLPixelFormatRGB10A2Unorm ? a : c, bl = fmt == MTLPixelFormatRGB10A2Unorm ? c : a;
                dst[x * 4 + 0] = (uint8_t)(r >> 2);
                dst[x * 4 + 1] = (uint8_t)(b >> 2);
                dst[x * 4 + 2] = (uint8_t)(bl >> 2);
                dst[x * 4 + 3] = 255;
            }
            return true;
        case MTLPixelFormatRGBA16Float:
            for (size_t x = 0; x < w; ++x) {
                const uint16_t* h = (const uint16_t*)(src + x * 8);
                for (int c = 0; c < 3; ++c) dst[x * 4 + c] = clamp8(toSrgb(halfToFloat(h[c])));
                dst[x * 4 + 3] = 255;
            }
            return true;
        default:
            return false;
    }
}

size_t bytesPerPixel(MTLPixelFormat fmt) { return fmt == MTLPixelFormatRGBA16Float ? 8 : 4; }

void writePng(const std::string& path, const std::vector<uint8_t>& rgba, size_t w, size_t h) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, rgba.data(), rgba.size(), nullptr);
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp,
                                   nullptr, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), path.size(), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dest && img) {
        CGImageDestinationAddImage(dest, img, nullptr);
        ok = CGImageDestinationFinalize(dest);
    }
    if (dest) CFRelease(dest);
    CFRelease(url);
    if (img) CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    fprintf(stderr, "[framegrab] %s %s (%zux%zu)\n", ok ? "saved" : "FAILED to save", path.c_str(), w, h);
    if (ok) ++g_saved;
}

std::string pngPath(const std::string& label, const char* api, const std::string& dir = config().dir) {
    char name[160];
    snprintf(name, sizeof(name), "/%s-%s-%d.png", label.c_str(), api, (int)getpid());
    return dir + name;
}

// Copies a drawable texture into a shared buffer inside `cb`, then writes the PNG when it completes.
void encodeCapture(id<MTLCommandBuffer> cb, id<MTLTexture> tex, const std::string& label, const char* api,
                   const std::string& dir = config().dir) {
    if (!tex || tex.framebufferOnly) {
        fprintf(stderr, "[framegrab] %s: texture is framebufferOnly, cannot capture\n", label.c_str());
        return;
    }
    const MTLPixelFormat fmt = tex.pixelFormat;
    const size_t w = tex.width, h = tex.height, bpp = bytesPerPixel(fmt), rowBytes = w * bpp;
    id<MTLBuffer> buf = [cb.device newBufferWithLength:rowBytes * h options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (!buf || !blit) return;
    [blit copyFromTexture:tex
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:buf
               destinationOffset:0
          destinationBytesPerRow:rowBytes
        destinationBytesPerImage:rowBytes * h];
    [blit endEncoding];
    std::string path = pngPath(label, api, dir);
    // Encoded on a queue of our own: completion handlers run one at a time on a queue Metal shares
    // with the app's own handlers (MoltenVK's presentation among them).
    static dispatch_queue_t writer = dispatch_queue_create("io.github.ridgeracermarathon.png", DISPATCH_QUEUE_SERIAL);
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
      dispatch_async(writer, ^{
        std::vector<uint8_t> rgba(w * h * 4);
        const uint8_t* src = (const uint8_t*)buf.contents;
        for (size_t y = 0; y < h; ++y)
            if (!convertRow(fmt, src + y * rowBytes, rgba.data() + y * w * 4, w)) {
                fprintf(stderr, "[framegrab] unsupported pixel format %lu\n", (unsigned long)fmt);
                return;
            }
        writePng(path, rgba, w, h);
      });
    }];
}

// ---- Metal hooks ---------------------------------------------------------------------------------
// Installed with block trampolines on every class that implements the method itself (Metal's
// command buffer classes plus the GPU driver's own, the drawable classes, CAMetalLayer), each
// capturing that class's original implementation, so a subclass calling super reaches the right
// code. Calls made while one of our hooks is running pass straight through (t_inHook).

const void* kCountedKey = &kCountedKey;      // drawable counted and captured at its command-buffer present
const void* kPassKey = &kPassKey;            // drawable of our own layer (frame generation)
const void* kPushedKey = &kPushedKey;        // app drawable already handed to frame generation
const void* kScheduledKey = &kScheduledKey;  // command buffer that got a scheduled handler (MoltenVK present)

thread_local int t_inHook = 0;
struct HookScope {
    HookScope() { ++t_inHook; }
    ~HookScope() { --t_inHook; }
};

void captureInCommandBuffer(id<MTLCommandBuffer> cb, id<MTLDrawable> drawable) {
    // The command buffer presents the drawable itself later: count it here only.
    objc_setAssociatedObject(drawable, kCountedKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    std::string label = onPresent("metal");
    if (label.empty() || ![drawable conformsToProtocol:@protocol(CAMetalDrawable)]) return;
    encodeCapture(cb, ((id<CAMetalDrawable>)drawable).texture, label, "metal");
}

id<MTLCommandQueue> g_captureQueue = nil;

// Drawables presented directly (not through a command buffer): read back on our own queue.
void captureDirect(id drawable, const char* api) {
    if (objc_getAssociatedObject(drawable, kCountedKey)) return;  // counted via its command buffer
    static std::atomic<int> traced{0};
    if (getenv("RRM_GRAB_DEBUG") && traced.fetch_add(1) == 0)
        fprintf(stderr, "[framegrab] direct present from:\n%s\n", [[NSThread callStackSymbols] description].UTF8String);
    std::string label = onPresent(api);
    if (label.empty()) return;
    id<MTLTexture> tex = ((id<CAMetalDrawable>)drawable).texture;
    if (!g_captureQueue) g_captureQueue = [tex.device newCommandQueue];
    id<MTLCommandBuffer> cb = [g_captureQueue commandBuffer];
    encodeCapture(cb, tex, label, api);
    [cb commit];
    [cb waitUntilCompleted];
}

// ---- frame generation ----------------------------------------------------------------------------
// The app keeps presenting its own frames exactly as it would without us; all we add to the
// command buffer that presents frame n is a signal of g_rendered = n at its end. On our own queue,
// a command buffer waits for that, copies the frame into the ring and runs the motion search. The
// generated images go to our own layer, an opaque sublayer covering the app's, so the app's
// drawables, presents and pacing are untouched: a private serial queue ("presenter") renders frame
// n's in-between images and then frame n itself (copied back from the ring) into that layer's
// drawables, each for one slot, on the same queue (so after the copy). Nothing of the app's ever
// waits for us (GPU waits placed in MoltenVK's command buffers deadlocked it at swapchain changes,
// and so did holding back its drawables). The presenter checks at each commit that the frames it
// reads are still in the ring (framegen.h), and skips a frame when two newer ones are waiting.

struct FramegenConfig {
    double targetFps = 0, gameFps = 60;
    bool force = false;
    std::string dump;
    uint64_t dumpAt = 600, dumpCount = 2;
};

const FramegenConfig& framegenConfig() {
    static const FramegenConfig& c = *new FramegenConfig([] {
        FramegenConfig k;
        if (const char* v = getenv("RRM_FRAMEGEN_FPS")) k.targetFps = atof(v);
        if (const char* v = getenv("RRM_FRAMEGEN_GAMEFPS")) k.gameFps = std::max(1.0, atof(v));
        if (const char* v = getenv("RRM_FRAMEGEN_FORCE")) k.force = *v && *v != '0';
        if (const char* v = getenv("RRM_FRAMEGEN_DUMP")) k.dump = v;
        if (const char* v = getenv("RRM_FRAMEGEN_DUMP_AT")) k.dumpAt = strtoull(v, nullptr, 10);
        if (const char* v = getenv("RRM_FRAMEGEN_DUMP_COUNT")) k.dumpCount = strtoull(v, nullptr, 10);
        return k;
    }());
    return c;
}

// RRM_FRAMEGEN_TRACE=<first frame>: timestamps of each stage for 16 frames from there (debugging).
bool fgTracing() {
    static const bool on = getenv("RRM_FRAMEGEN_TRACE") != nullptr;
    return on;
}

void fgTrace(uint64_t n, const char* what, double extra = 0) {
    static const long long first = [] {
        const char* v = getenv("RRM_FRAMEGEN_TRACE");
        return v ? atoll(v) : -1LL;
    }();
    if (first < 0 || (long long)n < first || (long long)n >= first + 16) return;
    fprintf(stderr, "[fgtrace] %.3f frame %llu %s %.3f\n", CACurrentMediaTime() * 1000.0, (unsigned long long)n, what, extra);
}

void fgTraceP(uint64_t n, const char* what, const void* p) {
    static const long long first = [] {
        const char* v = getenv("RRM_FRAMEGEN_TRACE");
        return v ? atoll(v) : -1LL;
    }();
    if (first < 0 || (long long)n < first || (long long)n >= first + 16) return;
    fprintf(stderr, "[fgtrace] %.3f frame %llu %s %p\n", CACurrentMediaTime() * 1000.0, (unsigned long long)n, what, p);
}

std::atomic<int> g_displayHz{0};  // the game window's screen, read on the main thread (watchDisplay)

int displayHz() {
    if (int hz = g_displayHz.load(); hz > 0) return hz;
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGMainDisplayID());
    double rate = mode ? CGDisplayModeGetRefreshRate(mode) : 0;
    if (mode) CGDisplayModeRelease(mode);
    return rate > 1 ? (int)std::lround(rate) : 60;
}

// Re-reads (at most once a second, on the main thread) the maximum refresh of the screen showing
// `layer`, so a game window moved to another display gets that display's rate.
void watchDisplay(CAMetalLayer* layer) {
    static std::atomic<double> last{-10.0};
    const double t = now();
    if (t - last.load() < 1.0) return;
    last = t;
    __weak CAMetalLayer* weakLayer = layer;
    dispatch_async(dispatch_get_main_queue(), ^{
      CAMetalLayer* l = weakLayer;
      NSWindow* win = [l.delegate isKindOfClass:[NSView class]] ? ((NSView*)l.delegate).window : nil;
      NSScreen* screen = (win ? win : targetWindow()).screen;
      if (!screen) screen = [NSScreen mainScreen];
      if (@available(macOS 12.0, *))
          if (screen.maximumFramesPerSecond > 0) g_displayHz = (int)screen.maximumFramesPerSecond;
    });
}

// Images per game frame: the target rate over the game's, no more than the display can show.
int framegenMultiplier() {
    const FramegenConfig& c = framegenConfig();
    if (c.targetFps <= 0) return 1;
    int n = (int)std::floor(c.targetFps / c.gameFps + 0.02);  // (never above the cap; 119.88 Hz counts as 120)
    if (!c.force) n = std::min(n, (int)std::floor(displayHz() / c.gameFps + 0.02));
    return std::clamp(n, 1, 8);
}

// How long each of the `count` images stays up: its share of a game frame rounded down to whole
// refreshes, minus half a refresh so the request never spills into the next one (a share of
// 8.33 ms is one refresh at 120 Hz and on a fixed 144 Hz screen, not two). Shares shorter than a
// refresh (RRM_FRAMEGEN_FORCE) are requested as they are.
double framegenSlot(int count) {
    const double share = 1.0 / (framegenConfig().gameFps * std::max(count, 1));
    const double refresh = 1.0 / displayHz();
    const double periods = std::floor(share / refresh + 1e-3);
    return periods >= 1 ? (periods - 0.5) * refresh : share;
}

std::mutex& g_fgMutex = *new std::mutex;  // g_framegen and the objects below
rrm::fg::FrameGen& g_framegen = *new rrm::fg::FrameGen;
std::atomic<uint64_t> g_framegenFrames{0};
__weak id g_lastAcquired = nil;      // newest drawable the app acquired (MoltenVK presents it later)
CAMetalLayer* g_overlay = nil;       // our layer (set on the main thread once it is in the layer tree)
__weak CAMetalLayer* g_host = nil;   // the app layer it covers
double g_hostSeen = 0;               // last time the host presented a frame
bool g_afterGap[16] = {};            // frame n in slot n % 16: came long after the one before
bool g_overlayRequested = false;
std::atomic<bool> g_framegenOff{false};
id<MTLCommandQueue> g_fgQueue = nil;
id<MTLSharedEvent> g_rendered = nil;
dispatch_queue_t g_presenter = nullptr;

// Turns frame generation off for good (the app's own layer shows again). Any thread.
void dropOverlay();
void framegenStop(const char* why) {
    if (g_framegenOff.exchange(true)) return;
    fprintf(stderr, "[framegen] off: %s\n", why);
    std::lock_guard<std::mutex> lock(g_fgMutex);
    dropOverlay();
}

// Adds (on the main thread) our layer on top of `host`: same device, format, color space, size and
// scale, opaque. It has no contents until our first present, so the app's layer shows until then.
// Called with g_fgMutex held.
void requestOverlay(CAMetalLayer* host) {
    if (g_overlayRequested && g_host == host) return;
    CAMetalLayer* old = g_overlay;
    g_overlayRequested = true;
    g_host = host;
    g_overlay = nil;
    __weak CAMetalLayer* weakHost = host;
    dispatch_async(dispatch_get_main_queue(), ^{
      CAMetalLayer* h = weakHost;
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      [old removeFromSuperlayer];
      std::lock_guard<std::mutex> lock(g_fgMutex);
      if (h && g_host == h && g_overlayRequested && !g_framegenOff) {
          CAMetalLayer* o = [CAMetalLayer layer];
          o.name = @"rrm-framegen";
          o.device = h.device;
          o.pixelFormat = h.pixelFormat;
          o.colorspace = h.colorspace;
          o.wantsExtendedDynamicRangeContent = h.wantsExtendedDynamicRangeContent;
          o.framebufferOnly = NO;
          o.opaque = YES;
          o.contentsScale = h.contentsScale;
          o.contentsGravity = h.contentsGravity;
          o.frame = h.bounds;
          o.drawableSize = h.drawableSize;
          o.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
          [h addSublayer:o];
          g_overlay = o;
      }
      [CATransaction commit];
    });
}

// Removes our layer (the app's shows again); frame generation adds it back when it next runs.
// Called with g_fgMutex held.
void dropOverlay() {
    CAMetalLayer* o = g_overlay;
    g_overlay = nil;
    g_overlayRequested = false;
    g_host = nil;
    if (!o) return;
    dispatch_async(dispatch_get_main_queue(), ^{
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      [o removeFromSuperlayer];
      [CATransaction commit];
    });
}

// The app presented one of its drawables: if frame generation is showing our layer but did not get
// this frame, our layer is falling behind the game; after a run of such frames, step aside.
void noteAppPresent(id drawable) {
    static std::atomic<int> missed{0};
    if (![drawable conformsToProtocol:@protocol(CAMetalDrawable)] || ((id<CAMetalDrawable>)drawable).layer != g_host)
        return;  // another layer of the app (or none chosen yet)
    if (objc_getAssociatedObject(drawable, kPushedKey)) {
        missed = 0;
        return;
    }
    if (g_framegenFrames == 0 || ++missed < 30) return;
    missed = 0;
    std::lock_guard<std::mutex> lock(g_fgMutex);
    if (g_overlay) {
        fprintf(stderr, "[framegen] the game's frames bypass frame generation: showing them directly\n");
        dropOverlay();
    }
}

// Makes our layer's geometry and format the app layer's again (main thread), and clears its
// picture meanwhile so the app's own layer shows through (a live window resize keeps moving).
void syncOverlay(CAMetalLayer* o, CGSize drawableSize, MTLPixelFormat format) {
    __weak CAMetalLayer* weak = o;
    dispatch_async(dispatch_get_main_queue(), ^{
      CAMetalLayer* l = weak;
      CAMetalLayer* h = (CAMetalLayer*)l.superlayer;
      if (!l || !h) return;
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      l.contents = nil;
      l.frame = h.bounds;
      l.contentsScale = h.contentsScale;
      l.pixelFormat = format;
      l.colorspace = h.colorspace;
      l.wantsExtendedDynamicRangeContent = h.wantsExtendedDynamicRangeContent;
      l.drawableSize = drawableSize;
      [CATransaction commit];
    });
}

void framegenQueue(uint64_t n);

// The app is about to present `drawable` from command buffer `appCb` (not yet committed): copy the
// frame once that has run, and queue its images to the presenter.
void framegenFrame(id<MTLCommandBuffer> appCb, id drawable) {
    if (g_framegenOff || ![drawable conformsToProtocol:@protocol(CAMetalDrawable)]) return;
    id<CAMetalDrawable> frame = (id<CAMetalDrawable>)drawable;
    if (objc_getAssociatedObject(frame, kPushedKey) || objc_getAssociatedObject(frame, kPassKey)) return;
    id<MTLTexture> tex = frame.texture;
    CAMetalLayer* layer = frame.layer;
    if (!tex || tex.framebufferOnly || !layer) return;
    watchDisplay(layer);  // (before the test below: the window may be on a faster display)
    const double t = now();
    std::lock_guard<std::mutex> lock(g_fgMutex);
    if (framegenMultiplier() < 2) {  // e.g. the window moved to a slower display
        if (g_overlayRequested) dropOverlay();
        return;
    }
    // One layer at a time: another one of the app's takes over only once this one stops presenting.
    CAMetalLayer* host = g_host;
    if (host && host != layer && t - g_hostSeen < 1.0) return;
    g_hostSeen = t;
    if (host != layer || (!g_overlay && !g_overlayRequested)) requestOverlay(layer);
    if (!g_overlay) return;  // not in the layer tree yet
    if (!g_framegen.ensure(appCb.device, tex.pixelFormat, tex.width, tex.height)) return;
    if (!g_fgQueue) {
        g_fgQueue = [appCb.device newCommandQueue];
        g_rendered = [appCb.device newSharedEvent];
        g_presenter = dispatch_queue_create("io.github.ridgeracermarathon.present", DISPATCH_QUEUE_SERIAL);
    }
    const uint64_t n = g_framegen.lastPushed() + 1;
    // A frame long after the one before (loading, a pause) is shown as it is, not blended with it.
    static double lastPush = 0;
    g_afterGap[n % 16] = lastPush > 0 && t - lastPush > 2.5 / framegenConfig().gameFps;
    lastPush = t;
    [appCb encodeSignalEvent:g_rendered value:n];
    id<MTLCommandBuffer> cb = [g_fgQueue commandBuffer];
    [cb encodeWaitForEvent:g_rendered value:n];
    if (g_framegen.push(cb, tex) != n) return;  // (cannot happen: ensure() matched the size)
    objc_setAssociatedObject(frame, kPushedKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    const FramegenConfig& c = framegenConfig();
    if (!c.dump.empty() && n >= c.dumpAt && n < c.dumpAt + c.dumpCount) {  // every real frame of the range
        char label[64];
        snprintf(label, sizeof(label), "frame%06llu-real", (unsigned long long)n);
        encodeCapture(cb, tex, label, "framegen", c.dump);
    }
    fgTraceP(n, "pushed", (__bridge void*)frame);
    if (fgTracing())
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
          fgTrace(n, "copied", (done.GPUEndTime - done.GPUStartTime) * 1000.0);
        }];
    [cb commit];
    framegenQueue(n);
}

// Presents frame n's in-between images, then frame n itself, on our layer (see above).
void framegenQueue(uint64_t n) {
    const FramegenConfig& c = framegenConfig();
    const int count = framegenMultiplier();
    const double slot = framegenSlot(count);
    dispatch_async(g_presenter, ^{
      @autoreleasepool {
          HookScope scope;  // our own acquisitions and presents are not the app's
          fgTrace(n, "presenter start");
          const bool dump = !c.dump.empty() && n >= c.dumpAt && n < c.dumpAt + c.dumpCount;
          bool interpolate, behind;
          CAMetalLayer* overlay;
          CGSize size;
          MTLPixelFormat format;
          {
              std::lock_guard<std::mutex> lock(g_fgMutex);
              interpolate = g_framegen.canRender(n) && !g_afterGap[n % 16];
              behind = g_framegen.lastPushed() >= n + 2;  // two newer frames waiting: skip to them
              overlay = g_overlay;
              size = CGSizeMake(g_framegen.width(), g_framegen.height());
              format = g_framegen.format();
          }
          if (overlay && (!CGSizeEqualToSize(overlay.drawableSize, size) || overlay.pixelFormat != format)) {
              syncOverlay(overlay, size, format);
              overlay = nil;  // shown from the next frame on
          }
          const int images = interpolate ? count : 1;
          static int timeouts = 0;
          char label[64];
          for (int k = 1; overlay && !behind && k <= images; ++k) {
              id<CAMetalDrawable> target = [overlay nextDrawable];  // waits for one of its three to free up
              fgTrace(n, "acquired", k);
              if (!target) {
                  if (++timeouts >= 3) framegenStop("our layer gives no drawables");
                  break;
              }
              timeouts = 0;
              objc_setAssociatedObject(target, kPassKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
              // Checked, encoded and committed under the lock pushes take, on the queue they use:
              // the GPU runs this before any later frame's copy into the ring (framegen.h).
              std::lock_guard<std::mutex> lock(g_fgMutex);
              const bool generated = k < images;
              if (!(generated ? g_framegen.canRender(n) : g_framegen.canCopy(n))) break;  // overwritten meanwhile
              id<MTLCommandBuffer> cb = [g_fgQueue commandBuffer];
              if (generated)
                  g_framegen.render(cb, target.texture, n, (float)k / images);
              else if (!g_framegen.copyFrame(cb, target.texture, n))
                  break;  // the layer's size changed
              if (generated && dump) {  // (the real frames are saved by framegenFrame)
                  snprintf(label, sizeof(label), "frame%06llu-gen%d-of-%d", (unsigned long long)n, k, images);
                  encodeCapture(cb, target.texture, label, "framegen", c.dump);
              }
              [cb presentDrawable:target afterMinimumDuration:slot];
              if (fgTracing()) {
                  const int image = k;
                  [target addPresentedHandler:^(id<MTLDrawable>) { fgTrace(n, "on screen", image); }];
              }
              [cb commit];
          }
      }
    });
    if (++g_framegenFrames == 1)
        fprintf(stderr, "[framegen] active: %d images per game frame (%.0f fps game, slot %.2f ms, display %d Hz)\n",
                count, framegenConfig().gameFps, slot * 1000.0, displayHz());
}

// ---- the hooks -----------------------------------------------------------------------------------

using CbPresentFn = void (*)(id, SEL, id);
using CbPresentTimeFn = void (*)(id, SEL, id, CFTimeInterval);
using VoidFn = void (*)(id, SEL);
using BlockArgFn = void (*)(id, SEL, id);
using DrPresentTimeFn = void (*)(id, SEL, CFTimeInterval);
using IdFn = id (*)(id, SEL);
using SetBoolFn = void (*)(id, SEL, BOOL);

// The app presents `drawable` through command buffer `cb`; the app may still encode work into it,
// so the frame is counted, captured and handed to frame generation at its commit (below).
const void* kCbDrawableKey = &kCbDrawableKey;
void onCbPresent(id cb, id drawable) {
    objc_setAssociatedObject(cb, kCbDrawableKey, drawable, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

// A drawable is presented directly: ours (count only), already counted at its command buffer, or
// read back on our own queue.
void onDrawablePresent(id drawable) {
    if (objc_getAssociatedObject(drawable, kPassKey)) {
        onPresent("framegen", false);
        return;
    }
    if (!objc_getAssociatedObject(drawable, kCountedKey)) noteAppPresent(drawable);  // (else seen at its command buffer)
    captureDirect(drawable, "metal-direct");
}

// Classes that conform to `protocol` (themselves or through a superclass), plus all their
// superclasses below NSObject: the implementation often lives in a private base class (e.g.
// Metal's _MTLCommandBuffer) that does not declare the protocol itself.
std::vector<Class> classesConformingTo(Protocol* protocol) {
    std::vector<Class> out;
    auto add = [&](Class c) {
        if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    };
    unsigned count = 0;
    Class* all = objc_copyClassList(&count);
    for (unsigned i = 0; i < count; ++i) {
        bool conforms = false;
        for (Class c = all[i]; c && !conforms; c = class_getSuperclass(c)) conforms = class_conformsToProtocol(c, protocol);
        if (!conforms) continue;
        for (Class c = all[i]; c && c != [NSObject class]; c = class_getSuperclass(c)) add(c);
    }
    free(all);
    return out;
}

// Replaces `sel` on every class in `classes` that implements it itself.
int hookEach(const std::vector<Class>& classes, SEL sel, id (^make)(IMP original)) {
    int hooked = 0;
    for (Class c : classes) {
        unsigned count = 0;
        Method* methods = class_copyMethodList(c, &count);
        for (unsigned i = 0; i < count; ++i)
            if (method_getName(methods[i]) == sel) {
                IMP original = method_getImplementation(methods[i]);
                method_setImplementation(methods[i], imp_implementationWithBlock(make(original)));
                ++hooked;
                if (getenv("RRM_GRAB_DEBUG"))
                    fprintf(stderr, "[framegrab] hooked -[%s %s]\n", class_getName(c), sel_getName(sel));
            }
        free(methods);
    }
    return hooked;
}

void installMetalHooks() {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();  // loads the GPU driver's classes
    if (!dev) return;
    const std::vector<Class> buffers = classesConformingTo(@protocol(MTLCommandBuffer));
    const std::vector<Class> drawables = classesConformingTo(@protocol(CAMetalDrawable));
    const std::vector<Class> layers = {[CAMetalLayer class]};
    int n = 0;

    n += hookEach(buffers, @selector(presentDrawable:), ^id(IMP original) {
        return ^(id self, id drawable) {
            if (t_inHook) return ((CbPresentFn)original)(self, @selector(presentDrawable:), drawable);
            HookScope scope;
            onCbPresent(self, drawable);
            ((CbPresentFn)original)(self, @selector(presentDrawable:), drawable);
        };
    });
    for (SEL sel : {@selector(presentDrawable:atTime:), @selector(presentDrawable:afterMinimumDuration:)}) {
        n += hookEach(buffers, sel, ^id(IMP original) {
            return ^(id self, id drawable, CFTimeInterval t) {
                if (t_inHook) return ((CbPresentTimeFn)original)(self, sel, drawable, t);
                HookScope scope;
                onCbPresent(self, drawable);
                ((CbPresentTimeFn)original)(self, sel, drawable, t);
            };
        });
    }
    // MoltenVK presents from a block it adds as a scheduled handler; mark that command buffer.
    n += hookEach(buffers, @selector(addScheduledHandler:), ^id(IMP original) {
        return ^(id self, id block) {
            if (!t_inHook) objc_setAssociatedObject(self, kScheduledKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            ((BlockArgFn)original)(self, @selector(addScheduledHandler:), block);
        };
    });
    // At commit, after everything the app encoded: the drawable it presents through this command
    // buffer, or (MoltenVK, which presents from a scheduled handler) its newest drawable, is
    // counted, captured and pushed for frame generation.
    n += hookEach(buffers, @selector(commit), ^id(IMP original) {
        return ^(id self) {
            id presented = t_inHook ? nil : objc_getAssociatedObject(self, kCbDrawableKey);
            if (!t_inHook && (presented || objc_getAssociatedObject(self, kScheduledKey))) {
                HookScope scope;
                id frame = presented;
                if (!frame) {
                    std::lock_guard<std::mutex> lock(g_fgMutex);
                    frame = g_lastAcquired;
                    g_lastAcquired = nil;
                }
                fgTraceP(g_framegen.lastPushed() + 1, "scheduled commit, frame", (__bridge void*)frame);
                if (frame && !objc_getAssociatedObject(frame, kCountedKey)) {
                    captureInCommandBuffer(self, frame);
                    framegenFrame(self, frame);
                    noteAppPresent(frame);
                }
            }
            ((VoidFn)original)(self, @selector(commit));
        };
    });

    n += hookEach(drawables, @selector(present), ^id(IMP original) {
        return ^(id self) {
            if (t_inHook) return ((VoidFn)original)(self, @selector(present));
            HookScope scope;
            onDrawablePresent(self);
            ((VoidFn)original)(self, @selector(present));
        };
    });
    for (SEL sel : {@selector(presentAtTime:), @selector(presentAfterMinimumDuration:)}) {
        n += hookEach(drawables, sel, ^id(IMP original) {
            return ^(id self, CFTimeInterval t) {
                if (t_inHook) return ((DrPresentTimeFn)original)(self, sel, t);
                HookScope scope;
                onDrawablePresent(self);
                ((DrPresentTimeFn)original)(self, sel, t);
            };
        });
    }

    // Readable drawables (the default is write-only) so frames can be copied; remember the app's.
    __block SetBoolFn setFramebufferOnly = nullptr;
    n += hookEach(layers, @selector(setFramebufferOnly:), ^id(IMP original) {
        setFramebufferOnly = (SetBoolFn)original;
        return ^(id self, BOOL) { ((SetBoolFn)original)(self, @selector(setFramebufferOnly:), NO); };
    });
    n += hookEach(layers, @selector(nextDrawable), ^id(IMP original) {
        return ^id(id self) {
            if (((CAMetalLayer*)self).framebufferOnly && setFramebufferOnly)
                setFramebufferOnly(self, @selector(setFramebufferOnly:), NO);
            id d = ((IdFn)original)(self, @selector(nextDrawable));
            if (d && !t_inHook) {
                std::lock_guard<std::mutex> lock(g_fgMutex);
                g_lastAcquired = d;
                fgTraceP(g_framegen.lastPushed() + 1, "app acquired", (__bridge void*)d);
            }
            static std::atomic<int> logged{0};
            if (d && getenv("RRM_GRAB_DEBUG") && logged.fetch_add(1) < 3)
                fprintf(stderr, "[framegrab] nextDrawable -> %s (layer %s)\n", class_getName([d class]),
                        class_getName([self class]));
            return d;
        };
    });
    fprintf(stderr, "[framegrab] %d metal hooks on %zu command buffer and %zu drawable classes\n", n, buffers.size(),
            drawables.size());
}

}  // namespace

// ---- OpenGL: interpose CGLFlushDrawable -------------------------------------------------------------

extern "C" CGLError rrm_CGLFlushDrawable(CGLContextObj ctx) {
    std::string label = onPresent("opengl");
    if (!label.empty() && CGLGetCurrentContext() == ctx) {
        // The whole drawable: the view's size in pixels, else (no view) the current viewport.
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        NSView* view = [NSOpenGLContext currentContext].view;
        if (view) {
            // Points unless the view asked for a Retina surface.
            const NSRect px = view.wantsBestResolutionOpenGLSurface ? [view convertRectToBacking:view.bounds] : view.bounds;
            vp[0] = vp[1] = 0;
            vp[2] = (GLint)px.size.width;
            vp[3] = (GLint)px.size.height;
        }
        GLint fbo = 0, readFbo = 0, readBuffer = 0, packAlignment = 4;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGetIntegerv(GL_READ_BUFFER, &readBuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
        const int w = vp[2], h = vp[3];
        if (w > 0 && h > 0) {
            std::vector<uint8_t> raw((size_t)w * h * 4), rgba((size_t)w * h * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(vp[0], vp[1], w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
            for (int y = 0; y < h; ++y) memcpy(rgba.data() + (size_t)y * w * 4, raw.data() + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            writePng(pngPath(label, "opengl"), rgba, (size_t)w, (size_t)h);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readFbo);
            glReadBuffer((GLenum)readBuffer);
            glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
        }
    }
    return CGLFlushDrawable(ctx);
}

__attribute__((used)) static struct {
    const void* replacement;
    const void* original;
} g_interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void*)&rrm_CGLFlushDrawable, (const void*)&CGLFlushDrawable},
};

__attribute__((constructor)) static void rrmFramegrabInit() {
    @autoreleasepool {
        installMetalHooks();
    }
    fprintf(stderr, "[framegrab] loaded in pid %d (dir=%s, %zu capture times, every=%.1f, fps=%d)\n", (int)getpid(),
            config().dir.c_str(), config().at.size(), config().every, (int)config().fps);
}
