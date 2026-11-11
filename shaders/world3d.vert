#version 450 core
// world3d.vert — simple 3D world geometry (ground/walls/water).
// Vertex format: vec3 position only (stride = 12 bytes).
// World-Y is passed to fragment for height-based coloring.

layout(location = 0) in vec3 a_pos;

layout(location = 0) out float out_world_y;

// set=1 binding=0: MVP matrix (64 bytes)
layout(set = 1, binding = 0) uniform WorldUBO {
    mat4 u_mvp;
};

void main() {
    out_world_y  = a_pos.y;
    gl_Position  = u_mvp * vec4(a_pos, 1.0);
}
