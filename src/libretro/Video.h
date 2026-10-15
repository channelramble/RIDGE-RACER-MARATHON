// Core video: the FBO handed to hardware-rendered cores, the texture for software
// frames, and the quad shader that presents either into a viewport.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "libretro.h"
#include "libretro/GlFunctions.h"
#include "libretro/Interpolator.h"

namespace rrm::lr {

class Video {
public:
    bool init(GlFunctions* gl, bool gles, std::string& error);
    // Deletes every GL object we own (deleteObjects=false just forgets them, for a lost context).
    void shutdown(bool deleteObjects = true);

    // Render target for RETRO_ENVIRONMENT_SET_HW_RENDER cores.
    bool createHwTarget(unsigned w, unsigned h, bool depth, bool stencil, std::string& error);
    // Grows the target in place (same GL names), so it is safe while the core runs.
    void growHwTarget(unsigned w, unsigned h);
    GLuint hwFramebuffer() const { return hwFbo_; }
    unsigned hwWidth() const { return hwW_; }
    unsigned hwHeight() const { return hwH_; }

    // Frames from retro_video_refresh_t (NULL/dupe frames simply aren't submitted). True when
    // the frame is a new image: a software frame identical to the previous one counts as a dupe.
    bool submitSoftware(const void* data, unsigned w, unsigned h, size_t pitch, retro_pixel_format fmt);
    bool submitHardware(unsigned w, unsigned h, bool bottomLeftOrigin);
    // Identical hardware frames (30 fps games) can only be found on the GPU, and waiting for its
    // answer would stall every core frame. After retro_run, beginRepeatCheck() queues a compare
    // of the new hardware frame with the previous one (false: nothing to compare with, or the
    // check is unavailable); repeatCheckResult() then gives 1 = same image, 0 = new image, -1 =
    // not known yet (or nothing pending). Normally the answer is there by the next present.
    bool beginRepeatCheck();
    int repeatCheckResult(bool wait);
    // Forgets a pending check whose answer is not needed any more (the guess stands).
    void cancelRepeatCheck() { checkPending_ = false; }
    // Keeps the newest frame out of the smoothing history (a repeat). Too late once drawn there;
    // the next submitted frame clears it.
    void holdFrame(bool hold) { held_ = hold; }
    bool hasFrame() const { return frameW_ && frameH_; }
    unsigned frameWidth() const { return frameW_; }
    unsigned frameHeight() const { return frameH_; }

    // High-refresh smoothing between the previous core frame and the newest one:
    // 0 = off, 1 = crossfade, 2 = motion-compensated interpolation (Interpolator.h).
    void setSmoothing(int mode) { smoothing_ = mode; }
    // The mode this GPU can actually do (interpolation falls back to a crossfade without
    // half-float targets, everything to 0 without the blend shader).
    int smoothingMode() const { return !smoothing_ || !blendProgram_ ? 0 : smoothing_ == 2 && interpReady_ ? 2 : 1; }
    // Draws the frame into viewport (x, y, w, h) of `fbo` (GL convention: y from the bottom).
    // mix: 0 = the previous core frame, 1 = the newest; < 0 = not smoothing this present (the
    // newest frame is drawn directly, without the history copy or motion estimation).
    void draw(GLuint fbo, int x, int y, int w, int h, bool linear, float mix = -1.0f);
    // Renders the frame at w x h offscreen and returns RGBA8 rows top-down.
    bool capture(int w, int h, bool linear, std::vector<uint8_t>& rgba);

private:
    void uploadSoftware();
    void deleteHwTarget();
    void drawFrame(GLuint fbo, int x, int y, int w, int h, bool linear);
    bool ensureHistory(int w, int h);
    void deleteHistory();
    bool ensureRepeatTargets(unsigned w, unsigned h);
    void deleteRepeatTargets();

    GlFunctions* gl_ = nullptr;
    GLuint program_ = 0, vao_ = 0;
    GLint uTexRect_ = -1, uClamp_ = -1, uSwapRB_ = -1;
    GLint maxTexture_ = 4096, maxRenderbuffer_ = 4096;

    GLuint hwFbo_ = 0, hwTex_ = 0, hwDepth_ = 0;
    unsigned hwW_ = 0, hwH_ = 0;
    bool hwDepthStencil_ = false;

    GLuint swTex_ = 0;
    unsigned swW_ = 0, swH_ = 0;
    bool sw32_ = false;            // texture holds XRGB8888 bytes (B,G,R,X) -> swizzled in the shader
    std::vector<uint8_t> staging_;
    unsigned stagingW_ = 0, stagingH_ = 0;
    bool staging32_ = false, stagingDirty_ = false;

    bool frameHw_ = false, frameBottomUp_ = false;
    unsigned frameW_ = 0, frameH_ = 0;
    uint64_t frameSerial_ = 0;     // bumped by every submitted (non-dupe) frame
    bool held_ = false;            // see holdFrame()
    bool unpushed_ = false;        // a new image was replaced by a held repeat before it reached the history

    // Smoothing: the last two core frames, already scaled to the viewport.
    int smoothing_ = 0;
    Interpolator interp_;
    bool interpReady_ = false;
    GLuint blendProgram_ = 0;
    GLint uBlendRect_ = -1, uMix_ = -1;
    GLuint histFbo_[2] = {0, 0}, histTex_[2] = {0, 0};
    bool histValid_[2] = {false, false};
    int histW_ = 0, histH_ = 0, histCur_ = 0;
    uint64_t histSerial_ = 0;
    // Motion field of the current history pair: estimated on its first in-between image.
    bool flowPending_ = false, flowValid_ = false;

    // Hardware repeat check: a copy of the last checked hardware image, and a target with one
    // pixel per 2x2 block that an occlusion query counts the differing blocks on.
    GLuint cmpProgram_ = 0, cmpQuery_ = 0;
    GLenum cmpQueryTarget_ = 0;
    GLint uCmpSize_ = -1, uCmpBlock_ = -1;
    GLuint lastFbo_ = 0, lastTex_ = 0, maskFbo_ = 0, maskTex_ = 0;
    unsigned lastW_ = 0, lastH_ = 0;
    bool lastBottomUp_ = false;
    uint64_t hwSerial_ = 0;        // hardware frames submitted
    uint64_t lastHwSerial_ = 0;    // hwSerial_ of the copied image (0 = none, or a software frame since)
    bool checkPending_ = false;
    // Occlusion queries (GL 1.5 / GLES 3.0) and glFlush are outside GlFunctions: loaded in init().
    void(RRM_GLAPIENTRY* genQueries_)(GLsizei n, GLuint* ids) = nullptr;
    void(RRM_GLAPIENTRY* deleteQueries_)(GLsizei n, const GLuint* ids) = nullptr;
    void(RRM_GLAPIENTRY* beginQuery_)(GLenum target, GLuint id) = nullptr;
    void(RRM_GLAPIENTRY* endQuery_)(GLenum target) = nullptr;
    void(RRM_GLAPIENTRY* getQueryObjectuiv_)(GLuint id, GLenum pname, GLuint* value) = nullptr;
    void(RRM_GLAPIENTRY* flush_)(void) = nullptr;
};

}  // namespace rrm::lr
