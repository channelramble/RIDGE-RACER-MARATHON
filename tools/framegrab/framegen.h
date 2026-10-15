// Frame generation for external Metal apps (injected by rrm_framegrab): keeps the last presented
// frames in a small ring, estimates motion between consecutive ones (framegen_shaders.h) and
// renders in-between images into drawables of framegrab's own layer over the app's.
//
// Frames are numbered by push(). push() encodes the copy of a frame into the ring and its motion
// search; render() and copyFrame() are encoded later, by the presenter, for the pair (frame - 1,
// frame). The ring keeps kSlots frames, so frame m's push overwrites what frame m - kSlots + 1's
// render reads. The caller encodes everything into one command queue and checks canRender() /
// canCopy() while holding the lock that push() is called under, up to the commit: the GPU then
// runs each render before any later push.
#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <vector>

namespace rrm::fg {

class FrameGen {
public:
    static constexpr int kSlots = 5;

    // (Re)creates resources for this device / drawable format / size. False if unusable.
    bool ensure(id<MTLDevice> device, MTLPixelFormat format, NSUInteger width, NSUInteger height);
    // Encodes into `cb`: copy `frame` into the ring, then (with a previous frame) the motion search.
    // Returns the frame's number (0 when unusable).
    uint64_t push(id<MTLCommandBuffer> cb, id<MTLTexture> frame);
    // True when frame `n` has a predecessor of the same size and both are still in the ring.
    bool canRender(uint64_t n) const { return n >= sizeEpoch_ + 1 && n <= pushed_ && pushed_ < n + kSlots - 1; }
    // True while frame n itself is still in the ring (copyFrame).
    bool canCopy(uint64_t n) const { return n >= sizeEpoch_ && n <= pushed_ && pushed_ < n + kSlots; }
    uint64_t lastPushed() const { return pushed_; }
    NSUInteger width() const { return width_; }
    MTLPixelFormat format() const { return format_; }
    NSUInteger height() const { return height_; }
    // Encodes into `cb` the image at t (0 = frame n-1, 1 = frame n) rendered into `target`.
    void render(id<MTLCommandBuffer> cb, id<MTLTexture> target, uint64_t n, float t);
    // Encodes into `cb` a copy of frame n itself into `target` (same size and format). False if not.
    bool copyFrame(id<MTLCommandBuffer> cb, id<MTLTexture> target, uint64_t n);

private:
    id<MTLTexture> makeTexture(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage usage);
    void dispatch(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> pipe, id<MTLTexture> grid);

    id<MTLDevice> device_ = nil;
    id<MTLComputePipelineState> lumaColor_ = nil, lumaDown_ = nil, search_ = nil, median_ = nil;
    id<MTLRenderPipelineState> interp_ = nil;
    MTLPixelFormat format_ = MTLPixelFormatInvalid;
    NSUInteger width_ = 0, height_ = 0;
    id<MTLTexture> history_[kSlots] = {};  // full-size copies, frame n in slot n % kSlots
    id<MTLTexture> flow_[kSlots] = {};     // final motion field for the pair (n-1, n), slot n % kSlots
    uint64_t pushed_ = 0;                  // number of the newest frame
    uint64_t sizeEpoch_ = 0;               // first frame number at the current size
    std::vector<id<MTLTexture>> lumaPrev_, lumaCur_, search_levels_;  // scratch, per pyramid level
    id<MTLTexture> scratch_ = nil;
    bool failed_ = false;
};

}  // namespace rrm::fg
