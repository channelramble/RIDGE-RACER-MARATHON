#include "libretro/Video.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace rrm::lr {

namespace {

// uClamp keeps linear filtering inside the used part of a larger texture;
// uSwapRB turns XRGB8888 bytes (B,G,R,X in memory) into RGB.
const char* kFragmentBody = R"(
uniform sampler2D uTex;
uniform vec4 uClamp;
uniform int uSwapRB;
in vec2 vUv;
void main() {
    vec4 c = texture(uTex, clamp(vUv, uClamp.xy, uClamp.zw));
    if (uSwapRB != 0) c = c.bgra;
    fragColor = vec4(c.rgb, 1.0);
}
)";

// Crossfade between the previous and the newest core frame (both already at viewport size).
const char* kBlendFragmentBody = R"(
uniform sampler2D uPrev;
uniform sampler2D uCur;
uniform float uMix;
in vec2 vUv;
void main() {
    fragColor = vec4(mix(texture(uPrev, vUv).rgb, texture(uCur, vUv).rgb, uMix), 1.0);
}
)";

// Hardware repeat check. uBlock 0: copies the core's image (uCur) texel for texel. uBlock N:
// one fragment per NxN block, discarded unless a texel differs from the copy of the previous
// image (uLast), so the occlusion query passes exactly when the images differ.
const char* kCompareFragmentBody = R"(
uniform sampler2D uCur;
uniform sampler2D uLast;
uniform vec2 uSize;
uniform int uBlock;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (uBlock == 0) {
        fragColor = texelFetch(uCur, p, 0);
        return;
    }
    ivec2 o = p * uBlock;
    ivec2 e = min(o + uBlock, ivec2(uSize));
    for (int y = o.y; y < e.y; ++y) {
        for (int x = o.x; x < e.x; ++x) {
            if (texelFetch(uCur, ivec2(x, y), 0).rgb != texelFetch(uLast, ivec2(x, y), 0).rgb) {
                fragColor = vec4(1.0);
                return;
            }
        }
    }
    discard;
}
)";

constexpr int kCompareBlock = 2;  // small blocks: more fragments in parallel, faster than 8x8
constexpr GLenum kGlSamplesPassed = 0x8914;     // GL_SAMPLES_PASSED (desktop GL)
constexpr GLenum kGlAnySamplesPassed = 0x8C2F;  // GL_ANY_SAMPLES_PASSED (GLES 3.0)
constexpr GLenum kGlQueryResult = 0x8866;       // GL_QUERY_RESULT
constexpr GLenum kGlQueryResultAvailable = 0x8867;

template <typename Fn>
void loadGl(Fn& fn, const char* name) {
    fn = reinterpret_cast<Fn>(SDL_GL_GetProcAddress(name));
}

}  // namespace

