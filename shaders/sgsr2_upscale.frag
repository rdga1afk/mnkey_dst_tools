#version 450 core
#define REQUEST_NDC_Y_UP   // our NDC y points up while the texture v points down (same as terrain_skyvis_apply.frag)

// Port of Qualcomm's Snapdragon GSR 2 (2-pass fragment variant, snapdragon-gsr commit d926f074, sgsr/v2/include/glsl_2_pass_fs/sgsr2_upscale.fs) to our SDL_GPU SPIR-V sets:
// fragment samplers = set 2, fragment uniform buffers = set 3; the vertex stage is upscale.vert (v_uv at location 0). BSD-3-Clause, see engine/THIRD_PARTY.md.
// Original copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved. SPDX-License-Identifier: BSD-3-Clause


float FastLanczos(float base)
{
	float y = base - 1.0f;
	float y2 = y * y;
	float y_temp = 0.75f * y + y2;
	return y_temp * y2;
}

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
    uint                       bSameCamera;
} params;

// FSR 2 parts added to the SGSR 2 port (DE-311): variance box and history clip in YCoCg, thin-feature lock. Ported in our own words from
// GPUOpen-Effects/FidelityFX-FSR2 (MIT, Copyright (c) 2022-2023 Advanced Micro Devices, Inc.; src/ffx-fsr2-api/shaders: ffx_fsr2_common.h RGBToYCoCg, ffx_fsr2_accumulate.h RectifyHistory, ffx_fsr2_lock.h ComputeThinFeatureConfidence), see engine/THIRD_PARTY.md.
// bSameCamera bit 1 (value 2) = switch the FSR 2 parts off (cost measurement only).
vec3 RGBToYCoCg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 YCoCgToRGB(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }

// thin bright / dark ridge in the 3x3 neighbourhood of the render pixel (FSR 2 ComputeThinFeatureConfidence, luma = YCoCg Y)
bool ThinFeature(ivec2 pos) {
    float l[9]; int k = 0;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x, ++k) {
        ivec2 q = clamp(pos + ivec2(x, y), ivec2(0), ivec2(params.renderSize) - ivec2(1));
        vec3 c = texelFetch(InputColor, q, 0).xyz; l[k] = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    }
    float nuc = l[4]; uint mask = 1u << 4; float dmin = 3.4e38, dmax = 0.0;
    for (int i = 0; i < 9; ++i) {
        if (i == 4) continue;
        float d = max(l[i], nuc) / max(min(l[i], nuc), 1e-6);
        if (d > 0.0 && d < 1.05) mask |= 1u << uint(i); else { dmin = min(dmin, l[i]); dmax = max(dmax, l[i]); }
    }
    if (!(nuc > dmax || nuc < dmin)) return false;
    const uint rej[4] = uint[4](0x01Bu, 0x036u, 0x0D8u, 0x1B0u);   // upper left, upper right, lower left, lower right 2x2 blocks
    for (int i = 0; i < 4; ++i) if ((mask & rej[i]) == rej[i]) return false;
    return true;
}

