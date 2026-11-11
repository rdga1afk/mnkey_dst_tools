#version 450 core
// upscale.frag -- resolution_scale: bilinear upscale of the internal-resolution scene colour to the swapchain,
// with an optional cheap contrast-adaptive sharpen (CAS-like, 4 neighbours, per-channel). SDL_GPU SPIR-V sets:
// fragment samplers = set 2, fragment uniform buffers = set 3.
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_scene;
layout(std140, set = 3, binding = 0) uniform UpscaleUBO {
    vec2  u_texel;   // 1 / internal-resolution size
    float u_sharp;   // 0 = plain bilinear, 1 = strongest sharpen
    float _pad;
};

void main() {
    vec3 e = texture(u_scene, v_uv).rgb;
    if (u_sharp <= 0.0) { out_color = vec4(e, 1.0); return; }
    vec3 n  = texture(u_scene, v_uv + vec2(0.0, -u_texel.y)).rgb;
    vec3 s  = texture(u_scene, v_uv + vec2(0.0,  u_texel.y)).rgb;
    vec3 w  = texture(u_scene, v_uv + vec2(-u_texel.x, 0.0)).rgb;
    vec3 ee = texture(u_scene, v_uv + vec2( u_texel.x, 0.0)).rgb;
    vec3 mn = min(min(n, s), min(min(w, ee), e));
    vec3 mx = max(max(n, s), max(max(w, ee), e));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    float peak = -1.0 / mix(8.0, 5.0, u_sharp);
    vec3 wgt = amp * peak;
    out_color = vec4((wgt * (n + s + w + ee) + e) / (1.0 + 4.0 * wgt), 1.0);
}