bool Video::init(GlFunctions* gl, bool gles, std::string& error) {
    gl_ = gl;
    auto& g = *gl_;
    program_ = buildQuadProgram(g, gles, kFragmentBody, error);
    if (!program_) return false;
    uTexRect_ = g.GetUniformLocation(program_, "uTexRect");
    uClamp_ = g.GetUniformLocation(program_, "uClamp");
    uSwapRB_ = g.GetUniformLocation(program_, "uSwapRB");
    g.UseProgram(program_);
    g.Uniform1i(g.GetUniformLocation(program_, "uTex"), 0);
    g.UseProgram(0);
    g.GenVertexArrays(1, &vao_);  // core profile cannot draw without a VAO

    // Optional: blending only switches itself off if this fails.
    std::string blendError;
    blendProgram_ = buildQuadProgram(g, gles, kBlendFragmentBody, blendError);
    if (blendProgram_) {
        uBlendRect_ = g.GetUniformLocation(blendProgram_, "uTexRect");
        uMix_ = g.GetUniformLocation(blendProgram_, "uMix");
        g.UseProgram(blendProgram_);
        g.Uniform1i(g.GetUniformLocation(blendProgram_, "uPrev"), 0);
        g.Uniform1i(g.GetUniformLocation(blendProgram_, "uCur"), 1);
        g.UseProgram(0);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[libretro] frame blending unavailable: %s", blendError.c_str());
    }
    std::string interpError;
    interpReady_ = blendProgram_ && interp_.init(gl_, gles, interpError);
    if (!interpReady_ && blendProgram_)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[libretro] frame interpolation unavailable: %s", interpError.c_str());
    // Optional too: without it, every hardware frame counts as a new image.
    loadGl(genQueries_, "glGenQueries");
    loadGl(deleteQueries_, "glDeleteQueries");
    loadGl(beginQuery_, "glBeginQuery");
    loadGl(endQuery_, "glEndQuery");
    loadGl(getQueryObjectuiv_, "glGetQueryObjectuiv");
    loadGl(flush_, "glFlush");
    std::string cmpError = "no occlusion queries";
    if (genQueries_ && deleteQueries_ && beginQuery_ && endQuery_ && getQueryObjectuiv_ && flush_)
        cmpProgram_ = buildQuadProgram(g, gles, kCompareFragmentBody, cmpError);
    if (cmpProgram_) {
        uCmpSize_ = g.GetUniformLocation(cmpProgram_, "uSize");
        uCmpBlock_ = g.GetUniformLocation(cmpProgram_, "uBlock");
        g.UseProgram(cmpProgram_);
        g.Uniform1i(g.GetUniformLocation(cmpProgram_, "uCur"), 0);
        g.Uniform1i(g.GetUniformLocation(cmpProgram_, "uLast"), 1);
        g.UseProgram(0);
        genQueries_(1, &cmpQuery_);
        cmpQueryTarget_ = gles ? kGlAnySamplesPassed : kGlSamplesPassed;
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[libretro] repeated hardware frames can't be detected: %s",
                    cmpError.c_str());
    }

    g.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture_);
    g.GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbuffer_);
    if (maxTexture_ <= 0) maxTexture_ = 4096;
    if (maxRenderbuffer_ <= 0) maxRenderbuffer_ = maxTexture_;
    return true;
}

void Video::deleteHwTarget() {
    if (!gl_) return;
    auto& g = *gl_;
    if (hwFbo_) g.DeleteFramebuffers(1, &hwFbo_);
    if (hwTex_) g.DeleteTextures(1, &hwTex_);
    if (hwDepth_) g.DeleteRenderbuffers(1, &hwDepth_);
    hwFbo_ = hwTex_ = hwDepth_ = 0;
    hwW_ = hwH_ = 0;
}

void Video::deleteHistory() {
    if (gl_) {
        auto& g = *gl_;
        for (int i = 0; i < 2; ++i) {
            if (histFbo_[i]) g.DeleteFramebuffers(1, &histFbo_[i]);
            if (histTex_[i]) g.DeleteTextures(1, &histTex_[i]);
        }
    }
    histFbo_[0] = histFbo_[1] = histTex_[0] = histTex_[1] = 0;
    histValid_[0] = histValid_[1] = false;
    histW_ = histH_ = 0;
    flowPending_ = flowValid_ = false;
}

void Video::deleteRepeatTargets() {
    if (gl_) {
        auto& g = *gl_;
        if (lastFbo_) g.DeleteFramebuffers(1, &lastFbo_);
        if (lastTex_) g.DeleteTextures(1, &lastTex_);
        if (maskFbo_) g.DeleteFramebuffers(1, &maskFbo_);
        if (maskTex_) g.DeleteTextures(1, &maskTex_);
    }
    lastFbo_ = lastTex_ = maskFbo_ = maskTex_ = 0;
    lastW_ = lastH_ = 0;
    lastHwSerial_ = 0;
    checkPending_ = false;
}

