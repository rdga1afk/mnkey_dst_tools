#version 460 core
// TEMP DEBUG TOOL (2026-09-04, CLAUDE_CONSTITUTION.md sec 7.7): solid-colour
// fragment stage paired with terrain_quadtree.vert + GpuRasterState::wireframe
// (SDL_GPU_FILLMODE_LINE) -- draws the REAL terrain triangle edges (same
// vertex transform as the live gbuffer/forward draws, not a reconstruction)
// so mesh defects (broken vertices, bad skirts, stitch_edge_mask errors) are
// visible directly, independent of shading/texture/LOD-bias questions.
// Same vertex input locations as terrain_gbuffer_mini.frag (shared vert
// shader) but unused -- fragment stage doesn't have to consume every
// vertex-stage output for pipeline linkage to be valid.
layout(location=0) in vec3  vNormal;
layout(location=1) in vec2  vWorldPos;
layout(location=2) in float vWorldY;
layout(location=3) in float vDist;
layout(location=4) in float vIsSkirt;

layout(location=0) out vec4 outColor;

void main() {
    outColor = vIsSkirt > 0.5 ? vec4(1.0, 0.15, 0.15, 1.0)   // skirts: red
                               : vec4(1.0, 1.0, 0.1, 1.0);    // main grid: yellow
}
