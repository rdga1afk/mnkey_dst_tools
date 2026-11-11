#version 460 core
// water.vert — world-space sea surface at Y=0.2m (G-3).
// 6 vertices (2 tris = 1 quad) generated from gl_VertexIndex.
// Centered on camera XZ — depth test against terrain handles occlusion.
#ifdef SPIRV_COMPILE

layout(std140, set=1, binding=0) uniform WaterVU {
    mat4  vp;
    float cam_x, cam_z;
    float world_sz;  // half-size of water quad in metres
    float time;
    float water_y;   // #173: 0.2 for the old water, 1.495 (Kenshi raw 100) for md.set_kenshi_water
    float _pad0, _pad1, _pad2;
} u;

layout(location=0) out vec2 vWorldXZ;  // world XZ for UV + specular
layout(location=1) out float vFade;    // edge fade
// cn spans [-1,1] over exactly the same square the CPU-side water mask
// grid covers (cam_xz +/- world_sz) -- remap to [0,1] so the fragment
// stage can index the mask with no extra UBO fields needed (task #158i-4/9).
layout(location=2) out vec2 vMaskUV;

const int  IDX[6]     = int[6](0,1,2, 0,2,3);
const vec2 CORNERS[4] = vec2[4](
    vec2(-1.0,-1.0), vec2(1.0,-1.0), vec2(1.0,1.0), vec2(-1.0,1.0)
);

void main() {
    vec2 cn  = CORNERS[IDX[gl_VertexIndex]];
    vec2 xz  = vec2(u.cam_x, u.cam_z) + cn * u.world_sz;
    gl_Position = u.vp * vec4(xz.x, u.water_y, xz.y, 1.0);
    vWorldXZ = xz;
    vFade    = 1.0 - smoothstep(0.75, 1.0, length(cn));
    vMaskUV  = cn * 0.5 + 0.5;
}
#else
out vec2 vWorldXZ;
out float vFade;
out vec2 vMaskUV;
void main() { gl_Position = vec4(0); vWorldXZ = vec2(0); vFade = 1.0; vMaskUV = vec2(0); }
#endif