void Video::shutdown(bool deleteObjects) {
    if (gl_ && deleteObjects) {
        auto& g = *gl_;
        deleteHwTarget();
        deleteHistory();
        deleteRepeatTargets();
        interp_.shutdown();
        if (swTex_) g.DeleteTextures(1, &swTex_);
        if (vao_) g.DeleteVertexArrays(1, &vao_);
        if (program_) g.DeleteProgram(program_);
        if (blendProgram_) g.DeleteProgram(blendProgram_);
        if (cmpProgram_) g.DeleteProgram(cmpProgram_);
        if (cmpQuery_) deleteQueries_(1, &cmpQuery_);
    }
    histFbo_[0] = histFbo_[1] = histTex_[0] = histTex_[1] = 0;
    histValid_[0] = histValid_[1] = false;
    histW_ = histH_ = 0;
    flowPending_ = flowValid_ = false;
    blendProgram_ = 0;
    lastFbo_ = lastTex_ = maskFbo_ = maskTex_ = 0;
    lastW_ = lastH_ = 0;
    lastHwSerial_ = hwSerial_ = 0;
    checkPending_ = held_ = false;
    cmpProgram_ = cmpQuery_ = 0;
    if (!deleteObjects) interp_.shutdown(false);
    interpReady_ = false;
    hwFbo_ = hwTex_ = hwDepth_ = 0;
    hwW_ = hwH_ = 0;
    swTex_ = vao_ = program_ = 0;
    swW_ = swH_ = 0;
    staging_.clear();
    staging_.shrink_to_fit();
    stagingDirty_ = false;
    frameW_ = frameH_ = 0;
    frameHw_ = false;
    gl_ = nullptr;
}

bool Video::createHwTarget(unsigned w, unsigned h, bool depth, bool stencil, std::string& error) {
    auto& g = *gl_;
    deleteHwTarget();
    const unsigned limit = static_cast<unsigned>(std::min(maxTexture_, maxRenderbuffer_));
    hwW_ = std::clamp(w, 1u, limit);
    hwH_ = std::clamp(h, 1u, limit);
    hwDepthStencil_ = stencil;  // stencil without depth is invalid per libretro and ignored

    g.GenTextures(1, &hwTex_);
    g.BindTexture(GL_TEXTURE_2D, hwTex_);
    g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(hwW_), static_cast<GLsizei>(hwH_), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g.BindTexture(GL_TEXTURE_2D, 0);

    g.GenFramebuffers(1, &hwFbo_);
    g.BindFramebuffer(GL_FRAMEBUFFER, hwFbo_);
    g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hwTex_, 0);
    if (depth) {
        g.GenRenderbuffers(1, &hwDepth_);
        g.BindRenderbuffer(GL_RENDERBUFFER, hwDepth_);
        g.RenderbufferStorage(GL_RENDERBUFFER, stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                              static_cast<GLsizei>(hwW_), static_cast<GLsizei>(hwH_));
        g.FramebufferRenderbuffer(GL_FRAMEBUFFER, stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, hwDepth_);
        g.BindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    GLenum status = g.CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        char buf[96];
        SDL_snprintf(buf, sizeof buf, "core framebuffer %ux%u incomplete (status 0x%04x)", hwW_, hwH_, status);
        error = buf;
        g.BindFramebuffer(GL_FRAMEBUFFER, 0);
        deleteHwTarget();
        return false;
    }
    g.ClearColor(0, 0, 0, 1);
    g.Clear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : 0u));
    return true;
}

void Video::growHwTarget(unsigned w, unsigned h) {
    if (!hwFbo_) return;
    auto& g = *gl_;
    const unsigned limit = static_cast<unsigned>(std::min(maxTexture_, maxRenderbuffer_));
    w = std::clamp(std::max(w, hwW_), 1u, limit);
    h = std::clamp(std::max(h, hwH_), 1u, limit);
    if (w == hwW_ && h == hwH_) return;

    // Called from inside retro_run: preserve the bindings we touch.
    GLint prevTex = 0, prevRb = 0, prevUnpack = 0;
    g.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    g.GetIntegerv(GL_RENDERBUFFER_BINDING, &prevRb);
    g.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevUnpack);
    if (prevUnpack) g.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    g.BindTexture(GL_TEXTURE_2D, hwTex_);
    g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    g.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    if (hwDepth_) {
        g.BindRenderbuffer(GL_RENDERBUFFER, hwDepth_);
        g.RenderbufferStorage(GL_RENDERBUFFER, hwDepthStencil_ ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                              static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        g.BindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(prevRb));
    }
    if (prevUnpack) g.BindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(prevUnpack));
    SDL_LogInfo(SDL_LOG_CATEGORY_RENDER, "[libretro] core framebuffer resized %ux%u -> %ux%u", hwW_, hwH_, w, h);
    hwW_ = w;
    hwH_ = h;
}

