// Live-ground cache, hour 4 (ours; docs/research/LIVE_GROUND_CACHE_DESIGN_2026-11-13.md §6): the fetch draw. Runs in the ground live pass after the slot bit planes, before the groups.
// A pixel at distance D..4096 m whose band ring holds it (baked texels, one texel of margin for the bilinear fetch) is written from the ring the way the original program ends
// (the original writeAlbedo / writeMetalness / writeGloss / writeNormal / writeDepth of deferred/gbuffer.hlsl, at this pixel's parity); the pipeline sets stencil bit 5, so the
// group draws (stencil EQUAL slot) skip it. Every other pixel is discarded and keeps the program path.
#define DX11
#include "@ORIG@"
#include "@GBUF@"
Texture2D<float4> csDepthTex : register(s0);
Texture2D<uint4> csPackedTex : register(s1);
sampler2D csA0 : register(s2);
sampler2D csB0 : register(s3);
sampler2D csA1 : register(s4);
sampler2D csB1 : register(s5);
sampler2D csA2 : register(s6);
sampler2D csB2 : register(s7);
uniform float4x4 cs_invVP;
uniform float4 cs_cam;       // xyz: camera (metres, world corner origin), w: D (metres)
uniform float4 cs_rect0;     // baked rectangle of ring r in metres: x0, z0, x1, z1
uniform float4 cs_rect1;
uniform float4 cs_rect2;
uniform float4 cs_far;       // x: farClip of the program (0.1 m units), y: the minimum normal y served from the rings

void main(float4 fragCoord : SV_Position, out GBuffer buffer)
{
    int2 px = int2(fragCoord.xy);
    float gd = csDepthTex.Load(int3(px, 0)).r;
    if (gd >= 1.0) discard;
    uint w_, h_, l_; csDepthTex.GetDimensions(0, w_, h_, l_);
    float2 ndc = float2((float(px.x) + 0.5) / float(w_) * 2.0 - 1.0, 1.0 - (float(px.y) + 0.5) / float(h_) * 2.0);
    float4 rd = mul(cs_invVP, float4(ndc, gd, 1.0));
    float3 approx = rd.xyz / rd.w, pm, nm; bool skirt;
    GbufDecode(csPackedTex.Load(int3(px, 0)), approx, length(cs_cam.xyz - approx), pm, nm, skirt);
    float d = length(pm - cs_cam.xyz);
    if (nm.y < cs_far.y) discard;   // steep faces: a top-down ring texel spans the whole wall height (stretched triplanar), they keep the program
    if (d < cs_cam.w || d >= 4096.0) discard;
    float t = d < 256.0 ? 0.25 : (d < 1024.0 ? 1.0 : 4.0);
    float4 rc = d < 256.0 ? cs_rect0 : (d < 1024.0 ? cs_rect1 : cs_rect2);
    if (pm.x < rc.x + t || pm.z < rc.y + t || pm.x > rc.z - t || pm.z > rc.w - t) discard;
    float4 uv = float4(pm.xz / (t * 2048.0), 0.0, 0.0);   // texel (tx, tz) lives at (tx mod 2048, tz mod 2048): the repeat sampler wraps
    float4 a, b;
    if (d < 256.0) { a = tex2Dlod(csA0, uv); b = tex2Dlod(csB0, uv); }
    else if (d < 1024.0) { a = tex2Dlod(csA1, uv); b = tex2Dlod(csB1, uv); }
    else { a = tex2Dlod(csA2, uv); b = tex2Dlod(csB2, uv); }
    INITIALISE_OUTPUT(buffer);
    writeAlbedo(buffer, a.rgb, fragCoord.xy);
    writeMetalness(buffer, 0.0);
    writeGloss(buffer, a.a);
    writeNormal(buffer, normalize(b.rgb * 2.0 - 1.0));
    writeDepth(buffer, d * 10.0 / cs_far.x);
}
