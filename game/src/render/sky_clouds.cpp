#include "sky_clouds.h"
#ifdef MD_HLSL_RUNTIME
#include "kenshi_hlsl.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/platform/window.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/kenshi_sky.h>
#include <monkey_dust/io/fcs_gamedata.h>
#include <monkey_dust/world/weather.h>
#include "stb_image.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

const char* kVsGlue = R"HLSL(
[[vk::binding(0, 1)]] cbuffer UV { float4 g_right; float4 g_up; float4 g_fwd; float4 g_proj; }   // proj: x fov_tan, y aspect
struct VSOut { float4 pos : SV_Position; [[vk::location(0)]] float3 dir : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    float2 t = float2((vid << 1) & 2, vid & 2);
    float2 ndc = t * 2.0 - 1.0;
    o.pos = float4(ndc, 0.9999, 1.0);
    o.dir = normalize(g_fwd.xyz + g_right.xyz * (ndc.x * g_proj.x * g_proj.y) + g_up.xyz * (ndc.y * g_proj.x));
    return o;
}
)HLSL";

const char* kFsGlue = R"HLSL(
#include "@ORIG@"
[[vk::binding(0, 3)]] cbuffer UF {
    float4 g_zenith;    // xyz zenithLight
    float4 g_sunc;      // sunColour
    float4 g_a;         // x uExposure, y uHeight, z uScale, w uTime
    float4 g_b;         // xy uWindDirection, z heightVolume, w volumetricDisplacement
    float4 g_c;         // x distanceAttenuation, y detailAttenuation, z densityMultiplier, w densityOffset
    float4 g_d;         // x darkness
}
[[vk::binding(0, 2)]] sampler2D g_clouds;
[[vk::binding(1, 2)]] sampler2D g_normal;
[[vk::binding(2, 2)]] sampler2D g_tile;
float4 main(float4 pos : SV_Position, [[vk::location(0)]] float3 dir : TEXCOORD0) : SV_Target0
{
    if (dir.y <= 0.002) discard;   // the layer exists above the horizon only
    float4 col;
    main_fp(pos, normalize(dir), col, g_zenith.xyz, float3(0, 0, 0), g_sunc, g_a.x, float3(1, 1, 1), g_a.y, g_a.w, g_a.z, g_b.xy, g_b.z, g_b.w,
            float3(0.55, 0.55, 0.55), g_c.y, g_c.x, g_c.z, g_c.w, g_d.x, g_clouds, g_normal, g_tile);
    return col;
}
)HLSL";

// The weather of the camera's zone drives the layer: coverage w -> density offset and darkness (the sky controller's rule).
struct Weather {
    bool tried = false, ok = false; md::weather::Data data; md::weather::World world;
    double minutes = 0.0; float cam_x = 0.f, cam_z = 0.f; std::chrono::steady_clock::time_point last; bool have_last = false;
} gw;

void InitWeather() {
    gw.tried = true;
    const std::string dir = md::KenshiDir();
    md::fcs::Install inst(dir); md::fcs::LoadOrder order; md::fcs::Error e; md::fcs::GameDataContainer gd;
    if (!inst.LoadOrderFor(order, e) || !gd.Load(order, e)) return;
    int w = 0, h = 0, c = 0;
    unsigned char* areas = stbi_load((dir + "/data/newland/land/areasmap.tga").c_str(), &w, &h, &c, 3);
    if (!areas) return;
    gw.ok = md::weather::LoadData(gd, areas, w, h, gw.data);
    stbi_image_free(areas);
    if (gw.ok) gw.world.Init(&gw.data, 1);
    fprintf(stderr, "[SkyClouds] weather: %zu biome groups, %s\n", gw.data.groups.size(), gw.ok ? "ok" : "no data");
}

struct WeatherOverride { bool on = false; float cloud_offset = 0.f, darkness = 0.f, wetness = 0.f; } gov;

struct State { bool tried = false, ok = false; GpuPipeline pipe; GpuTexture clouds, normal, tile; } g;