bool Video::submitSoftware(const void* data, unsigned w, unsigned h, size_t pitch, retro_pixel_format fmt) {
    if (!data || !w || !h) return false;
    const bool is32 = fmt == RETRO_PIXEL_FORMAT_XRGB8888;
    const size_t bpp = is32 ? 4 : 2;
    const size_t row = static_cast<size_t>(w) * bpp;
    if (pitch < row) return false;  // malformed frame
    // Games drawing at 30 fps often get the same image sent twice instead of a dupe: compared
    // while copying, so frame smoothing can pace in-between images by the real image rate.
    bool same = hasFrame() && !frameHw_ && stagingW_ == w && stagingH_ == h && staging32_ == is32 &&
                staging_.size() == row * h;
    staging_.resize(row * h);
    const auto* src = static_cast<const uint8_t*>(data);
    uint8_t* dst = staging_.data();
    if (fmt == RETRO_PIXEL_FORMAT_0RGB1555) {
        // No 1555 upload path exists in GLES 3.0; expand to RGB565 instead.
        uint16_t diff = 0;
        for (unsigned y = 0; y < h; ++y) {
            const auto* s = reinterpret_cast<const uint16_t*>(src + y * pitch);
            auto* d = reinterpret_cast<uint16_t*>(dst + y * row);
            for (unsigned x = 0; x < w; ++x) {
                uint16_t p = s[x];
                uint16_t r = (p >> 10) & 0x1F, gr = (p >> 5) & 0x1F, b = p & 0x1F;
                const auto v = static_cast<uint16_t>((r << 11) | (gr << 6) | ((gr >> 4) << 5) | b);
                diff |= static_cast<uint16_t>(d[x] ^ v);
                d[x] = v;
            }
        }
        same = same && diff == 0;
    } else {
        for (unsigned y = 0; y < h; ++y) {
            uint8_t* d = dst + y * row;
            const uint8_t* s = src + y * pitch;
            if (same && std::memcmp(d, s, row) == 0) continue;  // identical so far: nothing to copy
            same = false;
            std::memcpy(d, s, row);
        }
    }
    if (same) return false;
    stagingW_ = w;
    stagingH_ = h;
    staging32_ = is32;
    stagingDirty_ = true;
    frameHw_ = false;
    frameBottomUp_ = false;
    frameW_ = w;
    frameH_ = h;
    if (frameSerial_ != histSerial_ && !held_) unpushed_ = true;  // (see draw)
    ++frameSerial_;
    held_ = false;
    lastHwSerial_ = 0;  // the repeat check's copy is no longer the previous frame
    return true;
}

// Called inside retro_run, so no GL work here: whether the image repeats the previous one is
// checked afterwards, on request (beginRepeatCheck).
bool Video::submitHardware(unsigned w, unsigned h, bool bottomLeftOrigin) {
    if (!hwFbo_ || !w || !h) return false;
    frameHw_ = true;
    frameBottomUp_ = bottomLeftOrigin;
    frameW_ = std::min(w, hwW_);
    frameH_ = std::min(h, hwH_);
    if (frameSerial_ != histSerial_ && !held_) unpushed_ = true;  // (see draw)
    ++frameSerial_;
    ++hwSerial_;
    held_ = false;
    return true;
}

