#version 450 core
// ground_composite.frag -- #163 5b-2a: the ground colour (written by the resolve in its own pass, alpha 1 where it wrote a pixel) copied into the scene colour of the main pass.
// Pixels the resolve did not write (sky, behind an NPC: it depth-tested against the NPC depth) have alpha 0 and are left alone. No depth test: the depth buffer already holds the ground depth.
// SDL_GPU SPIR-V sets: fragment samplers = set 2.
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_ground;
void main() {
    vec4 g = texelFetch(u_ground, ivec2(gl_FragCoord.xy), 0);
    if (g.a < 0.5) discard;
    out_color = vec4(g.rgb, 1.0);
}
