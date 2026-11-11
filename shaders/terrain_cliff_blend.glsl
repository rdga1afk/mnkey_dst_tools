// terrain_cliff_blend.glsl -- TS_ZoneLayer and the cliff constants the Kenshi ground path (terrain_kenshi_blend.glsl) reads. The zone-tent cliff blend
// that used to live here was removed in #185 K4c together with the forward terrain shading path. Whichever file includes this (via
// terrain_shading_common.glsl) must have declared zoneGroundLayersTex.

// Fetches one uint32 slot (0-26, slot 27 unused padding) for a zone from
// zoneGroundLayersTex -- callers must declare that usampler2D binding
// (R32G32B32A32_UINT, 448x64, 7 texels/zone x 4 channels = 28 slots, 27
// used) before this #include, same textual-resolution convention as
// tex_colour/tex_ground/etc above -- this function resolves against
// whichever the including file declared. If-chain instead of dynamic
// uvec4 component indexing -- this codebase has hit enough Mesa/driver-
// specific GLSL quirks (hardware checklist) to prefer the portable form
// here. Packing must match TerrainRenderer::UploadZoneGroundLayers exactly.
uint TS_ZoneLayer(int zone_idx, int slot) {
    int zx = zone_idx % 64;
    int zy = zone_idx / 64;
    int t  = slot / 4;
    int c  = slot % 4;
    uvec4 texel = texelFetch(zoneGroundLayersTex, ivec2(zx * 7 + t, zy), 0);
    if (c == 0) return texel.x;
    if (c == 1) return texel.y;
    if (c == 2) return texel.z;
    return texel.w;
}

// 2026-08-24, "falls through textures" user report: TS_CLIFF_MIN=0.50
// requires steepness>=0.50 (N.y<=0.50, slope>=60deg) before ANY cliff
// (Y-projected triplanar) texture blends in. Live-diagnosed at a REAL
// reported bug location (11115.5,9676.2): height data there is smooth
// (confirmed via a direct height-fraction visualization -- no geometry
// corruption), but the slope is ~54deg (steepness~=0.41) -- BELOW the old
// 0.50 threshold, so it got ZERO cliff contribution, pure flat XZ-planar
// ground/overlay UV stretched over a real steep incline -- the dense
// vertical-stripe look the user's screenshots show. Lowered so the cliff
// blend ramp starts around ~26deg and reaches full strength by ~41deg,
// covering realistic walkable-adjacent steep terrain instead of only
// near-vertical (>=60deg) faces.

const float TS_CLIFF_MIN = 0.25, TS_CLIFF_BLEND = 0.15;
const float TS_CHUNK_SIZE_M = 460.8;
const float TS_CLIFF_UV_SCALE = 1.0 / 5000.0;
