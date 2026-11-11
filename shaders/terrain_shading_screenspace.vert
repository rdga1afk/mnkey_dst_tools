#version 460 core
// TERRAIN_CA_REBUILD_PROMPT.md Phase 4 -- Variant A shading-resolve pass.
// Fullscreen triangle, no vertex buffer (same pattern as smaa_edge.vert):
// Draw(3, 1, 0, 0).

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
