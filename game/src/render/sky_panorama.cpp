#include "sky_panorama.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <chrono>
#include <cmath>
#include <string>
#include <cstdio>
#include <cstdlib>
#ifdef MD_HLSL_RUNTIME
#include "kenshi_hlsl.h"
#endif

// Game speed steps 1.0 / 2.0 / 5.0 and 0 while paused: docs/spec/mechanics/game_speed.md (read against the exe).
static float g_game_speed = 1.f;

#ifdef MD_HLSL_RUNTIME
namespace {
// Path 1 (docs/spec/shaders/path1_adapter.md): our glue around the ORIGINAL materials/SkyX/SkyX_Skydome.hlsl (HDR + STARFIELD variant).
// Panorama texel -> dome vertex (iNPosition = unit direction, iUV = polar projection, as SkyX's MeshManager builds the dome), then the original
// main_vp and main_fp are called per texel. DEVIATION: the original evaluates main_vp per dome vertex and interpolates; here it is per texel.
const char* kSkyGlue = R"HLSL(
#define STARFIELD
#include "@ORIG@"
[[vk::binding(0, 2)]] sampler2D uStarfieldTex;
cbuffer U : register(b0, space3) {
    float4 g_sun;      // xyz uLightDir, w uTime
    float4 g_pad0; float4 g_pad1;
    float4 g_cam;      // x uCameraPos.y, y uInnerRadius, z uScale, w uScaleDepth
    float4 g_k0;       // x uScaleOverScaleDepth, y uKrESun, z uKmESun, w uKr4PI
    float4 g_k1;       // x uKm4PI, y uG, z uG2, w uExposure
    float4 g_wl;       // xyz uInvWaveLength, w uSamples
}
static const float PI_ = 3.14159265358979;
float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    float A = (uv.x - 0.5) * 2.0 * PI_;
    float el = uv.y * uv.y * 0.5 * PI_;
    float3 d = float3(cos(el) * cos(A), sin(el), cos(el) * sin(A));
    float r = acos(clamp(d.y, -1.0, 1.0)) / (0.5 * PI_);
    float2 duv = float2((cos(A) * r + 1.0) * 4.0, (sin(A) * r + 1.0) * 4.0);
    float4 oPos; float2 oUV; float3 oRay; float3 oMie; float3 oDir; float2 oOpa; float oH;
    float4x4 ident = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    main_vp(float4(0, 0, 0, 1), d, duv, 1.0, oPos, oUV, oRay, oMie, oDir, oOpa, oH, ident,
            g_sun.xyz, float3(0.0, g_cam.x, 0.0), g_wl.xyz, g_cam.y, g_k0.y, g_k0.z, g_k0.w, g_k1.x,
            g_cam.z, g_cam.w, g_k0.x, (int)g_wl.w, g_wl.w);
    float4 col;
    main_fp(float4(0, 0, 0, 0), oUV, oRay, oMie, oDir, oOpa, oH, col, g_sun.w, g_sun.xyz, g_k1.y, g_k1.z, g_k1.w, uStarfieldTex);
    return col;
}
)HLSL";

// SkyX::update accumulates mTime += timeMultiplier * dt and gives the dome uTime = mTime * 0.5 while the star field is on; the exe sets the multiplier to game speed * 0.0091667 (0 when paused).
float StarTime() {
    static double accum = 0.0; static std::chrono::steady_clock::time_point last; static bool have = false;
    const auto now = std::chrono::steady_clock::now();
    float dt = have ? std::chrono::duration<float>(now - last).count() : 0.f;
    last = now; have = true; if (dt > 0.1f) dt = 0.1f;
    accum += 0.0091666666 * (double)g_game_speed * (double)dt;
    return (float)(accum * 0.5);
}