bool Video::ensureRepeatTargets(unsigned w, unsigned h) {
    if (lastTex_ && lastW_ == w && lastH_ == h) return true;
    deleteRepeatTargets();
    auto& g = *gl_;
    const unsigned bw = (w + kCompareBlock - 1) / kCompareBlock, bh = (h + kCompareBlock - 1) / kCompareBlock;
    bool ok = true;
    auto create = [&](GLuint& tex, GLuint& fbo, unsigned tw, unsigned th, GLint internal, GLenum format) {
        g.GenTextures(1, &tex);
        g.BindTexture(GL_TEXTURE_2D, tex);
        g.TexImage2D(GL_TEXTURE_2D, 0, internal, static_cast<GLsizei>(tw), static_cast<GLsizei>(th), 0, format,
                     GL_UNSIGNED_BYTE, nullptr);
        g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g.GenFramebuffers(1, &fbo);
        g.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        ok = ok && g.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    };
    create(lastTex_, lastFbo_, w, h, GL_RGBA8, GL_RGBA);
    create(maskTex_, maskFbo_, bw, bh, GL_R8, GL_RED);  // contents unused
    g.BindTexture(GL_TEXTURE_2D, 0);
    g.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        deleteRepeatTargets();
        return false;
    }
    lastW_ = w;
    lastH_ = h;
    return true;
}

// Games drawing at 30 fps make hardware-rendered cores present each image twice. The check
// runs on the GPU at native size: the frame against a copy of the previous hardware frame (an
// exact compare, so only a truly identical image counts), then the frame becomes the copy.
bool Video::beginRepeatCheck() {
    if (checkPending_ || !frameHw_ || !hwTex_ || !cmpProgram_ || !hasFrame()) return false;
    auto& g = *gl_;
    // The copy only helps if it holds the frame right before this one, at the same size.
    const bool comparable = lastHwSerial_ && lastHwSerial_ + 1 == hwSerial_ && lastW_ == frameW_ &&
                            lastH_ == frameH_ && lastBottomUp_ == frameBottomUp_;
    if (!ensureRepeatTargets(frameW_, frameH_)) return false;
    g.UseProgram(cmpProgram_);
    g.BindVertexArray(vao_);
    g.ActiveTexture(GL_TEXTURE0);
    g.BindTexture(GL_TEXTURE_2D, hwTex_);
    if (g.BindSampler) g.BindSampler(0, 0);  // texelFetch still needs a complete texture
    g.ActiveTexture(GL_TEXTURE1);
    g.BindTexture(GL_TEXTURE_2D, comparable ? lastTex_ : hwTex_);  // never the copy while it is the target
    if (g.BindSampler) g.BindSampler(1, 0);
    g.Uniform2f(uCmpSize_, static_cast<float>(frameW_), static_cast<float>(frameH_));
    if (comparable) {
        g.BindFramebuffer(GL_FRAMEBUFFER, maskFbo_);
        g.Viewport(0, 0, static_cast<GLsizei>((frameW_ + kCompareBlock - 1) / kCompareBlock),
                   static_cast<GLsizei>((frameH_ + kCompareBlock - 1) / kCompareBlock));
        g.Uniform1i(uCmpBlock_, kCompareBlock);
        beginQuery_(cmpQueryTarget_, cmpQuery_);
        g.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        endQuery_(cmpQueryTarget_);
        g.BindTexture(GL_TEXTURE_2D, hwTex_);
    }
    // The answer isn't known yet, so the frame always becomes the copy (a repeat copies the same).
    g.BindFramebuffer(GL_FRAMEBUFFER, lastFbo_);
    g.Viewport(0, 0, static_cast<GLsizei>(frameW_), static_cast<GLsizei>(frameH_));
    g.Uniform1i(uCmpBlock_, 0);
    g.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    lastHwSerial_ = hwSerial_;
    lastBottomUp_ = frameBottomUp_;
    g.BindTexture(GL_TEXTURE_2D, 0);
    g.ActiveTexture(GL_TEXTURE0);
    g.BindTexture(GL_TEXTURE_2D, 0);
    g.BindVertexArray(0);
    g.UseProgram(0);
    if (comparable) flush_();  // the GPU starts now; polling without a flush may never see the answer
    checkPending_ = comparable;
    return comparable;
}

