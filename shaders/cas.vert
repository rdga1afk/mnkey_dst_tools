#version 450 core
// cas.vert — full-screen triangle, no vertex buffer needed.
// gl_VertexIndex 0,1,2 → three vertices that cover the entire screen.

layout(location = 0) out vec2 v_uv;

void main() {
    // Triangle vertices: (-1,-1), (3,-1), (-1,3)
    float x = float((gl_VertexIndex & 1) << 2) - 1.0;
    float y = float((gl_VertexIndex >> 1) << 2) - 1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
    // UV: (0,0)=top-left, (1,1)=bottom-right (Vulkan y=0 is top of texture).
    v_uv = vec2(x, -y) * 0.5 + 0.5;
}
