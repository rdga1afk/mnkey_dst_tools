// Ground live pass, classification step 2 (ours): marks the pixels whose group slot has bit gb_bit set (colour writes off, the stencil write mask of the pipeline is that bit).
Texture2D<uint> slotTex : register(s0);
uniform float4 gb_bit;   // x: bit index, y: reuse phase 0..7 (-1: every pixel)
void main(float4 fragCoord : SV_Position)
{
    if (gb_bit.y >= 0.0) { uint2 q = uint2(fragCoord.xy) >> 1u; if (((q.x & 3u) | ((q.y & 1u) << 2u)) != uint(gb_bit.y)) discard; }   // one quad in eight
    if (((slotTex.Load(int3(int2(fragCoord.xy), 0)) >> uint(gb_bit.x)) & 1u) == 0u) discard;
}
