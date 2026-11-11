#version 460 core
// Final terrain architecture (2026-08-18, "Ogre-quadtree (geomorph+skirts)",
// serene-pondering-teapot.md) -- production vertex shader for
// TerrainQuadtreeRenderer. Vertex-buffer-less (layout.count=0, matches the
// already-proven Phase 1 spike technique) but INDEXED -- gl_VertexIndex
// comes from a real, SHARED index buffer (terrain_quadtree_mesh.h),
// restoring post-transform-cache reuse this session's Phase 2 measurement
// proved a non-indexed draw loses (1.48-2.59x regression, confirmed root
// cause: 6 unshared vertex indices/quad vs an indexed grid's shared
// corners). PTC keys off INDEX VALUES, not vertex-attribute-buffer
// presence, so this keeps the cache benefit with zero bound buffers.
//
// 2026-08-24: #398 (TerrainHeightClipmap, per-depth toroidal cache) REVERTED
// -- a real, reproducible geometry-corruption bug (confirmed live: wrong
// world-Y baked into the mesh, worst on wide/distant views, present even at
// the game's own default spawn point on a fresh launch) traced to this
// clipmap's fill/recenter path. Root cause not pinned to an exact line
// (needs a RenderDoc capture this session couldn't do), so reverted to the
// previously-proven-correct direct binding of TerrainWorldHeightmap's
// world-wide height+normal textures instead of chasing more speculative
// shader edits. Costs ~128MB extra VTF working set back; #398 can be
// re-attempted later with real GPU-capture tooling.
//
// Geomorph and skirts REMOVED (2026-09-13, БОРГ-TERRAIN-1, confirmed dead
// by construction): flat fixed-depth tiling (TERRAIN_FLAT_LOD_PLAN.md,
// 2026-09-04/05) gives every visible node the SAME depth, so LOD
// mismatches at shared edges -- the whole reason skirts/geomorph existed
// -- cannot occur anymore. terrain_quadtree_mesh.cpp's shared IBO has
// carried ONLY the regular 17x17 grid (no skirt indices) since that
// transition, and CPU call sites have explicitly zeroed morph/skirt_depth
// ever since (terrain_quadtree_renderer.cpp's own "harmless no-ops"
// comment) -- gl_VertexIndex here is provably always < kRegularVertexCount
// and morph is provably always 0. This shader used to pay for a second
// VTF height+normal sample and a mix() per vertex for a value that was
// always discarded; both are gone now, not just the source lines.

layout(set=0, binding=0) uniform sampler2D heightTex;
layout(set=0, binding=1) uniform sampler2D normalTex;

layout(std140, set=1, binding=0) uniform TerrainQuadtreeUBO {
    mat4  vp;
    vec4  origin_size_texel_morph; // x=origin_x, y=origin_z, z=texelSize(fine), w=morph
    vec4  height_range;            // x=heightMin, y=heightMax, z=worldExtent, w=resTexels
    vec4  cam_pos_skirt;           // xyz=camera world position, w=skirtDepth
} u;

layout(location=0) out vec3  vNormal;
layout(location=1) out vec2  vWorldPos;
layout(location=2) out float vWorldY;
layout(location=3) out float vDist;
// Always 0.0 now -- no skirt geometry exists to set it (see this file's
// top doc comment, БОРГ-TERRAIN-1). Kept as a real output, not deleted:
// terrain_quadtree_forward.frag/terrain_gbuffer_mini.frag/terrain_
// wireframe.frag all still declare this varying as an input and degrade
// correctly (always take the non-skirt branch) without needing their own
// edit -- removing the output here would force touching all three for
// zero behavior change.
layout(location=4) out float vIsSkirt;

const int kGridSize = 17;

float SampleHeightFrac(vec2 worldXZ, float worldExtent, float resTexels) {
    vec2 worldUV = worldXZ / worldExtent;
    vec2 texUV = (worldUV * (resTexels - 1.0) + 0.5) / resTexels;
    return textureLod(heightTex, texUV, 0.0).r;
}

// 2026-08-29 (task #556): briefly fetched normalTex at an explicit LOD
// derived from this vertex's mesh spacing, so a coarse/distant node would
// sample a pre-filtered normal instead of a raw native texel. Reverted
// 2026-09-23 (engine/src/render/terrain_world_heightmap.cpp's normalTex
// doc comment has the full story): kFlatLodDepth's fixed-depth quadtree
// (БОРГ-TERRAIN-1, later than #556) makes every node's texelSize provably
// equal to this texture's native texel size, so that LOD was always 0 --
// normalTex is back to a single level, always sampled at lod 0 here too.
vec3 SampleNormal(vec2 worldXZ, float worldExtent, float resTexels) {
    vec2 worldUV = worldXZ / worldExtent;
    vec2 texUV = (worldUV * (resTexels - 1.0) + 0.5) / resTexels;
    vec2 nxz = textureLod(normalTex, texUV, 0.0).rg;
    float ny = sqrt(max(0.0, 1.0 - nxz.x * nxz.x - nxz.y * nxz.y));
    return normalize(vec3(nxz.x, ny, nxz.y));
}

void main() {
    vec2  originWorld  = u.origin_size_texel_morph.xy;
    float texelSize     = u.origin_size_texel_morph.z;
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
    vDist     = length(u.cam_pos_skirt.xyz - posWS);
    gl_Position = u.vp * vec4(posWS, 1.0);
}
