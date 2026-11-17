#version 450 core
// Grass at its own scale (dynres 0.7): the minimum (nearest) of the 2x2 full-resolution scene depths under each half-resolution texel. The half-resolution grass pass depth-tests against this,
// so a blade never shows in front of a nearer object at a silhouette. SDL_GPU SPIR-V sets: fragment samplers = set 2.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D u_depth;
void main() {
    vec4 g = textureGather(u_depth, v_uv, 0);
    o = vec4(min(min(g.x, g.y), min(g.z, g.w)), 0.0, 0.0, 0.0);
}