void main()
{
    const bool fsr2 = (params.bSameCamera & 2u) == 0u;
    const bool still = (params.bSameCamera & 1u) != 0u;
    float Biasmax_viewportXScale = params.scaleRatio.x;
    float scalefactor = params.scaleRatio.y;

    vec2 Hruv = texCoord;

    vec2 Jitteruv;
    Jitteruv.x = clamp(Hruv.x + (params.jitterOffset.x * params.renderSizeRcp.x), 0.0, 1.0);
    Jitteruv.y = clamp(Hruv.y + (params.jitterOffset.y * params.renderSizeRcp.y), 0.0, 1.0);

    ivec2 InputPos = ivec2(Jitteruv * params.renderSize);

    vec3 mda = textureLod(MotionDepthClipAlphaBuffer, Jitteruv, 0.0).xyz;
    vec2 Motion = mda.xy;

    vec2 PrevUV;
    PrevUV.x = clamp(-0.5 * Motion.x + Hruv.x, 0.0, 1.0);
#ifdef REQUEST_NDC_Y_UP
    PrevUV.y = clamp(0.5 * Motion.y + Hruv.y, 0.0, 1.0);
#else
    PrevUV.y = clamp(-0.5 * Motion.y + Hruv.y, 0.0, 1.0);
#endif

    float depthfactor = mda.z;

    vec3 HistoryColor = textureLod(PrevOutput, PrevUV, 0.0).xyz;

    /////upsample and compute box
    vec4 Upsampledcw = vec4(0.0);
    float biasmax = Biasmax_viewportXScale ;
    float biasmin = max(1.0f, 0.3 + 0.3 * biasmax);
    float biasfactor = 0.25f * depthfactor;
    float kernelbias = mix(biasmax, biasmin, biasfactor);
    float motion_viewport_len = length(Motion * params.outputSize);
    float curvebias = mix(-2.0, -3.0, clamp(motion_viewport_len * 0.02, 0.0, 1.0));

    vec3 rectboxcenter = vec3(0.0);
    vec3 rectboxvar = vec3(0.0);
    float rectboxweight = 0.0;
    vec2 srcpos = vec2(InputPos) + vec2(0.5) - params.jitterOffset;

    kernelbias *= 0.5f;
    float kernelbias2 = kernelbias * kernelbias;
    vec2 srcpos_srcOutputPos = srcpos - Hruv * params.renderSize;  //srcOutputPos = Hruv * params.renderSize;
    vec3 rectboxmin;
    vec3 rectboxmax;
    vec3 rgbmin;
    vec3 rgbmax;
    vec3 topMid = texelFetch(InputColor, InputPos + ivec2(0, 1), 0).xyz;
    {

        vec3 samplecolor = topMid;

        vec3 yc = fsr2 ? RGBToYCoCg(samplecolor) : samplecolor;
        vec2 baseoffset = srcpos_srcOutputPos + vec2(0.0, 1.0);
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = yc; rgbmin = samplecolor;
        rectboxmax = yc; rgbmax = samplecolor;
        vec3 wsample = yc * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (yc * wsample);
        rectboxweight += boxweight;
    }
    vec3 rightMid = texelFetch(InputColor, InputPos + ivec2(1, 0), 0).xyz;
    {

        vec3 samplecolor = rightMid;

        vec3 yc = fsr2 ? RGBToYCoCg(samplecolor) : samplecolor;
        vec2 baseoffset = srcpos_srcOutputPos + vec2(1.0, 0.0);
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = min(rectboxmin, yc); rgbmin = min(rgbmin, samplecolor);
        rectboxmax = max(rectboxmax, yc); rgbmax = max(rgbmax, samplecolor);
        vec3 wsample = yc * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (yc * wsample);
        rectboxweight += boxweight;
    }
    vec3 leftMid = texelFetch(InputColor, InputPos + ivec2(-1, 0) , 0).xyz;
    {

        vec3 samplecolor = leftMid;

        vec3 yc = fsr2 ? RGBToYCoCg(samplecolor) : samplecolor;
        vec2 baseoffset = srcpos_srcOutputPos + vec2(-1.0, 0.0);
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = min(rectboxmin, yc); rgbmin = min(rgbmin, samplecolor);
        rectboxmax = max(rectboxmax, yc); rgbmax = max(rgbmax, samplecolor);
        vec3 wsample = yc * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (yc * wsample);
        rectboxweight += boxweight;
    }
    vec3 centerMid = texelFetch(InputColor, InputPos + ivec2(0, 0) , 0).xyz;
    {

        vec3 samplecolor = centerMid;

        vec3 yc = fsr2 ? RGBToYCoCg(samplecolor) : samplecolor;
        vec2 baseoffset = srcpos_srcOutputPos;
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = min(rectboxmin, yc); rgbmin = min(rgbmin, samplecolor);
        rectboxmax = max(rectboxmax, yc); rgbmax = max(rgbmax, samplecolor);
        vec3 wsample = yc * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (yc * wsample);
        rectboxweight += boxweight;
    }
    vec3 btmMid = texelFetch(InputColor, InputPos + ivec2(0, -1) , 0).xyz;
    {

        vec3 samplecolor = btmMid;

        vec3 yc = fsr2 ? RGBToYCoCg(samplecolor) : samplecolor;
        vec2 baseoffset = srcpos_srcOutputPos + vec2(0.0, -1.0);
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = min(rectboxmin, yc); rgbmin = min(rgbmin, samplecolor);
        rectboxmax = max(rectboxmax, yc); rgbmax = max(rgbmax, samplecolor);
        vec3 wsample = yc * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (yc * wsample);
        rectboxweight += boxweight;
    }

    //if (params.sameCameraFrmNum!=0u)  //maybe disable this for ultra performance
    if (false)  //maybe disable this for ultra performance, true could generate more realistic output
    {
        {
            vec3 topRight = texelFetch(InputColor, InputPos + ivec2(1, 1), 0).xyz;
            vec3 samplecolor = topRight;
            vec3 yc = fsr2 ? RGBToYCoCg(samplecolor) : samplecolor;
            vec2 baseoffset = srcpos_srcOutputPos + vec2(1.0, 1.0);
            float baseoffset_dot = dot(baseoffset, baseoffset);
            float base = clamp(baseoffset_dot * kernelbias2, 0.0, 1.0);
            float weight = FastLanczos(base);
            Upsampledcw += vec4(samplecolor * weight, weight);
            float boxweight = exp(baseoffset_dot * curvebias);
            rectboxmin = min(rectboxmin, yc); rgbmin = min(rgbmin, samplecolor);
            rectboxmax = max(rectboxmax, yc); rgbmax = max(rgbmax, samplecolor);
            vec3 wsample = yc * boxweight;
            rectboxcenter += wsample;
            rectboxvar += (yc * wsample);
            rectboxweight += boxweight;
        }
        {
            vec3 topLeft = texelFetch(InputColor, InputPos + ivec2(-1, 1), 0).xyz;
            vec3 samplecolor = topLeft;
            vec3 yc = fsr2 ? RGBToYCoCg(samplecolor) : samplecolor;
            vec2 baseoffset = srcpos_srcOutputPos + vec2(-1.0, 1.0);
            float baseoffset_dot = dot(baseoffset, baseoffset);
            float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
            float weight = FastLanczos(base);
            Upsampledcw += vec4(samplecolor * weight, weight);
            float boxweight = exp(baseoffset_dot * curvebias);
            rectboxmin = min(rectboxmin, yc); rgbmin = min(rgbmin, samplecolor);
            rectboxmax = max(rectboxmax, yc); rgbmax = max(rgbmax, samplecolor);
            vec3 wsample = yc * boxweight;
            rectboxcenter += wsample;
            rectboxvar += (yc * wsample);
            rectboxweight += boxweight;
        }
        {
            vec3 btmRight = texelFetch(InputColor, InputPos + ivec2(1, -1) , 0).xyz;
            vec3 samplecolor = btmRight;
            vec3 yc = fsr2 ? RGBToYCoCg(samplecolor) : samplecolor;
            vec2 baseoffset = srcpos_srcOutputPos + vec2(1.0, -1.0);
            float baseoffset_dot = dot(baseoffset, baseoffset);
            float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
            float weight = FastLanczos(base);
            Upsampledcw += vec4(samplecolor * weight, weight);
            float boxweight = exp(baseoffset_dot * curvebias);
            rectboxmin = min(rectboxmin, yc); rgbmin = min(rgbmin, samplecolor);
            rectboxmax = max(rectboxmax, yc); rgbmax = max(rgbmax, samplecolor);
            vec3 wsample = yc * boxweight;
            rectboxcenter += wsample;
            rectboxvar += (yc * wsample);
            rectboxweight += boxweight;
        }

        {
            vec3 btmLeft = texelFetch(InputColor, InputPos + ivec2(-1, -1) , 0).xyz;
            vec3 samplecolor = btmLeft;
            vec3 yc = fsr2 ? RGBToYCoCg(samplecolor) : samplecolor;
            vec2 baseoffset = srcpos_srcOutputPos + vec2(-1.0, -1.0);
            float baseoffset_dot = dot(baseoffset, baseoffset);
            float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
            float weight = FastLanczos(base);
            Upsampledcw += vec4(samplecolor * weight, weight);
            float boxweight = exp(baseoffset_dot * curvebias);
            rectboxmin = min(rectboxmin, yc); rgbmin = min(rgbmin, samplecolor);
            rectboxmax = max(rectboxmax, yc); rgbmax = max(rgbmax, samplecolor);
            vec3 wsample = yc * boxweight;
            rectboxcenter += wsample;
            rectboxvar += (yc * wsample);
            rectboxweight += boxweight;
        }
    }

    rectboxweight = 1.0 / rectboxweight;
    rectboxcenter *= rectboxweight;
    rectboxvar *= rectboxweight;
    rectboxvar = sqrt(abs(rectboxvar - rectboxcenter * rectboxcenter));

    Upsampledcw.xyz =  clamp(Upsampledcw.xyz / Upsampledcw.w, rgbmin-vec3(0.075), rgbmax+vec3(0.075));
    Upsampledcw.w = Upsampledcw.w * (1.0f / 3.0f) ;

    float baseupdate = 1.0f - depthfactor;
    baseupdate = min(baseupdate, mix(baseupdate, Upsampledcw.w *10.0f, clamp(10.0f* motion_viewport_len, 0.0, 1.0)));
    baseupdate = min(baseupdate, mix(baseupdate, Upsampledcw.w, clamp(motion_viewport_len *0.05f, 0.0, 1.0)));
    float basealpha = baseupdate;

    const float EPSILON = 1.192e-07f;
    float boxscale = max(depthfactor, clamp(motion_viewport_len * 0.05f, 0.0, 1.0));
    float boxsize = mix(scalefactor, 1.0f, boxscale);
    vec3 sboxvar = rectboxvar * boxsize;
    vec3 boxmin = rectboxcenter - sboxvar;
    vec3 boxmax = rectboxcenter + sboxvar;
    rectboxmax = min(rectboxmax, boxmax);
    rectboxmin = max(rectboxmin, boxmin);

    vec3 histbox = fsr2 ? RGBToYCoCg(HistoryColor) : HistoryColor;
    vec3 clampedcolor = clamp(histbox, rectboxmin, rectboxmax);
    if (fsr2) clampedcolor = YCoCgToRGB(clampedcolor);
    float startLerpValue = params.minLerpContribution;
    if ((abs(mda.x) + abs(mda.y)) > 0.000001) startLerpValue = 0.0;
    float lerpcontribution = (any(greaterThan(rectboxmin, histbox)) || any(greaterThan(histbox, rectboxmax))) ? startLerpValue : 1.0f;

    if (fsr2 && motion_viewport_len < 0.5 && ThinFeature(InputPos)) lerpcontribution = max(lerpcontribution, 0.9);   // thin stable feature: keep the history (FSR 2 lock)
    if (still) lerpcontribution = max(lerpcontribution, 0.8);   // OWN CHANGE (not in the Qualcomm shader): a still camera keeps 80 % of an out-of-box history per frame; the full clamp followed the jitter phase (still-to-still mean abs diff 0.3-4 / 255 against 0.005)
    HistoryColor = mix(clampedcolor, HistoryColor, clamp(lerpcontribution, 0.0, 1.0));
    float basemin = min(basealpha, 0.1f);
    basealpha = mix(basemin, basealpha, clamp(lerpcontribution, 0.0, 1.0));

    ////blend color
    float alphasum = max(EPSILON, basealpha + Upsampledcw.w);
    float alpha = clamp(Upsampledcw.w / alphasum, 0.0, 1.0);
    // OWN CHANGE (not in the Qualcomm shader): with a still camera the jittered samples are accumulated slowly (1/64 per frame) instead of ~0.3
    if (still) alpha = min(alpha, 1.0 / 64.0);
    alpha = clamp(alpha + params.reset, 0.0, 1.0);

    Upsampledcw.xyz = mix(HistoryColor, Upsampledcw.xyz, alpha);

    Output = vec4(Upsampledcw.xyz, 0.0);
}