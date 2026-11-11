#version 460 core
// 2026-09-17 (owner decision, editor 3D World tab): draws only each tile's
// OUTER RECTANGLE (4 corners, 8 vertices/instance, LINELIST topology) --
// NOT the full 16x16 internal grid terrain_quadtree_batched.vert/
// TerrainQuadtreeRenderer::DrawNodeWireframe draw. That dense per-quad
// wireframe was both visually "огидно" (dense mesh covering the whole
// screen, indistinguishable from solid fill at aerial altitude where line
// density exceeds screen resolution) and measured live at 2 FPS interactive
// (down from 60) -- LINE-mode rasterization of ~512 triangle edges per
// visible tile, times dozens of tiles, is expensive on this hardware.
// A tile-boundary-only outline is ~64x fewer line segments per tile (8 vs
// 512) and reads as a clean grid of tile boundaries instead of a wireframe
// mesh soup.
//
// Same nodeDataTex/heightTex/normalTex/TerrainBatchUBO binding contract as
// terrain_quadtree_batched.vert (reuses its own instance data upload +
// per-frame UBO push, see TerrainQuadtreeRenderer::UploadNodeData/
// BeginBatchedWireframe) -- only the vertex generation differs.

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
layout(location=4) out float vIsSkirt; // always 0.0 -- same contract as the other terrain vert shaders

const int kNodeDataTexWidth = 256; // must match TerrainQuadtreeRenderer::kNodeDataTexWidth
const int kPatchQuads = 16;        // node size in texelSize units, matches kGridSize-1 elsewhere

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
    float nodeSize     = texelSize * float(kPatchQuads);

    float heightMin    = u.height_range.x;
    float heightMax    = u.height_range.y;
    float worldExtent  = u.height_range.z;
    float resTexels    = u.height_range.w;
    float heightRangeM = heightMax - heightMin;

    // 8 vertices/instance = 4 edges of the rectangle (LINELIST):
    // corner = (vid/2 + vid%2) % 4, corners in order (0,0)->(1,0)->(1,1)->(0,1).
    int vid = gl_VertexIndex;
    int corner = ((vid / 2) + (vid % 2)) % 4;
    vec2 cornerUV = vec2(
        (corner == 1 || corner == 2) ? 1.0 : 0.0,
        (corner == 2 || corner == 3) ? 1.0 : 0.0);

    vec2 worldXZ = originWorld + cornerUV * nodeSize;

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
