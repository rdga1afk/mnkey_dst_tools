#version 450 core
// Grass at its own scale (dynres 0.7): the half-resolution grass (premultiplied by coverage: the target is cleared to zero and only lit blade pixels are written, alpha 1) is upsampled
// bilinearly over the full-resolution ground copy. Writes every pixel of the ground target. SDL_GPU SPIR-V sets: fragment samplers = set 2.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D u_base;
layout(set = 2, binding = 1) uniform sampler2D u_grass;
void main() {
    vec4 b = texture(u_base, v_uv);
    vec4 g = texture(u_grass, v_uv);   // premultiplied by coverage, bilinear
    // the full-resolution blades are hard cut-outs (alpha test): sharpen the bilinear coverage back towards an edge and take the colour unpremultiplied, so a silhouette is neither soft nor dark
    float a = clamp((g.a - 0.5) * 2.5 + 0.5, 0.0, 1.0);
    vec3 c = g.rgb / max(g.a, 1e-3);
    o = vec4(mix(b.rgb, c, a), max(b.a, a));
}
