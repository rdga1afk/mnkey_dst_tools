// Ground live masked pass glue (ground step 2; ours, no formula of the original). The original vertex program turns a world position and normal into every input of the
// original fragment entry; here both run per pixel in one fragment shader. gw_position / gw_normal come from the G-buffer decode (filled by the caller).
#define TEXTURED
#define main_vs ground_orig_vs
#include "@VS@"
#undef main_vs
#ifdef GW_NOWET   // diagnostic only: the original makeWet (common/wet.hlsl) stays defined, the calls of terrainfp4 go to a no-op
#include "@WET@"
#define makeWet makeWetOff
void makeWetOff(inout float4 f, float a, float b, float c = -10.0, float d = 2.0) {}
#endif
#define main_fs ground_orig_fs
#include "@FS@"
#undef main_fs

#ifdef GW_ENGINE   // the engine pass: world position and normal come from our terrain G-buffer (game/src/render/ground_live_gbuf.hlsl), metres -> the original's centred 0.1 m units
#include "@GBUF@"
Texture2D<float4> gbufDepthTex : register(s12);
Texture2D<uint4> gbufPackedTex : register(s13);
uniform float4x4 gw_invVP;
uniform float4 gw_camWs;      // xyz: camera position in metres (world corner origin)
uniform float4 gw_originM;    // x, y: the original's origin of x and z in our metres (14745.6), w: units per metre (10)
#elif defined(GW_BAKE)   // the live-ground cache bake: a target texel = a ring texel; its surface on the world-heightmap lattice (stride 1) the way our terrain vertices build it
Texture2D<float> gwHeightTex : register(s12);   // the world heightmap, R16_UNORM, point loads
uniform float4 gw_bake0;      // xy: world metres (corner origin) of the centre of target texel (0, 0)
uniform float4 gw_bake2;      // xy: world metres per target pixel along x, zw: along y (a strip along z is baked transposed)
uniform float4 gw_bake1;      // x: height min, y: height max (metres), z: world extent (metres), w: resolution (cells)
float GwCellH(int2 c) {
    c = clamp(c, int2(0, 0), int2((int)gw_bake1.w - 1, (int)gw_bake1.w - 1));
    return gw_bake1.x + gwHeightTex.Load(int3(c, 0)).r * (gw_bake1.y - gw_bake1.x);
}
float3 GwCellN(int2 c, float sp) {   // tools/shaders/terrain_quadtree_batched_lod.vert NormalAt: the six-neighbour ring, cross products summed, negated
    const int2 ring[6] = { int2(0, -1), int2(1, -1), int2(1, 0), int2(0, 1), int2(-1, 1), int2(-1, 0) };
    float h0 = GwCellH(c); float3 r[6];
    for (int k = 0; k < 6; ++k) r[k] = float3(float(ring[k].x) * sp, GwCellH(c + ring[k]) - h0, float(ring[k].y) * sp);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int k = 0; k < 6; ++k) sum += cross(r[k], r[(k + 1) % 6]);
    return -normalize(sum);
}
#endif
uniform float4 gw_position;   // xyz: world position in the original's units
uniform float4 gw_normal;     // xyz: unit world normal
uniform float4x4 gw_wvp;      // the vertex program's matrices: the position is already in world units, so the world matrix is the identity
uniform float4x4 gw_world;
uniform float4 gw_distortion0, gw_distortion1, gw_biomeData, gw_overlayData;   // from the biome record / the map rectangles (re/re_/skyx/ground_packer.md)
uniform float3 gw_cameraPos, gw_cameraDir;

