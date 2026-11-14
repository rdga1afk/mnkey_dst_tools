// Ground live pass, HLSL side of our terrain G-buffer (shaders/terrain_gbuf_pack.glsl: the same packing, the same maths): world position and normal of a pixel.
// posApprox: the world position from the depth reconstruction; distApprox its distance to the camera.
#define TS_GB_PERIOD 256.0
#define TS_GB_SNAP_DIST 100000.0

void GbufDecode(uint4 g, float3 posApprox, float distApprox, out float3 pos, out float3 N, out bool skirt)
{
    float3 f = float3(float(g.x) * (1.0 / 65536.0), float(g.z & 0x7FFFu) * (1.0 / 32768.0), float(g.y) * (1.0 / 65536.0));
    pos = (distApprox > TS_GB_SNAP_DIST) ? posApprox : (floor(posApprox / TS_GB_PERIOD - f + 0.5) + f) * TS_GB_PERIOD;
    skirt = (g.z & 0x8000u) != 0u;
    float2 o = float2(float(g.w & 255u), float((g.w >> 8) & 255u)) * (2.0 / 255.0) - 1.0;
    N = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
    if (N.y < 0.0) N.xz = (1.0 - abs(o.yx)) * float2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);
    N = normalize(N);
}
