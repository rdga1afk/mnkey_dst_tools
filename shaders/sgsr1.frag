#version 450 core
// sgsr1.frag -- Snapdragon Game Super Resolution 1 (mobile, RGBA mode: edge direction from the green channel), #198 S1.
// Port of sgsr1_shader_mobile.frag (Qualcomm, BSD-3-Clause, see engine/THIRD_PARTY.md) to our SDL_GPU SPIR-V sets:
// fragment samplers = set 2, fragment uniform buffers = set 3. Replaces the bilinear + CAS-like sharpen of upscale.frag.
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D ps0;
layout(std140, set = 3, binding = 0) uniform SgsrUBO {
    vec4 ViewportInfo;   // xy = 1 / internal size, zw = internal size
};

const float EdgeThreshold = 8.0 / 255.0;
const float EdgeSharpness = 2.0;

float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}
vec2 weightY(float dx, float dy, float c, float std) {
    float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}

void main() {
    vec4 color;
    color.xyz = textureLod(ps0, v_uv, 0.0).xyz;
    color.w = 1.0;

    vec2 imgCoord = (v_uv * ViewportInfo.zw) + vec2(-0.5, 0.5);
    vec2 imgCoordPixel = floor(imgCoord);
    vec2 coord = imgCoordPixel * ViewportInfo.xy;
    vec2 pl = imgCoord - imgCoordPixel;
    vec4 left = textureGather(ps0, coord, 1);

    float edgeVote = abs(left.z - left.y) + abs(color.y - left.y) + abs(color.y - left.z);
    if (edgeVote > EdgeThreshold) {
        coord.x += ViewportInfo.x;

        vec4 right = textureGather(ps0, coord + vec2(ViewportInfo.x, 0.0), 1);
        vec4 upDown;
        upDown.xy = textureGather(ps0, coord + vec2(0.0, -ViewportInfo.y), 1).wz;
        upDown.zw = textureGather(ps0, coord + vec2(0.0,  ViewportInfo.y), 1).yx;

        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        float centre = color.y - mean;

        float sum = abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w)
                  + abs(right.x) + abs(right.y) + abs(right.z) + abs(right.w)
                  + abs(upDown.x) + abs(upDown.y) + abs(upDown.z) + abs(upDown.w);
        float std = 2.181818 / sum;

        vec2 aWY = weightY(pl.x,       pl.y + 1.0, upDown.x, std);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);
        aWY += weightY(pl.x,       pl.y - 2.0, upDown.w, std);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x,   std);
        aWY += weightY(pl.x,       pl.y - 1.0, left.y,   std);
        aWY += weightY(pl.x,       pl.y,       left.z,   std);
        aWY += weightY(pl.x + 1.0, pl.y,       left.w,   std);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x,  std);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y,  std);
        aWY += weightY(pl.x - 2.0, pl.y,       right.z,  std);
        aWY += weightY(pl.x - 1.0, pl.y,       right.w,  std);

        float finalY = aWY.y / aWY.x;
        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        finalY = clamp(EdgeSharpness * finalY, minY, maxY);

        float deltaY = clamp(finalY - centre, -23.0 / 255.0, 23.0 / 255.0);   // smooth high contrast input
        color.xyz = clamp(color.xyz + vec3(deltaY), 0.0, 1.0);
    }
    out_color = vec4(color.xyz, 1.0);
}
