#version 450 core
// DE-312 stair-only blur (coordinator 6942977914), from the description in docs/research/HIALGO_BOOST_2026-10.md §3: GRAD = max(|Lc - Lup|, |Lc - Ldown|), w = saturate((GRAD - 0.05) / (0.20 - 0.05)),
// a horizontal blur of up to 2R+1 taps, out = blur * w + orig * (1 - w): it smooths only where there is a vertical step. Written from the description (no HiAlgo shader text). Luma = green channel (as FXAA).
// On the internal frame, after FXAA and before the upscale. Taps R from the UBO (start 8, cap 31). SDL_GPU SPIR-V sets: samplers = set 2, UBO = set 3.
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_src;
layout(std140, set = 3, binding = 0) uniform BlurUBO { vec2 u_rcp; float u_radius; float _pad; };
void main() {
    vec3 c = texture(u_src, v_uv).rgb;
    float lc = c.g;
    float lu = texture(u_src, v_uv - vec2(0.0, u_rcp.y)).g, ld = texture(u_src, v_uv + vec2(0.0, u_rcp.y)).g;
    float grad = max(abs(lc - lu), abs(lc - ld));
    float w = clamp((grad - 0.05) / (0.20 - 0.05), 0.0, 1.0);
    if (w <= 0.0) { out_color = vec4(c, 1.0); return; }
    int R = int(u_radius);
    vec3 acc = c; float n = 1.0;
    for (int i = 1; i <= R; ++i) {
        float o = float(i) * u_rcp.x;
        acc += texture(u_src, v_uv + vec2(o, 0.0)).rgb + texture(u_src, v_uv - vec2(o, 0.0)).rgb; n += 2.0;
    }
    out_color = vec4(mix(c, acc / n, w), 1.0);
}
