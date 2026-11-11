#include "water_live_pass.h"
#include <filesystem>

namespace md {
namespace {

// The original waterVP outputs `oPosition : POSITION`, which glslang does not take as the position output: the original is included under another name and a thin entry
// point with SV_Position calls it. The loose uniforms keep the original's names, so the engine fills the block by those names. Vertex input: float4 position, float4 normal.
const char* kVertGlue = R"(#define waterVP waterVP_orig
#include "@VS@"
#undef waterVP
uniform float4 scale, mapBounds, pCameraPos, pFogParams;
uniform float3 worldOffset, sunDirection;
uniform float4x4 pWorldMatrix, pInverseWorldMatrix, pWorldViewProj;
struct VOut { float4 pos : SV_Position; float4 tex : TEXCOORD0; float4 ref : TEXCOORD1; float3 sun : TEXCOORD2; float3 cam : TEXCOORD3; float4 map : TEXCOORD4; float2 spd : TEXCOORD5; };
VOut main(float4 vertex : TEXCOORD6, float4 normal : TEXCOORD7)
{
    VOut o;
    waterVP_orig(vertex, normal, o.pos, o.tex, o.ref, o.sun, o.cam, o.map, o.spd, scale, mapBounds, pCameraPos, pFogParams, worldOffset, sunDirection, pWorldMatrix, pInverseWorldMatrix, pWorldViewProj);
    return o;
}
)";
// near variant (11 samplers) + BLEND (blendmap s11, blendChannel); the fragment entry is the original itself.
const char* kFragGlue = "#define BLEND\n#define waterFP main\n#include \"@FS@\"\n";

void Replace(std::string& s, const char* key, const std::string& v) { s.replace(s.find(key), std::string(key).size(), v); }

}   // namespace

// Our own glue (no Kenshi text): scene depth -> distance from the camera / farClip, the quantity the original's deferred pass writes into its depth target.
const char* const kWaterDepthVert = "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VO main(uint id : SV_VertexID) { VO o; float2 p = float2((id << 1) & 2, id & 2); o.uv = p; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n";
const char* const kWaterDepthFrag = "uniform float4x4 invVP;\nuniform float4 camFar;\nuniform sampler2D depthTex : register(s0);\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(VO i) : SV_Target { float z = tex2D(depthTex, i.uv).r; float4 w = mul(invVP, float4(i.uv.x * 2.0 - 1.0, i.uv.y * 2.0 - 1.0, z, 1.0));\n"
    "  return float4(length(w.xyz / w.w - camFar.xyz) / camFar.w, 0.0, 0.0, 1.0); }\n";

bool CompileWaterLive(const std::string& kd, WaterLiveShaders& out, std::string& log) {
    const std::string dir = kd + "/data/materials/", orig = dir + "forward/water.hlsl";
    std::string vg = kVertGlue, fg = kFragGlue;
    Replace(vg, "@VS@", orig); Replace(fg, "@FS@", orig);
    std::vector<std::string> deps = { orig };
    for (const char* d : { "common", "deferred", "forward", "post" })
        for (const auto& e : std::filesystem::directory_iterator(dir + d))
            if (e.path().extension() == ".hlsl" && e.path().string() != orig) deps.push_back(e.path().string());
    if (!hlsl::Compile(vg, deps, 'v', out.vs)) { log = "vertex: " + out.vs.log; return false; }
    if (!hlsl::Compile(fg, deps, 'f', out.fs)) { log = "fragment: " + out.fs.log; return false; }
    if (!hlsl::GlobalBlockMembers(out.vs.spv, out.vmem, out.vsz)) { log = "vertex: no $Global block"; return false; }
    if (!hlsl::GlobalBlockMembers(out.fs.spv, out.fmem, out.fsz)) { log = "fragment: no $Global block"; return false; }
    return true;
}

}   // namespace md
