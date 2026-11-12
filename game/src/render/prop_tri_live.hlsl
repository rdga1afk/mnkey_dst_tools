// Release glue for the triplanar map features (1b, #207): the material comes from the install's deferred/triplanar.hlsl (sampleTriplanar /
// sampleTriplanarNormal, included at run time from KENSHI_DIR), the interface and the light are those of shaders/prop_tri.frag (PropRenderer:
// set 2 samplers 0 diffuse / 1 normal, set 3 PropFrag { sun_dir_str, ambient }, inputs of prop.vert at locations 0..3).
#include "@TRI@"

uniform float4 md_sun_dir_str;
uniform float4 md_ambient;
uniform sampler2D md_diffuse : register(s0);
uniform sampler2D md_normal : register(s1);

float4 main(float3 vNormal : TEXCOORD0, float2 vUV : TEXCOORD1, float vLayer : TEXCOORD2, float3 vWorld : TEXCOORD3) : SV_Target0
{
    float3 N = normalize(vNormal);
    const float tile = (vLayer - 4.0) * 1000.0;                                        // FeatureScatter: layer = 4 + tile / 1000
    const float3 coord = float3(vWorld.x - 14745.6, vWorld.y, vWorld.z - 14745.6) * (10.0 / 5000.0 * tile);   // triplanar_vs oTexCoord, metres -> units, features.dat origin
    float3 b = max(0.0, pow(saturate(abs(N) - 0.2) * 7.0, 2.0));                     // triplanar_vs oBlend
    b /= max(b.x + b.y + b.z, 1e-4);
    const float3 albedo = sampleTriplanar(md_diffuse, coord, b).rgb;
    N = normalize(N + sampleTriplanarNormal(md_normal, coord, N, b));
    const float ndl = max(dot(N, md_sun_dir_str.xyz), 0.0);
    const float3 amb = max(md_ambient.xyz, 0.30);
    return float4(albedo * (amb + ndl * md_sun_dir_str.w), 1.0);
}
