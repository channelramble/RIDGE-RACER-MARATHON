// fg_eval: scores the Metal frame generation (framegen.mm) offline on consecutive game frames,
// e.g. the "-real" images RRM_FRAMEGEN_DUMP saves. For each triple F0, F1, F2 of distinct frames it
// generates the middle image from F0 and F2 and compares it with the real F1 (PSNR), next to a
// plain blend and repeating F0, the same test as launch_tool --eval-interp for the GL version.
// Also reports the GPU time of a frame's motion search (push) and of one generated image (render).
//
//   fg_eval <out dir> <frame.png>...      (frames in order; the first triple's images are saved)
//
// Build: xcrun clang++ -std=c++17 -O2 -fobjc-arc tools/framegrab/fg_eval.mm tools/framegrab/framegen.mm
//        -framework Foundation -framework Metal -framework ImageIO -framework CoreGraphics -o fg_eval
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "framegen.h"

namespace {

struct Image {
    size_t w = 0, h = 0;
    std::vector<uint8_t> bgra;
};

bool load(const char* path, Image& out) {
    NSURL* url = [NSURL fileURLWithPath:@(path)];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!src) return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return false;
    out.w = CGImageGetWidth(img);
    out.h = CGImageGetHeight(img);
    out.bgra.assign(out.w * out.h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(out.bgra.data(), out.w, out.h, 8, out.w * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGContextDrawImage(ctx, CGRectMake(0, 0, out.w, out.h), img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    return true;
}

void save(const std::string& path, const Image& im) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate((void*)im.bgra.data(), im.w, im.h, 8, im.w * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
    CGImageDestinationAddImage(dst, img, nullptr);
    CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}

double psnr(const Image& a, const Image& b) {
    double se = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.bgra.size() && i < b.bgra.size(); ++i) {
        if (i % 4 == 3) continue;
        const double d = double(a.bgra[i]) - double(b.bgra[i]);
        se += d * d;
        ++n;
    }
    const double mse = n ? se / n : 0;
    return mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

id<MTLTexture> texture(id<MTLDevice> dev, const Image& im, MTLTextureUsage usage) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:im.w
                                                                                height:im.h
                                                                             mipmapped:NO];
    d.usage = usage;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    [t replaceRegion:MTLRegionMake2D(0, 0, im.w, im.h) mipmapLevel:0 withBytes:im.bgra.data() bytesPerRow:im.w * 4];
    return t;
}

double gpuMs(id<MTLCommandBuffer> cb) { return (cb.GPUEndTime - cb.GPUStartTime) * 1000.0; }

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 5) {
            fprintf(stderr, "usage: fg_eval <out dir> <frame.png> <frame.png> <frame.png>...\n");
            return 2;
        }
        const std::string outDir = argv[1];
        [[NSFileManager defaultManager] createDirectoryAtPath:@(outDir.c_str()) withIntermediateDirectories:YES attributes:nil error:nil];
        std::vector<Image> frames;
        for (int i = 2; i < argc; ++i) {
            Image im;
            if (!load(argv[i], im)) {
                fprintf(stderr, "cannot read %s\n", argv[i]);
                return 1;
            }
            if (!frames.empty() && im.bgra == frames.back().bgra) continue;  // repeated frame (30 fps content)
            frames.push_back(std::move(im));
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        rrm::fg::FrameGen fg;
        double sumI = 0, sumB = 0, sumR = 0, pushMs = 0, renderMs = 0;
        int done = 0;
        for (size_t i = 0; i + 2 < frames.size(); i += 2) {
            const Image &f0 = frames[i], &f1 = frames[i + 1], &f2 = frames[i + 2];
            if (f0.w != f2.w || f0.h != f2.h) continue;
            if (!fg.ensure(dev, MTLPixelFormatBGRA8Unorm, f0.w, f0.h)) return 1;
            id<MTLTexture> t0 = texture(dev, f0, MTLTextureUsageShaderRead);
            id<MTLTexture> t2 = texture(dev, f2, MTLTextureUsageShaderRead);
            id<MTLTexture> out = texture(dev, f0, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            fg.push(cb, t0);
            [cb commit];
            cb = [queue commandBuffer];
            const uint64_t n = fg.push(cb, t2);
            [cb commit];
            [cb waitUntilCompleted];
            pushMs += gpuMs(cb);
            cb = [queue commandBuffer];
            fg.render(cb, out, n, 0.5f);
            [cb commit];
            [cb waitUntilCompleted];
            renderMs += gpuMs(cb);
            Image mid = f0, blend = f0;
            [out getBytes:mid.bgra.data() bytesPerRow:mid.w * 4 fromRegion:MTLRegionMake2D(0, 0, mid.w, mid.h) mipmapLevel:0];
            for (size_t k = 0; k < blend.bgra.size(); ++k) blend.bgra[k] = (uint8_t)((f0.bgra[k] + f2.bgra[k] + 1) / 2);
            const double pI = psnr(mid, f1), pB = psnr(blend, f1), pR = psnr(f0, f1);
            printf("triple %zu: %zux%zu  generated %.2f dB  blend %.2f dB  repeat %.2f dB\n", i, f0.w, f0.h, pI, pB, pR);
            sumI += pI;
            sumB += pB;
            sumR += pR;
            if (done++ == 0) {
                save(outDir + "/f0.png", f0);
                save(outDir + "/f1-real.png", f1);
                save(outDir + "/f2.png", f2);
                save(outDir + "/f1-generated.png", mid);
                save(outDir + "/f1-blend.png", blend);
            }
        }
        if (!done) {
            fprintf(stderr, "need at least three distinct frames of one size\n");
            return 1;
        }
        printf("mean over %d: generated %.2f dB  blend %.2f dB  repeat %.2f dB\n", done, sumI / done, sumB / done, sumR / done);
        (void)pushMs;
        (void)renderMs;
        // Warm timing: the last two frames pushed again and again (first uses of new textures cost extra).
        const Image& a = frames[frames.size() - 2];
        const Image& b = frames.back();
        id<MTLTexture> ta = texture(dev, a, MTLTextureUsageShaderRead);
        id<MTLTexture> tb = texture(dev, b, MTLTextureUsageShaderRead);
        id<MTLTexture> out = texture(dev, a, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        std::vector<double> push, render;
        for (int i = 0; i < 40; ++i) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            const uint64_t n = fg.push(cb, (i % 2) ? tb : ta);
            [cb commit];
            [cb waitUntilCompleted];
            push.push_back(gpuMs(cb));
            cb = [queue commandBuffer];
            fg.render(cb, out, n, 0.5f);
            [cb commit];
            [cb waitUntilCompleted];
            render.push_back(gpuMs(cb));
        }
        std::sort(push.begin(), push.end());
        std::sort(render.begin(), render.end());
        printf("GPU time (warm, median of 40): copy + motion search %.2f ms per game frame, %.2f ms per generated image\n",
               push[push.size() / 2], render[render.size() / 2]);
    }
    return 0;
}
