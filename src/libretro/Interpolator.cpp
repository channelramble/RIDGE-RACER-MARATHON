#include "libretro/Interpolator.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <utility>

namespace rrm::lr {

namespace {

// Luma downsample. From colour (the viewport-size history image): four bilinear taps one
// texel off-centre average a 4x4 block. From luma: one bilinear tap at the 2x2 corner.
const char* kDown = R"(
uniform sampler2D uSrc;
uniform vec2 uTexel;
uniform int uFromColor;
in vec2 vUv;
float luma(vec4 c) { return uFromColor != 0 ? dot(c.rgb, vec3(0.299, 0.587, 0.114)) : c.r; }
void main() {
    vec2 o = uFromColor != 0 ? uTexel : vec2(0.0);
    float l = luma(texture(uSrc, vUv + vec2(-o.x, -o.y))) + luma(texture(uSrc, vUv + vec2(o.x, -o.y))) +
              luma(texture(uSrc, vUv + vec2(-o.x, o.y))) + luma(texture(uSrc, vUv + vec2(o.x, o.y)));
    fragColor = vec4(0.25 * l, 0.0, 0.0, 1.0);
}
)";

// Symmetric block matching around a predicted vector (all vectors in UV units, so they carry
// across pyramid levels unchanged). Candidates: zero motion (slightly favoured, so a static HUD or
// sky never drifts), the prediction and its four neighbours' predictions, and a square search of
// `uRadius` steps of `uStep` texels around the prediction. Cost: 5x5 sum of absolute luma
// differences plus a small penalty for leaving the prediction.
const char* kSearch = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform sampler2D uPrevFlow;
uniform int uHasPrev;
uniform vec2 uTexel;
uniform int uRadius;
uniform float uStep;
in vec2 vUv;
float cost(vec2 v) {
    float s = 0.0;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i) {
            vec2 q = vUv + vec2(float(i), float(j)) * uTexel;
            s += abs(texture(uA, q - 0.5 * v).r - texture(uB, q + 0.5 * v).r);
        }
    return s;
}
void main() {
    vec2 base = vec2(0.0);
    if (uHasPrev != 0) base = texture(uPrevFlow, vUv).xy;
    vec2 best = vec2(0.0);
    float bestCost = cost(best) * 0.97 - 0.01;
    if (uHasPrev != 0) {
        vec2 n[4];
        n[0] = texture(uPrevFlow, vUv + vec2(4.0 * uTexel.x, 0.0)).xy;
        n[1] = texture(uPrevFlow, vUv - vec2(4.0 * uTexel.x, 0.0)).xy;
        n[2] = texture(uPrevFlow, vUv + vec2(0.0, 4.0 * uTexel.y)).xy;
        n[3] = texture(uPrevFlow, vUv - vec2(0.0, 4.0 * uTexel.y)).xy;
        for (int k = 0; k < 4; ++k) {
            float c = cost(n[k]) + 0.02;
            if (c < bestCost) { bestCost = c; best = n[k]; }
        }
        float c0 = cost(base);
        if (c0 < bestCost) { bestCost = c0; best = base; }
    }
    vec2 center = best;
    for (int y = -uRadius; y <= uRadius; ++y)
        for (int x = -uRadius; x <= uRadius; ++x) {
            if (x == 0 && y == 0) continue;
            vec2 d = vec2(float(x), float(y)) * uStep;
            vec2 v = center + d * uTexel;
            float c = cost(v) + 0.01 * length(d);
            if (c < bestCost) { bestCost = c; best = v; }
        }
    fragColor = vec4(best, 0.0, 1.0);
}
)";

// 3x3 vector median: the vector with the smallest summed distance to its neighbours.
const char* kMedian = R"(
uniform sampler2D uFlow;
uniform vec2 uTexel;
in vec2 vUv;
void main() {
    vec2 v[9];
    int k = 0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) v[k++] = texture(uFlow, vUv + vec2(float(i), float(j)) * uTexel).xy;
    vec2 best = v[4];
    float bestSum = 1e9;
    for (int a = 0; a < 9; ++a) {
        float s = 0.0;
        for (int b = 0; b < 9; ++b) s += length(v[a] - v[b]);
        if (s < bestSum) { bestSum = s; best = v[a]; }
    }
    fragColor = vec4(best, 0.0, 1.0);
}
)";

