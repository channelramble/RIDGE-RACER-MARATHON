// Motion-compensated frame interpolation for the in-app player: from two consecutive
// core frames (already scaled to the viewport) it estimates where every part of the
// image moved, then draws in-between images at any time t. This is image-based frame
// generation (no game data): it adds one frame of latency, and fast or overlapping
// motion can show artifacts, where it falls back to a plain blend.
//
// GL 3.2 core / GLES 3.0 fragment-shader passes (macOS has no compute shaders):
//   1. luma pyramid of both frames (1/4 of the viewport, then halving)
//   2. symmetric block matching, coarse to fine: a vector v at point p pairs prev(p - v/2)
//      with cur(p + v/2), so the field lives on the in-between frame's grid (no holes)
//   3. sub-pixel refinement and a 3x3 vector median at the finest level
//   4. per present: prev(p - t*v) and cur(p + (1-t)*v), checked against zero motion
#pragma once

#include <string>
#include <vector>

#include "libretro/GlFunctions.h"

namespace rrm::lr {

class Interpolator {
public:
    bool init(GlFunctions* gl, bool gles, std::string& error);
    void shutdown(bool deleteObjects = true);
    bool ready() const { return ready_; }

    // Motion between two RGBA8 textures of size w x h (bottom-up, as Video's history keeps them).
    // False if no field was made (viewport under 64 pixels, targets could not be created):
    // then draw() must not be used for this pair.
    bool estimate(GLuint prevTex, GLuint curTex, int w, int h);
    // The in-between image at t (0 = prev, 1 = cur) into viewport (x, y, w, h) of `fbo`, using
    // the field of the last successful estimate(). False if nothing was drawn.
    bool draw(GLuint fbo, int x, int y, int w, int h, GLuint prevTex, GLuint curTex, float t);

private:
    struct Target {
        GLuint tex = 0, fbo = 0;
        int w = 0, h = 0;
    };
    bool makeTarget(Target& t, int w, int h, bool flow);
    void freeTarget(Target& t);
    bool ensureLevels(int w, int h);
    void freeLevels();
    void bindTex(int unit, GLuint tex, bool linear);
    void pass(const Target& dst);

    GlFunctions* gl_ = nullptr;
    bool ready_ = false, gles_ = false;
    GLuint vao_ = 0;
    GLuint downProg_ = 0, searchProg_ = 0, medianProg_ = 0, interpProg_ = 0;
    struct {
        GLint src, texel, fromColor, rect;
    } down_{};
    struct {
        GLint a, b, prevFlow, hasPrev, texel, radius, step, rect;
    } search_{};
    struct {
        GLint flow, texel, rect;
    } median_{};
    struct {
        GLint prev, cur, flow, t, rect;
    } interp_{};

    int srcW_ = 0, srcH_ = 0;
    std::vector<Target> lumaPrev_, lumaCur_, flow_;  // per level, 0 = finest (1/4 of the viewport)
    Target flowTmp_;
};

}  // namespace rrm::lr
