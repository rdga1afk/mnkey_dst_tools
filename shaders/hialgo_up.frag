#version 450 core
// DE-312 "hialgo": neighbour-clamped upscale + clamped sharpening for fractional render scales (0.67 / 0.75 / 0.5), written from the description in docs/research/HIALGO_BOOST_2026-10.md §3 and
// HIALGO_SWITCH_0_2_2026-11-14.md (HiAlgo is closed source: no HiAlgo shader text is used). Input: the internal-resolution LDR frame (after tonemap and FXAA, gamma). SDL_GPU SPIR-V sets: samplers = set 2, UBO = set 3.
//  1. each output pixel has a fractional source position; the colour is interpolated from the nearest source texels (mode 0: 4-tap bilinear, mode 1: 4x4 Catmull-Rom as 9 bilinear taps);
//  2. the result is clamped to the min / max of the 4 nearest texels (no ringing, no halos);
//  3. sharpening: sharp = (colour - mean of the 4 nearest texels) / 2, colour = clamp(colour + sharp, min4, max4).
// align 0: pixel centres (output pixel x maps to (x + 0.5) * s - 0.5), align 1 ("x2"): corner-aligned parity grid of the exact 2x case (output pixel 2i = source texel i, the others fall half way).
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_src;
layout(std140, set = 3, binding = 0) uniform HialgoUBO {
    vec2  u_src_size;    // internal frame size in pixels
    vec2  u_out_size;    // output size in pixels
    float u_mode;        // 0 bilinear, 1 Catmull-Rom
    float u_sharp;       // 1 = the sharpening above, 0 = off
    float u_align;       // 0 centres, 1 corner-aligned (x2)
    float _pad;
};

void main() {
    vec2 outp = v_uv * u_out_size - 0.5;                       // output pixel index (fractional only through the interpolation of v_uv)
    vec2 ratio = u_src_size / u_out_size;
    vec2 p = (u_align > 0.5) ? outp * ratio : (outp + 0.5) * ratio - 0.5;   // source position in texel index units
    vec2 i0 = floor(p), f = p - i0;
    vec2 inv = 1.0 / u_src_size;
    ivec2 lim = ivec2(u_src_size) - 1;
    ivec2 b = ivec2(i0);
    vec3 c00 = texelFetch(u_src, clamp(b, ivec2(0), lim), 0).rgb;
    vec3 c10 = texelFetch(u_src, clamp(b + ivec2(1, 0), ivec2(0), lim), 0).rgb;
    vec3 c01 = texelFetch(u_src, clamp(b + ivec2(0, 1), ivec2(0), lim), 0).rgb;
    vec3 c11 = texelFetch(u_src, clamp(b + ivec2(1, 1), ivec2(0), lim), 0).rgb;
    vec3 mn = min(min(c00, c10), min(c01, c11)), mx = max(max(c00, c10), max(c01, c11));
    vec3 col;
    if (u_mode < 0.5) {
        col = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
    } else {   // Catmull-Rom weights (a = -0.5) on the 4x4 neighbourhood, folded into 3x3 bilinear taps
        vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
        vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
        vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
        vec2 w3 = f * f * (-0.5 + 0.5 * f);
        vec2 s12 = w1 + w2;
        vec2 off12 = w2 / s12;
        vec2 t0 = (i0 - 0.5) * inv, t12 = (i0 + 0.5 + off12) * inv, t3 = (i0 + 2.5) * inv;   // texel i has its centre at (i + 0.5) / size
        col = (texture(u_src, vec2(t0.x, t0.y)).rgb * w0.x * w0.y + texture(u_src, vec2(t12.x, t0.y)).rgb * s12.x * w0.y + texture(u_src, vec2(t3.x, t0.y)).rgb * w3.x * w0.y
             + texture(u_src, vec2(t0.x, t12.y)).rgb * w0.x * s12.y + texture(u_src, vec2(t12.x, t12.y)).rgb * s12.x * s12.y + texture(u_src, vec2(t3.x, t12.y)).rgb * w3.x * s12.y
             + texture(u_src, vec2(t0.x, t3.y)).rgb * w0.x * w3.y + texture(u_src, vec2(t12.x, t3.y)).rgb * s12.x * w3.y + texture(u_src, vec2(t3.x, t3.y)).rgb * w3.x * w3.y);
    }
    col = clamp(col, mn, mx);
    if (u_sharp > 0.5) {
        vec3 mean4 = 0.25 * (c00 + c10 + c01 + c11);
        col = clamp(col + 0.5 * (col - mean4), mn, mx);
    }
    out_color = vec4(col, 1.0);
}
