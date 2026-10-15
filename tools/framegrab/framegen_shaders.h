// Metal source for rrm frame generation: the same motion-compensated interpolation as the
// in-app player (src/libretro/Interpolator.cpp), as compute kernels plus one render pass that
// writes the generated image into a drawable of framegrab's own layer. Compiled at run time.
#pragma once

static const char* kFramegenMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

constexpr sampler kLinear(filter::linear, address::clamp_to_edge, coord::normalized);

struct SearchParams {
    float2 texel;     // 1 / level size
    float step;       // candidate spacing in texels
    int radius;       // square search radius in steps
    int hasPrev;      // a predicted field exists
};

static inline float2 uvOf(uint2 gid, uint w, uint h) { return (float2(gid) + 0.5) / float2(w, h); }

// Downsample of the colour image to luma (4x or more): four bilinear taps a quarter of the output
// texel off-centre (one source texel at 4x = a 4x4 box).
kernel void lumaFromColor(texture2d<float, access::sample> src [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(1)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float2 uv = uvOf(gid, dst.get_width(), dst.get_height());
    float2 o = 0.25 / float2(dst.get_width(), dst.get_height());
    const float3 w = float3(0.299, 0.587, 0.114);
    float l = dot(src.sample(kLinear, uv + float2(-o.x, -o.y)).rgb, w) +
              dot(src.sample(kLinear, uv + float2(o.x, -o.y)).rgb, w) +
              dot(src.sample(kLinear, uv + float2(-o.x, o.y)).rgb, w) +
              dot(src.sample(kLinear, uv + float2(o.x, o.y)).rgb, w);
    dst.write(float4(0.25 * l, 0.0, 0.0, 1.0), gid);
}

// 2x luma downsample: one bilinear tap at the 2x2 block corner.
kernel void lumaDown(texture2d<float, access::sample> src [[texture(0)]],
                     texture2d<float, access::write> dst [[texture(1)]],
                     uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    dst.write(float4(src.sample(kLinear, uvOf(gid, dst.get_width(), dst.get_height())).r, 0.0, 0.0, 1.0), gid);
}

static inline float cost(texture2d<float, access::sample> a, texture2d<float, access::sample> b, float2 uv,
                         float2 texel, float2 v) {
    float s = 0.0;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i) {
            float2 q = uv + float2(i, j) * texel;
            s += abs(a.sample(kLinear, q - 0.5 * v).r - b.sample(kLinear, q + 0.5 * v).r);
        }
    return s;
}

// Symmetric block matching (vectors in UV units, defined on the in-between frame's grid).
kernel void search(texture2d<float, access::sample> a [[texture(0)]],
                   texture2d<float, access::sample> b [[texture(1)]],
                   texture2d<float, access::sample> prevFlow [[texture(2)]],
                   texture2d<float, access::write> outFlow [[texture(3)]],
                   constant SearchParams& p [[buffer(0)]],
                   uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= outFlow.get_width() || gid.y >= outFlow.get_height()) return;
    float2 uv = uvOf(gid, outFlow.get_width(), outFlow.get_height());
    float2 base = p.hasPrev ? prevFlow.sample(kLinear, uv).xy : float2(0.0);
    float2 best = float2(0.0);
    float bestCost = cost(a, b, uv, p.texel, best) * 0.97 - 0.01;  // standing still wins ties
    if (p.hasPrev) {
        float2 n[4] = {prevFlow.sample(kLinear, uv + float2(4.0 * p.texel.x, 0.0)).xy,
                       prevFlow.sample(kLinear, uv - float2(4.0 * p.texel.x, 0.0)).xy,
                       prevFlow.sample(kLinear, uv + float2(0.0, 4.0 * p.texel.y)).xy,
                       prevFlow.sample(kLinear, uv - float2(0.0, 4.0 * p.texel.y)).xy};
        for (int k = 0; k < 4; ++k) {
            float c = cost(a, b, uv, p.texel, n[k]) + 0.02;
            if (c < bestCost) { bestCost = c; best = n[k]; }
        }
        float c0 = cost(a, b, uv, p.texel, base);
        if (c0 < bestCost) { bestCost = c0; best = base; }
    }
    float2 center = best;
    for (int y = -p.radius; y <= p.radius; ++y)
        for (int x = -p.radius; x <= p.radius; ++x) {
            if (x == 0 && y == 0) continue;
            float2 d = float2(x, y) * p.step;
            float2 v = center + d * p.texel;
            float c = cost(a, b, uv, p.texel, v) + 0.01 * length(d);
            if (c < bestCost) { bestCost = c; best = v; }
        }
    outFlow.write(float4(best, 0.0, 1.0), gid);
}

// 3x3 vector median.
kernel void median(texture2d<float, access::read> flow [[texture(0)]],
                   texture2d<float, access::write> outFlow [[texture(1)]],
                   uint2 gid [[thread_position_in_grid]]) {
    int w = int(flow.get_width()), h = int(flow.get_height());
    if (int(gid.x) >= w || int(gid.y) >= h) return;
    float2 v[9];
    int k = 0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
            v[k++] = flow.read(uint2(clamp(int(gid.x) + i, 0, w - 1), clamp(int(gid.y) + j, 0, h - 1))).xy;
    float2 best = v[4];
    float bestSum = 1e9;
    for (int s = 0; s < 9; ++s) {
        float sum = 0.0;
        for (int t = 0; t < 9; ++t) sum += length(v[s] - v[t]);
        if (sum < bestSum) { bestSum = sum; best = v[s]; }
    }
    outFlow.write(float4(best, 0.0, 1.0), gid);
}

struct QuadOut {
    float4 position [[position]];
    float2 uv;
};

vertex QuadOut quadVertex(uint vid [[vertex_id]]) {
    float2 p = float2(float(vid & 1), float(vid >> 1));
    QuadOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

// The in-between image at time t (0 = previous frame, 1 = newest).
fragment float4 interpFragment(QuadOut in [[stage_in]],
                               texture2d<float, access::sample> prev [[texture(0)]],
                               texture2d<float, access::sample> cur [[texture(1)]],
                               texture2d<float, access::sample> flow [[texture(2)]],
                               constant float& t [[buffer(0)]]) {
    float2 v = flow.sample(kLinear, in.uv).xy;
    float3 a = prev.sample(kLinear, in.uv - t * v).rgb;
    float3 b = cur.sample(kLinear, in.uv + (1.0 - t) * v).rgb;
    float3 a0 = prev.sample(kLinear, in.uv).rgb;
    float3 b0 = cur.sample(kLinear, in.uv).rgb;
    float em = dot(abs(a - b), float3(1.0 / 3.0));
    float e0 = dot(abs(a0 - b0), float3(1.0 / 3.0));
    float3 moved = mix(a, b, t);
    float3 still = mix(a0, b0, t);
    float useStill = smoothstep(-0.01, 0.01, em - e0);
    // Where neither matches (occlusion, a changing HUD figure), the nearer frame, unmoved if that
    // pair fits better: text that changes in place is not smeared by the motion around it.
    float3 nearest = mix(t < 0.5 ? a : b, t < 0.5 ? a0 : b0, useStill);
    float trust = 1.0 - smoothstep(0.08, 0.2, min(em, e0));
    return float4(mix(nearest, mix(moved, still, useStill), trust), 1.0);
}
)METAL";