int Video::repeatCheckResult(bool wait) {
    if (!checkPending_) return -1;
    GLuint ready = 1;
    if (!wait) getQueryObjectuiv_(cmpQuery_, kGlQueryResultAvailable, &ready);
    if (!ready) return -1;
    GLuint differs = 1;
    getQueryObjectuiv_(cmpQuery_, kGlQueryResult, &differs);
    checkPending_ = false;
    return differs ? 0 : 1;
}

void Video::uploadSoftware() {
    if (!stagingDirty_) return;
    stagingDirty_ = false;
    auto& g = *gl_;
    if (!swTex_) g.GenTextures(1, &swTex_);
    g.ActiveTexture(GL_TEXTURE0);
    g.BindTexture(GL_TEXTURE_2D, swTex_);
    g.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    g.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    const GLenum format = staging32_ ? GL_RGBA : GL_RGB;
    const GLenum type = staging32_ ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT_5_6_5;
    if (stagingW_ != swW_ || stagingH_ != swH_ || staging32_ != sw32_) {
        g.TexImage2D(GL_TEXTURE_2D, 0, staging32_ ? GL_RGBA8 : GL_RGB, static_cast<GLsizei>(stagingW_),
                     static_cast<GLsizei>(stagingH_), 0, format, type, staging_.data());
        g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        swW_ = stagingW_;
        swH_ = stagingH_;
        sw32_ = staging32_;
    } else {
        g.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(swW_), static_cast<GLsizei>(swH_), format, type,
                        staging_.data());
    }
    g.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g.BindTexture(GL_TEXTURE_2D, 0);
}

bool Video::ensureHistory(int w, int h) {
    if (histTex_[0] && histW_ == w && histH_ == h) return true;
    deleteHistory();
    if (w > maxTexture_ || h > maxTexture_) return false;
    auto& g = *gl_;
    g.GenTextures(2, histTex_);
    g.GenFramebuffers(2, histFbo_);
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        g.BindTexture(GL_TEXTURE_2D, histTex_[i]);
        g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);  // sampled 1:1
        g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g.BindFramebuffer(GL_FRAMEBUFFER, histFbo_[i]);
        g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, histTex_[i], 0);
        ok = ok && g.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    g.BindTexture(GL_TEXTURE_2D, 0);
    g.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        deleteHistory();
        return false;
    }
    histW_ = w;
    histH_ = h;
    return true;
}

void Video::draw(GLuint fbo, int x, int y, int w, int h, bool linear, float mix) {
    if (!hasFrame() || !program_ || w <= 0 || h <= 0) return;
    if (mix < 0.0f || !smoothingMode() || !ensureHistory(w, h)) {
        // Not smoothing: no history work. It restarts from fresh frames when smoothing resumes.
        histValid_[0] = histValid_[1] = false;
        flowPending_ = flowValid_ = false;
        unpushed_ = false;
        drawFrame(fbo, x, y, w, h, linear);
        return;
    }
    // A new core frame goes into the other history slot; the previous one stays for the fade. A
    // held repeat that followed a new image not drawn yet still brings that image (it is the same).
    if ((histSerial_ != frameSerial_ && (!held_ || unpushed_)) || !histValid_[histCur_]) {
        histCur_ ^= 1;
        drawFrame(histFbo_[histCur_], 0, 0, w, h, linear);
        histValid_[histCur_] = true;
        histSerial_ = frameSerial_;
        unpushed_ = false;
        flowPending_ = smoothingMode() == 2 && histValid_[histCur_ ^ 1];
        flowValid_ = false;
    }
    const int prev = histCur_ ^ 1;
    if (!histValid_[prev]) mix = 1.0f;
    if (mix < 1.0f && flowPending_) {
        flowPending_ = false;
        flowValid_ = interp_.estimate(histTex_[prev], histTex_[histCur_], w, h);
    }
    // Without a motion field for this pair (viewport too small, no targets) it crossfades.
    if (mix < 1.0f && flowValid_ && interp_.draw(fbo, x, y, w, h, histTex_[prev], histTex_[histCur_], mix)) return;
    auto& g = *gl_;
    g.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    g.Viewport(x, y, w, h);
    g.UseProgram(blendProgram_);
    g.BindVertexArray(vao_);
    g.ActiveTexture(GL_TEXTURE0);
    g.BindTexture(GL_TEXTURE_2D, histTex_[prev]);
    if (g.BindSampler) g.BindSampler(0, 0);
    g.ActiveTexture(GL_TEXTURE1);
    g.BindTexture(GL_TEXTURE_2D, histTex_[histCur_]);
    if (g.BindSampler) g.BindSampler(1, 0);
    g.Uniform4f(uBlendRect_, 0.0f, 0.0f, 1.0f, 1.0f);  // history images are stored bottom-up already
    g.Uniform1f(uMix_, std::clamp(mix, 0.0f, 1.0f));
    g.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g.BindTexture(GL_TEXTURE_2D, 0);
    g.ActiveTexture(GL_TEXTURE0);
    g.BindTexture(GL_TEXTURE_2D, 0);
    g.BindVertexArray(0);
    g.UseProgram(0);
}