// SkyX AtmosphereManager::_update (SkyX_x64.dll, skyx_verdicts.tsv): shader uniforms from the Options the exe gives.
void SkyUniforms(const float sun[3], float utime, float u[32]) {
    const float inner = 9.77501f, outer = 10.2963f, height = 0.01f, kr = 0.0022f, km = 0.000675f, esun = 30.f, g = -0.991f, expo = 1.4f;
    const float wl[3] = { 0.57f, 0.48f, 0.44f }; const int samples = 4; const float PI = 3.14159265f;
    const float thick = outer - inner, scale = 1.f / thick, depth = thick * 0.5f;
    for (int i = 0; i < 32; ++i) u[i] = 0.f;
    u[0] = sun[0]; u[1] = sun[1]; u[2] = sun[2]; u[3] = utime;   // uTime: the star field scroll, see StarTime()
    u[12] = (thick * height) + inner; u[13] = inner; u[14] = scale; u[15] = depth;
    u[16] = scale / depth; u[17] = kr * esun; u[18] = km * esun; u[19] = kr * 4.f * PI;
    u[20] = km * 4.f * PI; u[21] = g; u[22] = g * g; u[23] = expo;
    for (int i = 0; i < 3; ++i) u[24 + i] = 1.f / powf(wl[i], 4.f);
    u[27] = (float)samples;
}
} // namespace
#endif

void SkyPanorama::SetGameSpeed(float speed) { g_game_speed = speed; }

SkyPanorama& SkyPanorama::Get() { static SkyPanorama s; return s; }

bool SkyPanorama::Init() {
    tried_ = true;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/upscale.vert"; pd.frag_path = "shaders/sky_pano.frag";
#ifdef MD_HLSL_RUNTIME
    std::string install_spv;
    if (getenv("MD_SKY_INSTALL")) {   // path 1: the sky from the install's SkyX HLSL, default off
        const std::string orig = md::KenshiDir() + "/data/materials/SkyX/SkyX_Skydome.hlsl";
        std::string glue = kSkyGlue; glue.replace(glue.find("@ORIG@"), 6, orig);
        md::hlsl::Compiled c;
        if (md::hlsl::Compile(glue, { orig }, 'f', c)) { install_spv = c.cache_path; pd.frag_path = "shaders/sky_install_pano.frag"; pd.frag_spv_path = install_spv.c_str(); use_install_ = true;
            fprintf(stderr, "[SkyPanorama] SkyX_Skydome.hlsl from the install: %s %.1f ms\n", c.from_cache ? "cache" : "compiled", c.compile_ms); }
        else fprintf(stderr, "[SkyPanorama] SkyX_Skydome.hlsl does not compile, old sky stays:\n%s\n", c.log.c_str());
    }
#endif
    pd.layout.count = 0; pd.layout.stride = 0;
    pd.raster.depth_test = false; pd.raster.depth_write = false; pd.raster.cull_back = false;
    pd.has_depth_target = false; pd.frag_uniform_bufs = 1; pd.frag_samplers = 4;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    if (!pipe_.Create(pd)) { fprintf(stderr, "[SkyPanorama] pipeline create failed\n"); return false; }
    {   // the SkyX textures of the install (DXT1): star field 4096^2 with mips, the cloud maps 1024^2
        const std::string dir = md::KenshiDir() + "/data/materials/SkyX/";
        GpuSamplerDesc sd; sd.min_filter = GpuSamplerDesc::Filter::LINEAR; sd.gen_mipmap = false;
        if (!star_.InitFromFile((dir + "SkyX_Starfield.dds").c_str(), sd) || !clouds_.InitFromFile((dir + "Clouds.dds").c_str(), sd) ||
            !clouds_normal_.InitFromFile((dir + "CloudsNormal.dds").c_str(), sd) || !clouds_tile_.InitFromFile((dir + "CloudsTile.dds").c_str(), sd)) { fprintf(stderr, "[SkyPanorama] SkyX textures failed\n"); return false; }
    }
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; const int sc = use_install_ ? (getenv("MD_SKY_PANO_SCALE") ? atoi(getenv("MD_SKY_PANO_SCALE")) : 4) : 1; ti.width = sc * W; ti.height = sc * H; ti.layer_count_or_depth = 1; ti.num_levels = 1;   // the install's dome needs the stars sharp: 1024 x 512, redrawn rarely
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_ = GpuCreateTexture(dev, &ti);
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT; si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = GpuCreateSampler(dev, &si);
    ready_ = tex_ && sampler_;
    return ready_;
}

