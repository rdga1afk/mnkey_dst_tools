#version 450 core
// upscale.vert -- full-screen triangle, no vertex buffer (resolution_scale upscale pass).
layout(location = 0) out vec2 v_uv;
void main() {
    float x = float((gl_VertexIndex & 1) << 2) - 1.0;
    float y = float((gl_VertexIndex >> 1) << 2) - 1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
    v_uv = vec2(x, -y) * 0.5 + 0.5; // Vulkan: y=0 is the top of the texture
}