// The in-between image. Motion-compensated samples are used where they agree; where standing
// still explains the change at least as well (HUD, sky) the frames are blended in place; where
// neither matches (occlusion, new content) the moved sample of the nearer frame is shown.
const char* kInterp = R"(
uniform sampler2D uPrev;
uniform sampler2D uCur;
uniform sampler2D uFlow;
uniform float uT;
in vec2 vUv;
void main() {
    vec2 v = texture(uFlow, vUv).xy;
    vec3 a = texture(uPrev, vUv - uT * v).rgb;
    vec3 b = texture(uCur, vUv + (1.0 - uT) * v).rgb;
    vec3 a0 = texture(uPrev, vUv).rgb;
    vec3 b0 = texture(uCur, vUv).rgb;
    float em = dot(abs(a - b), vec3(1.0 / 3.0));
    float e0 = dot(abs(a0 - b0), vec3(1.0 / 3.0));
    vec3 moved = mix(a, b, uT);
    vec3 still = mix(a0, b0, uT);
    vec3 nearest = uT < 0.5 ? a : b;
    float useStill = smoothstep(-0.01, 0.01, em - e0);       // 1 where standing still matches better
    float trust = 1.0 - smoothstep(0.08, 0.2, min(em, e0));  // 0 where nothing matches
    vec3 c = mix(moved, still, useStill);
    fragColor = vec4(mix(nearest, c, trust), 1.0);
}
)";

constexpr int kMinLevelWidth = 48;

}  // namespace

bool Interpolator::init(GlFunctions* gl, bool gles, std::string& error) {
    gl_ = gl;
    gles_ = gles;
    auto& g = *gl_;
    downProg_ = buildQuadProgram(g, gles, kDown, error);
    searchProg_ = downProg_ ? buildQuadProgram(g, gles, kSearch, error) : 0;
    medianProg_ = searchProg_ ? buildQuadProgram(g, gles, kMedian, error) : 0;
    interpProg_ = medianProg_ ? buildQuadProgram(g, gles, kInterp, error) : 0;
    if (!interpProg_) {
        shutdown();
        return false;
    }
    down_ = {g.GetUniformLocation(downProg_, "uSrc"), g.GetUniformLocation(downProg_, "uTexel"),
             g.GetUniformLocation(downProg_, "uFromColor"), g.GetUniformLocation(downProg_, "uTexRect")};
    search_ = {g.GetUniformLocation(searchProg_, "uA"),       g.GetUniformLocation(searchProg_, "uB"),
               g.GetUniformLocation(searchProg_, "uPrevFlow"), g.GetUniformLocation(searchProg_, "uHasPrev"),
               g.GetUniformLocation(searchProg_, "uTexel"),    g.GetUniformLocation(searchProg_, "uRadius"),
               g.GetUniformLocation(searchProg_, "uStep"),     g.GetUniformLocation(searchProg_, "uTexRect")};
    median_ = {g.GetUniformLocation(medianProg_, "uFlow"), g.GetUniformLocation(medianProg_, "uTexel"),
               g.GetUniformLocation(medianProg_, "uTexRect")};
    interp_ = {g.GetUniformLocation(interpProg_, "uPrev"), g.GetUniformLocation(interpProg_, "uCur"),
               g.GetUniformLocation(interpProg_, "uFlow"), g.GetUniformLocation(interpProg_, "uT"),
               g.GetUniformLocation(interpProg_, "uTexRect")};
    g.GenVertexArrays(1, &vao_);
    // Flow vectors need a renderable half-float target (always on desktop GL; an extension on GLES).
    Target probe;
    ready_ = makeTarget(probe, 8, 8, true);
    freeTarget(probe);
    if (!ready_) {
        error = "half-float render targets are not supported";
        shutdown();
        return false;
    }
    return true;
}