void SkyPanorama::Shutdown() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev && tex_) GpuReleaseTexture(dev, tex_);
    if (dev && sampler_) GpuReleaseSampler(dev, sampler_);
    tex_ = nullptr; sampler_ = nullptr; pipe_.Destroy(); star_.Shutdown(); clouds_.Shutdown(); clouds_normal_.Shutdown(); clouds_tile_.Shutdown(); ready_ = valid_ = tried_ = false;
}

void SkyPanorama::Update(md::GpuCommandBufferHandle cmd, const float sun[3], const float zenith[3], const float sun_colour[3]) {
    if (!tried_) Init();
    if (!ready_) return;
    // The install's dome is 1024 x 512 and costs a spike when redrawn in one frame (owner 09.11): a redraw runs as STRIPES stripes, one per frame, each a scissored band of
    // the same full-screen pass (the sun and the zenith of the cycle are those of its first frame; the previous panorama stays in the other bands meanwhile).
    const int stripes = use_install_ ? STRIPES : 1;
    if (stripe_ == 0) {
        const float d = sun[0] * last_sun_[0] + sun[1] * last_sun_[1] + sun[2] * last_sun_[2];
        const bool moved = !valid_ || d < 0.99999f;   // about a quarter of a degree
        if (!moved && ++since_ < REDRAW_FRAMES) return;
        since_ = 0; last_sun_[0] = sun[0]; last_sun_[1] = sun[1]; last_sun_[2] = sun[2];
        for (int i = 0; i < 3; ++i) { cycle_sun_[i] = sun[i]; cycle_zenith_[i] = zenith[i]; cycle_colour_[i] = sun_colour[i]; }
    }
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd = cmd; cpd.color_tex[0] = tex_; cpd.depth_tex = nullptr; cpd.load_color = valid_ || stripe_ > 0;   // the first fill clears, every later band keeps what is there
    cb.BeginColorPass(cpd);
    if (stripes > 1) {
        const int th = (use_install_ ? (getenv("MD_SKY_PANO_SCALE") ? atoi(getenv("MD_SKY_PANO_SCALE")) : 4) : 1) * H;
        const SDL_Rect band = { 0, stripe_ * th / stripes, (use_install_ ? (getenv("MD_SKY_PANO_SCALE") ? atoi(getenv("MD_SKY_PANO_SCALE")) : 4) : 1) * W, th / stripes };
        GpuSetScissor(cb.SDLPass(), band);
    }
    cb.BindPipeline(&pipe_);
    SDL_GPUTextureSamplerBinding tsb[4] = { { star_.SDLTexture(), star_.SDLSampler() }, { clouds_.SDLTexture(), clouds_.SDLSampler() }, { clouds_normal_.SDLTexture(), clouds_normal_.SDLSampler() }, { clouds_tile_.SDLTexture(), clouds_tile_.SDLSampler() } };
    cb.BindFragmentSamplers(0, tsb, 4);
#ifdef MD_HLSL_RUNTIME
    if (use_install_) { float ui[32]; SkyUniforms(cycle_sun_, StarTime(), ui); cb.PushFragmentUniforms(0, ui, sizeof(ui)); cb.Draw(3, 0); cb.EndPass(); if (++stripe_ >= stripes) { stripe_ = 0; valid_ = true; ++redraws_; } return; }
#endif
    float u[16] = { cycle_sun_[0], cycle_sun_[1], cycle_sun_[2], 0.f, cycle_zenith_[0], cycle_zenith_[1], cycle_zenith_[2], 0.f, cycle_colour_[0], cycle_colour_[1], cycle_colour_[2], 0.f, 0.f, 0.f, 0.f, 0.f };   // Darkness 0 (clear weather)
    cb.PushFragmentUniforms(0, u, sizeof(u));
    cb.Draw(3, 0);
    cb.EndPass();
    stripe_ = 0; valid_ = true; ++redraws_;
}
