#version 450 core
// upscale_light.frag -- "Sharpening while moving = Light" (#207, owner 31.10): bilinear upscale + a cheap unsharp mask.
// Two diagonal half-texel bilinear taps = a 4x4-texel blur; out = e + k (e - blur). No per-pixel adaptive weight, no sqrt / divide.
// ~1.3 ms against ~3 ms of upscale.frag (5 taps + adaptive) on the HD 520 at 0.75. SDL_GPU SPIR-V sets: fragment samplers = set 2, uniform buffers = set 3.
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_scene;
layout(std140, set = 3, binding = 0) uniform UpscaleUBO {
    vec2  u_texel;   // 1 / internal-resolution size
    float u_sharp;   // the same strength as upscale.frag (0.3 default)
    float _pad;
};

void main() {
    vec3 e  = texture(u_scene, v_uv).rgb;
    vec3 bl = 0.5 * (texture(u_scene, v_uv + 0.5 * u_texel).rgb + texture(u_scene, v_uv - 0.5 * u_texel).rgb);
    out_color = vec4(clamp(e + (0.35 * u_sharp / 0.3) * (e - bl), 0.0, 1.0), 1.0);
}