void Interpolator::shutdown(bool deleteObjects) {
    if (gl_ && deleteObjects) {
        auto& g = *gl_;
        freeLevels();
        for (GLuint p : {downProg_, searchProg_, medianProg_, interpProg_})
            if (p) g.DeleteProgram(p);
        if (vao_) g.DeleteVertexArrays(1, &vao_);
    }
    lumaPrev_.clear();
    lumaCur_.clear();
    flow_.clear();
    flowTmp_ = {};
    downProg_ = searchProg_ = medianProg_ = interpProg_ = vao_ = 0;
    srcW_ = srcH_ = 0;
    ready_ = false;
}

bool Interpolator::makeTarget(Target& t, int w, int h, bool flow) {
    auto& g = *gl_;
    g.GenTextures(1, &t.tex);
    g.BindTexture(GL_TEXTURE_2D, t.tex);
    if (flow) g.TexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, w, h, 0, GL_RG, GL_HALF_FLOAT, nullptr);
    else g.TexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g.GenFramebuffers(1, &t.fbo);
    g.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    const bool ok = g.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    g.BindFramebuffer(GL_FRAMEBUFFER, 0);
    g.BindTexture(GL_TEXTURE_2D, 0);
    t.w = w;
    t.h = h;
    return ok;
}

void Interpolator::freeTarget(Target& t) {
    if (!gl_) return;
    if (t.fbo) gl_->DeleteFramebuffers(1, &t.fbo);
    if (t.tex) gl_->DeleteTextures(1, &t.tex);
    t = {};
}

void Interpolator::freeLevels() {
    for (auto* v : {&lumaPrev_, &lumaCur_, &flow_})
        for (auto& t : *v) freeTarget(t);
    lumaPrev_.clear();
    lumaCur_.clear();
    flow_.clear();
    freeTarget(flowTmp_);
    srcW_ = srcH_ = 0;
}

// Level 0 is a quarter of the viewport; each further level halves it down to ~48 pixels wide.
bool Interpolator::ensureLevels(int w, int h) {
    if (w == srcW_ && h == srcH_ && !flow_.empty()) return true;
    freeLevels();
    int lw = std::max(1, w / 4), lh = std::max(1, h / 4);
    bool ok = true;
    while (true) {
        Target a, b, f;
        ok = ok && makeTarget(a, lw, lh, false) && makeTarget(b, lw, lh, false) && makeTarget(f, lw, lh, true);
        lumaPrev_.push_back(a);
        lumaCur_.push_back(b);
        flow_.push_back(f);
        if (!ok || lw / 2 < kMinLevelWidth || lh / 2 < 8) break;
        lw /= 2;
        lh /= 2;
    }
    ok = ok && makeTarget(flowTmp_, flow_[0].w, flow_[0].h, true);
    if (!ok) {
        freeLevels();  // never keep incomplete targets: the next call starts over
        return false;
    }
    srcW_ = w;
    srcH_ = h;
    return true;
}

void Interpolator::bindTex(int unit, GLuint tex, bool linear) {
    auto& g = *gl_;
    g.ActiveTexture(GL_TEXTURE0 + unit);
    g.BindTexture(GL_TEXTURE_2D, tex);
    if (g.BindSampler) g.BindSampler(unit, 0);
    const GLint f = linear ? GL_LINEAR : GL_NEAREST;
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}

