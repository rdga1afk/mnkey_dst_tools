// #142 phase 1b: the terrain G-buffer colour target is R16G16B16A16_UINT (8 B/px, was RGBA32F 16 B/px). Pure integer packing, no float bit reinterpretation:
//   c0 = fract(x / 32 m) in 16 bits    c1 = fract(z / 32 m) in 16 bits         (0.49 mm)
//   c2 = fract(y / 32 m) in 15 bits | skirt << 15                               (1 mm)
//   c3 = full-sphere octahedral normal, 8 + 8 bits (feature meshes have N.y < 0: arch undersides, overhangs; tests/test_gbuf_normal.cpp mirrors this maths)
// The whole metres of the position are not stored: the resolve rebuilds the position from the depth (+ inv VP) and snaps it to the stored fraction -- exact
// whenever that reconstruction is within +-16 m (the depth reconstruction is within 8-16 m at 3 km, a few cm close up), so there is neither the position flicker of the
// plain depth reconstruction (S7) nor the quantisation of a fixed-point position.
#define TS_GB_PERIOD   256.0   // NOW-0 14.11: was 32 m; with 2000 m as the snap limit the far ground got the unsnapped depth reconstruction (8-16 m error), so the biome border read from it slid
#define TS_GB_PERIOD_Y 256.0

uvec4 TS_GbufEncode(vec3 pos, vec3 N, bool skirt)
{
    vec3 f = pos / vec3(TS_GB_PERIOD, TS_GB_PERIOD_Y, TS_GB_PERIOD);
    f = f - floor(f);
    uint qx = uint(min(floor(f.x * 65536.0 + 0.5), 65535.0));
    uint qz = uint(min(floor(f.z * 65536.0 + 0.5), 65535.0));
    uint qy = uint(min(floor(f.y * 32768.0 + 0.5), 32767.0));
    vec2 o = N.xz / (abs(N.x) + abs(N.y) + abs(N.z));
    if (N.y < 0.0) o = (1.0 - abs(o.yx)) * vec2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);
    uvec2 oq = uvec2(floor(clamp(o * 0.5 + 0.5, 0.0, 1.0) * 255.0 + 0.5));
    return uvec4(qx, qz, qy | (skirt ? 0x8000u : 0u), oq.x | (oq.y << 8));
}

// posApprox: world position from the depth reconstruction (any error below TS_GB_PERIOD / 2 in XZ and TS_GB_PERIOD_Y / 2 in Y).
// Beyond TS_GB_SNAP_DIST the depth error (standard D32F, near 0.5 m: 8-16 m at 3 km) can exceed that, and the ground there is on coarse mips:
// the position is then the depth reconstruction itself, not snapped (a wrong snap would jump by a whole period).
#define TS_GB_SNAP_DIST 100000.0
void TS_GbufDecode(uvec4 g, vec3 posApprox, float distApprox, out vec3 pos, out vec3 N, out bool skirt)
{
    vec3 f = vec3(float(g.x) * (1.0 / 65536.0), float(g.z & 0x7FFFu) * (1.0 / 32768.0), float(g.y) * (1.0 / 65536.0));
    pos = (distApprox > TS_GB_SNAP_DIST) ? posApprox
        : (floor(posApprox / vec3(TS_GB_PERIOD, TS_GB_PERIOD_Y, TS_GB_PERIOD) - f + 0.5) + f) * vec3(TS_GB_PERIOD, TS_GB_PERIOD_Y, TS_GB_PERIOD);
    skirt = (g.z & 0x8000u) != 0u;
    vec2 o = vec2(float(g.w & 255u), float((g.w >> 8) & 255u)) * (2.0 / 255.0) - 1.0;
    N = vec3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
    if (N.y < 0.0) N.xz = (1.0 - abs(o.yx)) * vec2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);
    N = normalize(N);
}
