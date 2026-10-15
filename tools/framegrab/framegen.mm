#import "framegen.h"

#include <algorithm>
#include <cstdio>

#include "framegen_shaders.h"

namespace rrm::fg {

namespace {

struct SearchParams {  // matches the MSL struct (float2 aligns it to 8 bytes there)
    float texel[2];
    float step;
    int radius;
    int hasPrev;
    int pad;
};
static_assert(sizeof(SearchParams) == 24, "SearchParams must match framegen_shaders.h");

constexpr NSUInteger kMinLevelWidth = 48;
constexpr NSUInteger kMaxLevelWidth = 240;

}  // namespace

id<MTLTexture> FrameGen::makeTexture(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage usage) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = usage;
    d.storageMode = MTLStorageModePrivate;
    return [device_ newTextureWithDescriptor:d];
}

bool FrameGen::ensure(id<MTLDevice> device, MTLPixelFormat format, NSUInteger width, NSUInteger height) {
    if (failed_) return false;
    if (device == device_ && format == format_ && width == width_ && height == height_ && interp_) return true;
    if (width < 128 || height < 128) return false;
    if (device != device_ || format != format_ || !interp_) {
        device_ = device;
        NSError* error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@(kFramegenMetalSource) options:nil error:&error];
        if (!lib) {
            fprintf(stderr, "[framegen] shader compile failed: %s\n", error.localizedDescription.UTF8String);
            failed_ = true;
            return false;
        }
        auto compute = [&](const char* name) {
            NSError* e = nil;
            id<MTLComputePipelineState> p =
                [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@(name)] error:&e];
            if (!p) fprintf(stderr, "[framegen] pipeline %s: %s\n", name, e.localizedDescription.UTF8String);
            return p;
        };
        lumaColor_ = compute("lumaFromColor");
        lumaDown_ = compute("lumaDown");
        search_ = compute("search");
        median_ = compute("median");
        MTLRenderPipelineDescriptor* rp = [MTLRenderPipelineDescriptor new];
        rp.vertexFunction = [lib newFunctionWithName:@"quadVertex"];
        rp.fragmentFunction = [lib newFunctionWithName:@"interpFragment"];
        rp.colorAttachments[0].pixelFormat = format;
        interp_ = [device newRenderPipelineStateWithDescriptor:rp error:&error];
        if (!lumaColor_ || !lumaDown_ || !search_ || !median_ || !interp_) {
            fprintf(stderr, "[framegen] setup failed: %s\n", error ? error.localizedDescription.UTF8String : "pipeline");
            failed_ = true;
            return false;
        }
        format_ = format;
    }
    width_ = width;
    height_ = height;
    const MTLTextureUsage rw = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    lumaPrev_.clear();
    lumaCur_.clear();
    search_levels_.clear();
    // The search starts at a quarter of the frame, or less above 960 wide (about 240 texels
    // across): at 1080p and 4K the same cost as at 960x540, for at most half a dB less (measured
    // with fg_eval on Ridge Racer 7, Rave Racer and Ridge Racer frames).
    const NSUInteger div = std::max<NSUInteger>(4, (width + kMaxLevelWidth - 1) / kMaxLevelWidth);
    NSUInteger lw = std::max<NSUInteger>(1, width / div), lh = std::max<NSUInteger>(1, height / div);
    while (true) {
        lumaPrev_.push_back(makeTexture(MTLPixelFormatR8Unorm, lw, lh, rw));
        lumaCur_.push_back(makeTexture(MTLPixelFormatR8Unorm, lw, lh, rw));
        search_levels_.push_back(makeTexture(MTLPixelFormatRG16Float, lw, lh, rw));
        if (lw / 2 < kMinLevelWidth || lh / 2 < 8) break;
        lw /= 2;
        lh /= 2;
    }
    scratch_ = makeTexture(MTLPixelFormatRG16Float, search_levels_[0].width, search_levels_[0].height, rw);
    for (int i = 0; i < kSlots; ++i) {
        history_[i] = makeTexture(format, width, height, MTLTextureUsageShaderRead);
        flow_[i] = makeTexture(MTLPixelFormatRG16Float, search_levels_[0].width, search_levels_[0].height, rw);
    }
    sizeEpoch_ = pushed_ + 1;  // frames from the next push on have this size
    fprintf(stderr, "[framegen] ready: %lux%lu format %lu, %zu pyramid levels, from frame %llu\n", (unsigned long)width,
            (unsigned long)height, (unsigned long)format, search_levels_.size(), (unsigned long long)sizeEpoch_);
    return true;
}

