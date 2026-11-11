#version 460 core
// B3 (Granite terrain finding, RENDER_VS_GRANITE_DEEPSEEK_RESEARCH.md):
// instanced variant of terrain_quadtree.vert -- per-node data (origin,
// texelSize, morph, skirtDepth) comes from nodeDataTex (2 texels/node,
// indexed by gl_InstanceIndex) instead of a per-draw push UBO, so the
// whole visible-node set draws in ONE instanced draw_indexed call instead
// of N DrawNode calls (TerrainQuadtreeRenderer::DrawBatched).
//
// Deliberately a TEXTURE, not an SSBO -- no shader in this codebase
// combines vert_samplers>0 with vert_storage_bufs>0 (GpuPipeline::Create
// has a hard guard rejecting that exact combination when frag_samplers>0
// too, gpu_hal_pipeline.cpp -- this project's own documented history of
// Intel Gen9 ANV vertex-stage resource hazards). This vertex shader
// already has 2 samplers (heightTex, normalTex); a 3rd sampler reuses an
// already-proven-safe resource category instead of introducing a new,
// untested-on-this-hardware combination.
//
// Kept as a SEPARATE shader from terrain_quadtree.vert (not a shared/
// parameterized one) specifically so the forward-shading pipeline
// (terrain_quadtree_forward.frag, which DOES have real frag_samplers=5)
// is never at risk of inheriting any vertex-stage change made here.
// Geometry otherwise identical to terrain_quadtree.vert -- see that
// file's doc comment for the full rationale, including the 2026-09-13
// БОРГ-TERRAIN-1 geomorph/skirt removal this file mirrors (nodeA.w/
// nodeB.x were always fed 0 from the CPU-side node-data-texture fill,
// same "harmless no-ops" pattern as the per-draw UBO path).

layout(set=0, binding=0) uniform sampler2D heightTex;
layout(set=0, binding=1) uniform sampler2D normalTex;
layout(set=0, binding=2) uniform sampler2D nodeDataTex; // kNodeDataTexWidth wide, 2 texels/node

layout(std140, set=1, binding=0) uniform TerrainBatchUBO {
    mat4  vp;
    vec4  height_range; // x=heightMin, y=heightMax, z=worldExtent, w=resTexels
    vec4  cam_pos_pad;  // xyz=camera world position, w=unused
} u;

layout(location=0) out vec3  vNormal;
layout(location=1) out vec2  vWorldPos;
layout(location=2) out float vWorldY;
layout(location=3) out float vDist;
// Always 0.0 -- see terrain_quadtree.vert's matching doc comment
// (БОРГ-TERRAIN-1). Kept as a real output for the same reason: downstream
// frag shaders degrade correctly without their own edit.
layout(location=4) out float vIsSkirt;

const int kGridSize = 17;
const int kNodeDataTexWidth = 256; // must match TerrainQuadtreeRenderer::kNodeDataTexWidth

float SampleHeightFrac(vec2 worldXZ, float worldExtent, float resTexels) {
    vec2 worldUV = worldXZ / worldExtent;
    vec2 texUV = (worldUV * (resTexels - 1.0) + 0.5) / resTexels;
    return textureLod(heightTex, texUV, 0.0).r;
}

vec3 SampleNormal(vec2 worldXZ, float worldExtent, float resTexels) {
    vec2 worldUV = worldXZ / worldExtent;
    vec2 texUV = (worldUV * (resTexels - 1.0) + 0.5) / resTexels;
    vec2 nxz = textureLod(normalTex, texUV, 0.0).rg;
    float ny = sqrt(max(0.0, 1.0 - nxz.x * nxz.x - nxz.y * nxz.y));
    return normalize(vec3(nxz.x, ny, nxz.y));
}

vec4 FetchNodeTexel(int texelIndex) {
    ivec2 coord = ivec2(texelIndex % kNodeDataTexWidth, texelIndex / kNodeDataTexWidth);
    return texelFetch(nodeDataTex, coord, 0);
}

void main() {
    vec4 nodeA = FetchNodeTexel(gl_InstanceIndex * 2); // origin_x,origin_z,texelSize,(unused)

    vec2  originWorld = nodeA.xy;
    float texelSize    = nodeA.z;

    float heightMin    = u.height_range.x;
    float heightMax    = u.height_range.y;
    float worldExtent  = u.height_range.z;
    float resTexels    = u.height_range.w;
    float heightRangeM = heightMax - heightMin;

    int col = gl_VertexIndex % kGridSize;
    int row = gl_VertexIndex / kGridSize;

    vec2 worldXZ = originWorld + vec2(float(col), float(row)) * texelSize;

    float hFrac = SampleHeightFrac(worldXZ, worldExtent, resTexels);
    float worldY = heightMin + hFrac * heightRangeM;

    vec3 N = SampleNormal(worldXZ, worldExtent, resTexels);

    vec3 posWS = vec3(worldXZ.x, worldY, worldXZ.y);
    vNormal   = N;
    vWorldPos = worldXZ;
    vWorldY   = worldY;
    vIsSkirt  = 0.0;
    vDist     = length(u.cam_pos_pad.xyz - posWS);
    gl_Position = u.vp * vec4(posWS, 1.0);
}