void Video::drawFrame(GLuint fbo, int x, int y, int w, int h, bool linear) {
    auto& g = *gl_;
    if (!frameHw_) uploadSoftware();
    const GLuint tex = frameHw_ ? hwTex_ : swTex_;
    const float tw = static_cast<float>(frameHw_ ? hwW_ : swW_);
    const float th = static_cast<float>(frameHw_ ? hwH_ : swH_);
    if (!tex || tw <= 0 || th <= 0) return;
    const float u1 = frameW_ / tw, v1 = frameH_ / th;

    g.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    g.Viewport(x, y, w, h);
    g.UseProgram(program_);
    g.BindVertexArray(vao_);
    g.ActiveTexture(GL_TEXTURE0);
    g.BindTexture(GL_TEXTURE_2D, tex);
    if (g.BindSampler) g.BindSampler(0, 0);
    const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    if (frameBottomUp_) g.Uniform4f(uTexRect_, 0.0f, 0.0f, u1, v1);
    else g.Uniform4f(uTexRect_, 0.0f, v1, u1, 0.0f);  // first row is the top of the image
    g.Uniform4f(uClamp_, 0.5f / tw, 0.5f / th, (frameW_ - 0.5f) / tw, (frameH_ - 0.5f) / th);
    g.Uniform1i(uSwapRB_, (!frameHw_ && sw32_) ? 1 : 0);
    g.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g.BindTexture(GL_TEXTURE_2D, 0);
    g.BindVertexArray(0);
    g.UseProgram(0);
}

bool Video::capture(int w, int h, bool linear, std::vector<uint8_t>& rgba) {
    if (!hasFrame() || w <= 0 || h <= 0 || w > maxTexture_ || h > maxTexture_) return false;
    auto& g = *gl_;
    GLuint tex = 0, fbo = 0;
    g.GenTextures(1, &tex);
    g.BindTexture(GL_TEXTURE_2D, tex);
    g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    g.BindTexture(GL_TEXTURE_2D, 0);
    g.GenFramebuffers(1, &fbo);
    g.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    bool ok = g.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        drawFrame(fbo, 0, 0, w, h, linear);
        g.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        g.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        g.PixelStorei(GL_PACK_ALIGNMENT, 4);
        g.PixelStorei(GL_PACK_ROW_LENGTH, 0);
        std::vector<uint8_t> raw(static_cast<size_t>(w) * h * 4);
        g.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        rgba.resize(raw.size());
        const size_t row = static_cast<size_t>(w) * 4;
        for (int y = 0; y < h; ++y)  // GL rows are bottom-up
            std::memcpy(rgba.data() + static_cast<size_t>(y) * row, raw.data() + static_cast<size_t>(h - 1 - y) * row, row);
    }
    g.BindFramebuffer(GL_FRAMEBUFFER, 0);
    g.DeleteFramebuffers(1, &fbo);
    g.DeleteTextures(1, &tex);
    return ok;
}

}  // namespace rrm::lr