void FrameGen::dispatch(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> pipe, id<MTLTexture> grid) {
    [enc setComputePipelineState:pipe];
    MTLSize tg = MTLSizeMake(16, 16, 1);
    MTLSize groups = MTLSizeMake((grid.width + 15) / 16, (grid.height + 15) / 16, 1);
    [enc dispatchThreadgroups:groups threadsPerThreadgroup:tg];
}

uint64_t FrameGen::push(id<MTLCommandBuffer> cb, id<MTLTexture> frame) {
    if (!interp_ || frame.width != width_ || frame.height != height_ || frame.pixelFormat != format_) return 0;
    const uint64_t n = ++pushed_;
    id<MTLTexture> cur = history_[n % kSlots];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:frame toTexture:cur];
    [blit endEncoding];
    if (n < sizeEpoch_ + 1) return n;  // no previous frame at this size yet
    id<MTLTexture> prev = history_[(n - 1) % kSlots];

    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    // 1. luma pyramids of both frames
    for (int which = 0; which < 2; ++which) {
        auto& levels = which == 0 ? lumaPrev_ : lumaCur_;
        for (size_t l = 0; l < levels.size(); ++l) {
            [enc setTexture:(l == 0 ? (which == 0 ? prev : cur) : levels[l - 1]) atIndex:0];
            [enc setTexture:levels[l] atIndex:1];
            dispatch(enc, l == 0 ? lumaColor_ : lumaDown_, levels[l]);
        }
    }
    // 2. coarse-to-fine search, 3. sub-pixel refinement at level 0
    auto search = [&](size_t l, id<MTLTexture> predicted, id<MTLTexture> dst, int radius, float step) {
        SearchParams p{{1.0f / lumaPrev_[l].width, 1.0f / lumaPrev_[l].height}, step, radius, predicted ? 1 : 0, 0};
        [enc setTexture:lumaPrev_[l] atIndex:0];
        [enc setTexture:lumaCur_[l] atIndex:1];
        [enc setTexture:(predicted ? predicted : dst) atIndex:2];
        [enc setTexture:dst atIndex:3];
        [enc setBytes:&p length:sizeof(p) atIndex:0];
        dispatch(enc, search_, dst);
    };
    const size_t coarsest = search_levels_.size() - 1;
    search(coarsest, nil, search_levels_[coarsest], 4, 1.0f);
    for (size_t l = coarsest; l-- > 0;) search(l, search_levels_[l + 1], search_levels_[l], 1, 1.0f);
    search(0, search_levels_[0], scratch_, 1, 0.5f);
    search(0, scratch_, search_levels_[0], 1, 0.25f);
    // 4. outlier removal into this frame's field
    [enc setTexture:search_levels_[0] atIndex:0];
    [enc setTexture:flow_[n % kSlots] atIndex:1];
    dispatch(enc, median_, flow_[n % kSlots]);
    [enc endEncoding];
    return n;
}

void FrameGen::render(id<MTLCommandBuffer> cb, id<MTLTexture> target, uint64_t n, float t) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    [enc setRenderPipelineState:interp_];
    [enc setFragmentTexture:history_[(n - 1) % kSlots] atIndex:0];
    [enc setFragmentTexture:history_[n % kSlots] atIndex:1];
    [enc setFragmentTexture:flow_[n % kSlots] atIndex:2];
    [enc setFragmentBytes:&t length:sizeof(t) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];
}

bool FrameGen::copyFrame(id<MTLCommandBuffer> cb, id<MTLTexture> target, uint64_t n) {
    if (!target || target.width != width_ || target.height != height_ || target.pixelFormat != format_) return false;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:history_[n % kSlots] toTexture:target];
    [blit endEncoding];
    return true;
}

}  // namespace rrm::fg