void Init() {
    g.tried = true;
    const std::string mats = md::KenshiDir() + "/data/materials";
    std::string fs = kFsGlue; const std::string orig = mats + "/SkyX/SkyX_Clouds.hlsl";
    fs.replace(fs.find("@ORIG@"), 6, orig);
    md::hlsl::Compiled cv, cf;
    if (!md::hlsl::Compile(kVsGlue, {}, 'v', cv)) { fprintf(stderr, "[SkyClouds] vertex glue does not compile:\n%s\n", cv.log.c_str()); return; }
    if (!md::hlsl::Compile(fs, { orig }, 'f', cf)) { fprintf(stderr, "[SkyClouds] SkyX_Clouds.hlsl does not compile:\n%s\n", cf.log.c_str()); return; }
    GpuSamplerDesc sd; sd.min_filter = GpuSamplerDesc::Filter::LINEAR; sd.gen_mipmap = false;
    if (!g.clouds.InitFromFile((mats + "/SkyX/Clouds.dds").c_str(), sd) || !g.normal.InitFromFile((mats + "/SkyX/CloudsNormal.dds").c_str(), sd) || !g.tile.InitFromFile((mats + "/SkyX/CloudsTile.dds").c_str(), sd)) { fprintf(stderr, "[SkyClouds] textures failed\n"); return; }
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/sky_install_clouds.vert"; pd.frag_path = "shaders/sky_install_clouds.frag";
    pd.vert_spv_path = cv.cache_path.c_str(); pd.frag_spv_path = cf.cache_path.c_str();
    pd.layout.count = 0; pd.vert_uniform_bufs = 1; pd.frag_uniform_bufs = 1; pd.frag_samplers = 3;
    pd.has_depth_target = true; pd.raster.depth_test = true; pd.raster.depth_write = false; pd.raster.cull_back = false;
    pd.raster.blend_enable = true;   // SkyX layer: scene_blend alpha_blend
    if (!g.pipe.Create(pd)) { fprintf(stderr, "[SkyClouds] pipeline create failed\n"); return; }
    g.ok = true;
    fprintf(stderr, "[SkyClouds] SkyX_Clouds.hlsl from the install: %s %.1f ms\n", cf.from_cache ? "cache" : "compiled", cf.compile_ms);
}

} // namespace

namespace SkyClouds {
void SetClock(double minutes) { gw.minutes = minutes; }
void SetCameraWorld(float x_m, float z_m) { gw.cam_x = x_m; gw.cam_z = z_m; }
void SetWeatherOverride(bool on, float cloud_offset, float darkness, float wetness) { gov = { on, cloud_offset, darkness, wetness }; }
float OverrideWetness() { return gov.on ? gov.wetness : 0.f; }
void Draw(GpuPassView& pv, const MdCamera& cam, float hours) {
    if (!getenv("MD_SKY_INSTALL")) return;
    if (!g.tried) Init();
    if (!g.ok) return;
    if (!gw.tried) InitWeather();
    float cloud_offset = -0.8f, darkness = 0.f;
    if (gov.on) { cloud_offset = gov.cloud_offset; darkness = gov.darkness; }
    else if (gw.ok) {
        const auto now = std::chrono::steady_clock::now();
        float dt = gw.have_last ? std::chrono::duration<float>(now - gw.last).count() : 0.f;
        gw.last = now; gw.have_last = true; if (dt > 0.1f) dt = 0.1f;
        gw.world.Update(gw.minutes, dt, (gw.cam_x - 14745.6f) * 10.f, (gw.cam_z - 14745.6f) * 10.f);
        cloud_offset = gw.world.Out().cloud_offset; darkness = gw.world.Out().darkness;
    }
    const md::KenshiSkySettings ks;
    const auto ml = ks.GetMainLight(hours);
    const md::KsVec3 zen = ks.ZenithLight(hours);
    Mat4 view = cam.ViewMatrix(); const float* v = mat4_ptr(view);
    float u[16] = { v[0], v[4], v[8], 0.f,  v[1], v[5], v[9], 0.f,  -v[2], -v[6], -v[10], 0.f,  std::tan(cam.fovy * 0.00872664f), (float)window_get_width() / (float)window_get_height(), 0.f, 0.f };
    // layer options as the exe sets them (private notes): height 100, scale 0.001, wind (1, 1), distance attenuation 0.05, detail attenuation 1,
    // height volume 0.25, volumetric displacement 0.01, density multiplier 3; density offset and darkness follow the weather of the camera's zone; uTime is unused by the shader
    float f[24] = { zen.x, zen.y, zen.z, 0.f,  ml.colour.x, ml.colour.y, ml.colour.z, ml.w,  ks.atmosphere.exposure, 100.f, 0.001f, 0.f,
                    1.f, 1.f, 0.25f, 0.01f,  0.05f, 1.f, 3.f, cloud_offset,  darkness, 0.f, 0.f, 0.f };
    SDL_GPUTextureSamplerBinding tsb[3] = { { g.clouds.SDLTexture(), g.clouds.SDLSampler() }, { g.normal.SDLTexture(), g.normal.SDLSampler() }, { g.tile.SDLTexture(), g.tile.SDLSampler() } };
    pv.BindPipeline(&g.pipe);
    pv.BindFragmentSamplers(0, tsb, 3);
    pv.PushVertexUniforms(0, u, sizeof(u)); pv.PushFragmentUniforms(0, f, sizeof(f));
    pv.Draw(3, 1, 0, 0);
}
}
#else
namespace SkyClouds { void Draw(GpuPassView&, const MdCamera&, float) {} void SetClock(double) {} void SetCameraWorld(float, float) {} void SetWeatherOverride(bool, float, float, float) {} float OverrideWetness() { return 0.f; } }
#endif
