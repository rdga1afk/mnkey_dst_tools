// Ground live pass, decode (ours glue around the original): the original decodePixel (deferred/gbuffer.hlsl) turns the YCoCg checkerboard albedo target into RGB.
// Two targets for the consumer (the ground resolve has no more free sampler slots): A = albedo rgb + gloss (buf0.a), B = the world normal as stored (buf1.rgb, 0..1) + the distance (buf2.r, 0 = not written).
#include "@ORIG@"
uniform sampler2D gbuf0 : register(s0);
uniform sampler2D gbuf1 : register(s1);
uniform sampler2D gbuf2 : register(s2);
uniform float4 viewport;
void main(float4 fragCoord : SV_Position, out float4 outA : SV_Target0, out float4 outB : SV_Target1)
{
    const float2 uv = fragCoord.xy * viewport.zw;
    outA = float4(decodePixel(gbuf0, uv, viewport, fragCoord.xy - 0.5), tex2D(gbuf0, uv).a);
    outB = float4(tex2D(gbuf1, uv).rgb, tex2D(gbuf2, uv).r);
}
