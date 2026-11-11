// Ground live main light (ours, glue around the original deferred/deferred.hlsl main_fs): a full-screen triangle that gives the original fragment program its two varyings.
// texCoord = the pixel's position in the G-buffer (row 0 at the top: SDL_GPU clip y is up), ray = the world direction of the pixel from the camera (the pass uses an identity inverseView,
// so the original's viewPos = normalize(ray) * distance is already the camera-relative world position).
uniform float4x4 vl_invVP;
uniform float4 vl_cam;
void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 texCoord : TEXCOORD0, out float3 ray : TEXCOORD1)
{
    const float2 p = float2((id << 1) & 2, id & 2);
    pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    texCoord = float2(p.x, 1.0 - p.y);
    const float4 w = mul(vl_invVP, float4(pos.xy, 1.0, 1.0));
    ray = w.xyz / w.w - vl_cam.xyz;
}
