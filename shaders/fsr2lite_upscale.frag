#version 450 core
#define REQUEST_NDC_Y_UP   // our NDC y points up while the texture v points down

// Variant 5 "fsr2lite": our own temporal upscaler on the FSR 2 model (coordinator 6932774009). Same bindings and UBO as sgsr2_upscale.frag; the motion / depth clip texture comes from sgsr2_convert.frag
// (nearest-depth dilation of the motion, depth separation clip). The structure follows GPUOpen-Effects/FidelityFX-FSR2 (MIT, Copyright (c) 2022-2023 Advanced Micro Devices, Inc.):
// Lanczos-2 reconstruction of the jittered samples (reduced to 3x3 taps), YCoCg rectification of the history, thin-feature lock, luma instability, a per-pixel accumulation age.
// History texture: rgb = colour, a = accumulated sample count (0..64).

layout(location = 0) out vec4 Output;
layout(location = 0) in vec2 texCoord;

layout(set = 2, binding = 0) uniform sampler2D PrevOutput;
layout(set = 2, binding = 1) uniform sampler2D MotionDepthClipAlphaBuffer;
layout(set = 2, binding = 2) uniform sampler2D InputColor;

layout(std140, set = 3, binding = 0) uniform Params
{
    vec4                 clipToPrevClip[4];
    vec2                 renderSize;
    vec2                 outputSize;
    vec2                 renderSizeRcp;
    vec2                 outputSizeRcp;
    vec2                 jitterOffset;
    vec2                 scaleRatio;
    float                cameraFovAngleHor;
    float                minLerpContribution;
    float                reset;
    uint                 bSameCamera;
} params;

vec3 RGBToYCoCg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 YCoCgToRGB(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }

float Kernel2(float x)   // Catmull-Rom, support 2: the closest sin-free stand-in for the Lanczos-2 lobe (sin() cost ~3 ms at 1080p on Gen9)
{
    x = abs(x);
    float x2 = x * x;
    if (x < 1.0) return 1.5 * x2 * x - 2.5 * x2 + 1.0;
    if (x < 2.0) return -0.5 * x2 * x + 2.5 * x2 - 4.0 * x + 2.0;
    return 0.0;
}

// thin bright / dark ridge in the 3x3 neighbourhood (FSR 2 ComputeThinFeatureConfidence)
bool ThinFeature(float l[9])
{
    float nuc = l[4]; uint mask = 1u << 4; float dmin = 3.4e38, dmax = 0.0;
    for (int i = 0; i < 9; ++i) {
        if (i == 4) continue;
        float d = max(l[i], nuc) / max(min(l[i], nuc), 1e-6);
        if (d > 0.0 && d < 1.05) mask |= 1u << uint(i); else { dmin = min(dmin, l[i]); dmax = max(dmax, l[i]); }
    }
    if (!(nuc > dmax || nuc < dmin)) return false;
    const uint rej[4] = uint[4](0x01Bu, 0x036u, 0x0D8u, 0x1B0u);
    for (int i = 0; i < 4; ++i) if ((mask & rej[i]) == rej[i]) return false;
    return true;
}

void main()
{
    const bool still = (params.bSameCamera & 1u) != 0u;
    vec2 Hruv = texCoord;
    vec2 Jitteruv = clamp(Hruv + params.jitterOffset * params.renderSizeRcp, vec2(0.0), vec2(1.0));
    ivec2 InputPos = ivec2(Jitteruv * params.renderSize);
    ivec2 lim = ivec2(params.renderSize) - ivec2(1);

    vec3 mda = textureLod(MotionDepthClipAlphaBuffer, Jitteruv, 0.0).xyz;
    vec2 Motion = mda.xy;
    float depthclip = mda.z;
    vec2 PrevUV = vec2(clamp(-0.5 * Motion.x + Hruv.x, 0.0, 1.0), clamp(0.5 * Motion.y + Hruv.y, 0.0, 1.0));
    float motion_len = length(Motion * params.outputSize);

    // position of the output pixel centre in render pixels, relative to the 3x3 sample centres of this frame
    vec2 srcpos = vec2(InputPos) + vec2(0.5) - params.jitterOffset;
    vec2 outpos = Hruv * params.renderSize;
    vec2 rel = srcpos - outpos;

    vec3 col = vec3(0.0); float wsum = 0.0;
    vec3 ymin = vec3(3.4e38), ymax = vec3(-3.4e38);
    const ivec2 offs[5] = ivec2[5](ivec2(0, 0), ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));   // 5 taps: the cross (the corners weigh little and are fetched only for the thin-feature test)
    for (int i = 0; i < 5; ++i) {
        vec3 c = texelFetch(InputColor, clamp(InputPos + offs[i], ivec2(0), lim), 0).xyz;
        vec3 yc = RGBToYCoCg(c);
        ymin = min(ymin, yc); ymax = max(ymax, yc);
        vec2 d = rel + vec2(offs[i]);
        float w = Kernel2(d.x) * Kernel2(d.y);
        col += c * w; wsum += w;
    }
    vec3 recon = col / max(wsum, 1e-4);
    recon = clamp(recon, vec3(0.0), vec3(65504.0));

    vec4 hist = textureLod(PrevOutput, PrevUV, 0.0);
    vec3 histY = RGBToYCoCg(hist.rgb);
    float age = hist.a;

    // rectify: clip the history into the box of the current neighbourhood, slightly expanded
    vec3 pad = (ymax - ymin) * 0.1 + vec3(0.002);
    vec3 histC = clamp(histY, ymin - pad, ymax + pad);
    float outside = any(notEqual(histC, histY)) ? 1.0 : 0.0;
    float reconY = 0.25 * recon.r + 0.5 * recon.g + 0.25 * recon.b;
    float lumaInstability = abs(histY.x - reconY) / max(max(histY.x, reconY), 1e-3);
    // a still camera has no motion to ghost: the converged history is kept unless it differs strongly (animated pixels), so the jitter phase does not move the box around it
    if (still && lumaInstability < 0.35) { histC = histY; outside = 0.0; }

    // thin-feature lock: a stable thin ridge keeps its history
    bool lock = false;
    if (motion_len < 0.5 && lumaInstability < 0.25) {
        float luma[9]; int k = 0;
        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x, ++k) { vec3 c = texelFetch(InputColor, clamp(InputPos + ivec2(x, y), ivec2(0), lim), 0).xyz; luma[k] = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b; }
        lock = ThinFeature(luma);
    }

    float maxAge = still ? 64.0 : 16.0;
    age = min(age, maxAge);
    age *= (1.0 - depthclip);                                   // disocclusion / depth separation: start over
    age *= mix(1.0, lock ? 0.9 : 0.25, outside);               // history outside the box: forget most of it, less when locked
    age *= (1.0 - clamp(lumaInstability * 2.0, 0.0, 0.5) * (still ? 0.25 : 1.0));     // luma instability shortens the memory
    if (params.reset > 0.5) age = 0.0;

    float wh = age / (age + 1.0);                               // history weight, running average
    vec3 histRGB = YCoCgToRGB(histC);
    vec3 outc = mix(recon, histRGB, wh);
    Output = vec4(outc, min(age + 1.0, maxAge));
}