void Interpolator::pass(const Target& dst) {
    auto& g = *gl_;
    g.BindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    g.Viewport(0, 0, dst.w, dst.h);
    g.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

bool Interpolator::estimate(GLuint prevTex, GLuint curTex, int w, int h) {
    if (!ready_ || w < 64 || h < 64 || !ensureLevels(w, h)) return false;
    auto& g = *gl_;
    g.BindVertexArray(vao_);

    // 1. luma pyramids
    g.UseProgram(downProg_);
    g.Uniform4f(down_.rect, 0.0f, 0.0f, 1.0f, 1.0f);
    g.Uniform1i(down_.src, 0);
    for (int which = 0; which < 2; ++which) {
        auto& levels = which == 0 ? lumaPrev_ : lumaCur_;
        for (size_t l = 0; l < levels.size(); ++l) {
            const bool fromColor = l == 0;
            const GLuint src = fromColor ? (which == 0 ? prevTex : curTex) : levels[l - 1].tex;
            const int sw = fromColor ? w : levels[l - 1].w, sh = fromColor ? h : levels[l - 1].h;
            bindTex(0, src, true);
            g.Uniform2f(down_.texel, 1.0f / sw, 1.0f / sh);
            g.Uniform1i(down_.fromColor, fromColor ? 1 : 0);
            pass(levels[l]);
        }
    }

    // 2. coarse-to-fine search; 3. sub-pixel refinement at level 0 (ping-pong with flowTmp_)
    g.UseProgram(searchProg_);
    g.Uniform4f(search_.rect, 0.0f, 0.0f, 1.0f, 1.0f);
    g.Uniform1i(search_.a, 0);
    g.Uniform1i(search_.b, 1);
    g.Uniform1i(search_.prevFlow, 2);
    auto search = [&](size_t l, const Target* prev, const Target& dst, int radius, float step) {
        bindTex(0, lumaPrev_[l].tex, true);
        bindTex(1, lumaCur_[l].tex, true);
        bindTex(2, prev ? prev->tex : 0, true);
        g.Uniform1i(search_.hasPrev, prev ? 1 : 0);
        g.Uniform2f(search_.texel, 1.0f / lumaPrev_[l].w, 1.0f / lumaPrev_[l].h);
        g.Uniform1i(search_.radius, radius);
        g.Uniform1f(search_.step, step);
        pass(dst);
    };
    const size_t coarsest = flow_.size() - 1;
    search(coarsest, nullptr, flow_[coarsest], 4, 1.0f);
    for (size_t l = coarsest; l-- > 0;) search(l, &flow_[l + 1], flow_[l], 1, 1.0f);
    search(0, &flow_[0], flowTmp_, 1, 0.5f);
    search(0, &flowTmp_, flow_[0], 1, 0.25f);

    // 4. outlier removal: flow_[0] -> flowTmp_ (the field draw() uses)
    g.UseProgram(medianProg_);
    g.Uniform4f(median_.rect, 0.0f, 0.0f, 1.0f, 1.0f);
    g.Uniform1i(median_.flow, 0);
    g.Uniform2f(median_.texel, 1.0f / flow_[0].w, 1.0f / flow_[0].h);
    bindTex(0, flow_[0].tex, false);
    pass(flowTmp_);

    for (int u = 2; u >= 0; --u) {
        g.ActiveTexture(GL_TEXTURE0 + u);
        g.BindTexture(GL_TEXTURE_2D, 0);
    }
    g.BindVertexArray(0);
    g.UseProgram(0);
    g.BindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool Interpolator::draw(GLuint fbo, int x, int y, int w, int h, GLuint prevTex, GLuint curTex, float t) {
    if (!ready_ || !flowTmp_.tex) return false;
    auto& g = *gl_;
    g.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    g.Viewport(x, y, w, h);
    g.UseProgram(interpProg_);
    g.BindVertexArray(vao_);
    g.Uniform4f(interp_.rect, 0.0f, 0.0f, 1.0f, 1.0f);
    g.Uniform1i(interp_.prev, 0);
    g.Uniform1i(interp_.cur, 1);
    g.Uniform1i(interp_.flow, 2);
    g.Uniform1f(interp_.t, std::clamp(t, 0.0f, 1.0f));
    bindTex(0, prevTex, true);
    bindTex(1, curTex, true);
    bindTex(2, flowTmp_.tex, true);
    g.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    for (int u = 2; u >= 0; --u) {
        g.ActiveTexture(GL_TEXTURE0 + u);
        g.BindTexture(GL_TEXTURE_2D, 0);
    }
    g.BindVertexArray(0);
    g.UseProgram(0);
    return true;
}

}  // namespace rrm::lr