void main(float4 fragCoord : SV_Position, out GBuffer buffer)
{
#ifdef GW_ENGINE
    int2 gpx = int2(fragCoord.xy);
    float gd = gbufDepthTex.Load(int3(gpx, 0)).r;
    int2 gsz; { uint gw_, gh_, gl_; gbufDepthTex.GetDimensions(0, gw_, gh_, gl_); gsz = int2(gw_, gh_); }
    float2 gndc = float2((float(gpx.x) + 0.5) / float(gsz.x) * 2.0 - 1.0, 1.0 - (float(gpx.y) + 0.5) / float(gsz.y) * 2.0);
    float4 grd = mul(gw_invVP, float4(gndc, gd, 1.0));
    float3 gapprox = grd.xyz / grd.w, gpm, gnm; bool gskirt;
    GbufDecode(gbufPackedTex.Load(int3(gpx, 0)), gapprox, length(gw_camWs.xyz - gapprox), gpm, gnm, gskirt);
    float4 gw_position4 = float4((gpm.x - gw_originM.x) * gw_originM.w, gpm.y * gw_originM.w, (gpm.z - gw_originM.y) * gw_originM.w, 1.0), gw_normal4 = float4(gnm, 0.0);
#elif defined(GW_TEST_GRID)   // numeric oracle (ground_fs_rt, MD_GROUND_LIVE=1): x = slope 0..1, y = case row; position in the original's units, centred
    float gs = (fragCoord.x - 0.5) / 127.0, grow = fragCoord.y - 0.5;
    float gny = 1.0 - gs, gh = sqrt(max(0.0, 1.0 - gny * gny));
    float3 gwn = float3(0.8 * gh, gny, 0.6 * gh);
    float3 gwp = float3(-3000.0 + 31.7 * fragCoord.x + 410.0 * grow, 1000.0 + 4.0 * fragCoord.x, 5000.0 + 17.3 * fragCoord.x + 233.0 * grow);
    float4 gw_position4 = float4(gwp, 1.0), gw_normal4 = float4(gwn, 0.0);
#elif defined(GW_BAKE)
    float2 bxz = gw_bake0.xy + (fragCoord.x - 0.5) * gw_bake2.xy + (fragCoord.y - 0.5) * gw_bake2.zw;
    float bsp = gw_bake1.z / (gw_bake1.w - 1.0);   // lattice spacing (cell i at i * spacing)
    float2 bg = bxz / bsp; int2 bc = int2(floor(bg)); float2 bf = bg - float2(bc);
    float bh; float3 bn;   // the lattice triangles share the (1, 0)-(0, 1) diagonal (the normal ring's (1, -1) / (-1, 1) neighbours)
    if (bf.x + bf.y <= 1.0) {
        float h00 = GwCellH(bc), h10 = GwCellH(bc + int2(1, 0)), h01 = GwCellH(bc + int2(0, 1));
        bh = h00 + bf.x * (h10 - h00) + bf.y * (h01 - h00);
        float3 n00 = GwCellN(bc, bsp), n10 = GwCellN(bc + int2(1, 0), bsp), n01 = GwCellN(bc + int2(0, 1), bsp);
        bn = normalize(n00 + bf.x * (n10 - n00) + bf.y * (n01 - n00));
    } else {
        float h11 = GwCellH(bc + int2(1, 1)), h10 = GwCellH(bc + int2(1, 0)), h01 = GwCellH(bc + int2(0, 1));
        bh = h11 + (1.0 - bf.x) * (h01 - h11) + (1.0 - bf.y) * (h10 - h11);
        float3 n11 = GwCellN(bc + int2(1, 1), bsp), n10 = GwCellN(bc + int2(1, 0), bsp), n01 = GwCellN(bc + int2(0, 1), bsp);
        bn = normalize(n11 + (1.0 - bf.x) * (n01 - n11) + (1.0 - bf.y) * (n10 - n11));
    }
    float4 gw_position4 = float4((bxz.x - 14745.6) * 10.0, bh * 10.0, (bxz.y - 14745.6) * 10.0, 1.0), gw_normal4 = float4(bn, 0.0);
#else
    float4 gw_position4 = gw_position, gw_normal4 = gw_normal;
#endif
    float4 oPos; float3 oNormal; float3 oWorld; float3 oTex0; float4 oTex1; float2 uvblend; float4 oTexV;
    ground_orig_vs(float4(gw_position4.xyz, 1.0), gw_normal4.xyz.yzx, gw_normal4.xyz.yzx, oPos, oNormal, oWorld, oTex0, oTex1, uvblend, oTexV,
        gw_wvp, gw_world, 0.0, gw_distortion0, gw_distortion1, gw_biomeData, gw_overlayData, gw_cameraPos, gw_cameraDir);
    ground_orig_fs(fragCoord, oNormal, oWorld, oTex0, oTex1, uvblend, oTexV, buffer);
}
