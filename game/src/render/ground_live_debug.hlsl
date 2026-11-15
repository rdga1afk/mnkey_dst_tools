// Ground live pass, debug view (ours, md.set_debug_view): the terrain geometry stage replaces the slot of each pixel by a colour code 1..7 (0 = no terrain); this paints the albedo / lit colour from it.
// (code 1..7 as set by the vertex stage) 1 red, 2 orange, 3 yellow, 4 green, 5 cyan, 6 blue, 7 magenta. Views: 1 LOD level 1.8 m * 2^(code - 1) (6 = coarser), 2 node hash, 3 skirt (magenta; the rest green), 4 vertex source (1 fine window, 2 world lattice, 3 bilinear).
Texture2D<uint> slotTex : register(s0);
float4 main(float4 fragCoord : SV_Position) : SV_Target0
{
    const uint code = slotTex.Load(int3(int2(fragCoord.xy), 0)) & 7u;
    if (code == 0u) discard;
    float3 c = float3(1, 0.1, 0.1);
    if (code == 2u) c = float3(1, 0.55, 0.05); else if (code == 3u) c = float3(1, 1, 0.1); else if (code == 4u) c = float3(0.1, 0.9, 0.1);
    else if (code == 5u) c = float3(0.1, 0.9, 0.9); else if (code == 6u) c = float3(0.15, 0.3, 1); else if (code == 7u) c = float3(1, 0, 1);
    return float4(c, 1.0);
}
