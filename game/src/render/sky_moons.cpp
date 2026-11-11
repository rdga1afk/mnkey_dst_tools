#include "sky_moons.h"
#ifdef MD_HLSL_RUNTIME
#include "kenshi_hlsl.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/io/ogre_mesh.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/kenshi_sky.h>
#include <monkey_dust/platform/window.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const char* kVsGlue = R"HLSL(
#include "@ORIG@"
[[vk::binding(0, 1)]] cbuffer UV {
    column_major float4x4 g_world; column_major float4x4 g_wvp;
    float4 g_cam;   // xyz cameraPos, w uInnerRadius
    float4 g_sun;   // xyz sunDirectionReal, w uExposure
    float4 g_iw;    // xyz uInvWaveLength, w uScale
    float4 g_k0;    // x uKrESun, y uKr4PI, z uKm4PI, w uScaleDepth
    float4 g_k1;    // x uScaleOverScaleDepth, y uCameraPos.y
}
struct VSOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float3 n : TEXCOORD1;
    [[vk::location(2)]] float4 col : COLOR0;
};
VSOut main([[vk::location(0)]] float3 p : POSITION, [[vk::location(1)]] float2 uv : TEXCOORD0, [[vk::location(2)]] float3 n : NORMAL)
{
    VSOut o;
    main_vs(float4(p, 1.0), uv, n, o.pos, o.uv, o.n, o.col, g_world, g_wvp, g_cam.xyz, g_sun.xyz,
            float3(0.0, g_k1.y, 0.0), g_iw.xyz, g_cam.w, g_k0.x, g_k0.y, g_k0.z, g_iw.w, g_k0.w, g_k1.x, g_sun.w);
    return o;
}
)HLSL";

const char* kFsGlue = R"HLSL(
#include "@ORIG@"
[[vk::binding(0, 3)]] cbuffer UF { float4 g_sunf; }
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D g_tex;
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState g_smp;
float4 main(float4 pos : SV_Position, [[vk::location(0)]] float2 uv : TEXCOORD0, [[vk::location(1)]] float3 n : TEXCOORD1, [[vk::location(2)]] float4 col : COLOR0) : SV_Target0
{
    return main_fs(pos, uv, n, col, g_tex, g_smp, g_sunf.xyz);
}
)HLSL";

struct Moon { md::ogre::Mesh mesh; GpuStaticBuffer vbo, ibo; uint32_t index_count = 0; GpuTexture tex; float dir[3]; float size; float speed; float radius = 1.f; };

int g_day = 0;

struct State {
    bool tried = false, ok = false;
    GpuPipeline pipe;
    Moon moon[2];
} g;

void Init() {
    g.tried = true;
    const std::string mats = md::KenshiDir() + "/data/materials";
    std::string vs = kVsGlue, fs = kFsGlue; const std::string orig = mats + "/forward/moon.hlsl";
    vs.replace(vs.find("@ORIG@"), 6, orig); fs.replace(fs.find("@ORIG@"), 6, orig);
    md::hlsl::Compiled cv, cf;
    if (!md::hlsl::Compile(vs, { orig }, 'v', cv)) { fprintf(stderr, "[SkyMoons] moon.hlsl vertex does not compile:\n%s\n", cv.log.c_str()); return; }
    if (!md::hlsl::Compile(fs, { orig }, 'f', cf)) { fprintf(stderr, "[SkyMoons] moon.hlsl fragment does not compile:\n%s\n", cf.log.c_str()); return; }
    // placement the exe sets up when it creates the two objects: direction = normalised offset, size, rotation speed per minute (private notes)
    const float dirs[2][3] = { { 1.f, 0.3f, -1.f }, { 1.f, 0.14f, -0.7f } };
    const float sizes[2] = { 35.f, 10.f }, speeds[2] = { 1.f, 4.7233f };
    const char* tex[2] = { "moon_HI.dds", "moon2_HI.dds" };
    md::ogre::Error err;
    for (int i = 0; i < 2; ++i) {
        Moon& m = g.moon[i];
        if (!md::ogre::ReadMesh(mats + "/planet01.mesh", m.mesh, err) || m.mesh.submeshes.empty()) { fprintf(stderr, "[SkyMoons] planet01.mesh: %s\n", err.what.c_str()); return; }
        const auto& sm = m.mesh.submeshes[0];
        const md::ogre::Geometry& geo = sm.use_shared ? m.mesh.shared : sm.geometry;
        std::vector<float> P, N, U;
        if (!geo.Positions(P) || !geo.Normals(N) || !geo.Uvs(0, U)) { fprintf(stderr, "[SkyMoons] planet01.mesh lacks position / normal / uv\n"); return; }
        std::vector<float> v; v.reserve(geo.vertex_count * 8);
        for (uint32_t k = 0; k < geo.vertex_count; ++k) { for (int c = 0; c < 3; ++c) v.push_back(P[k * 3 + c]); v.push_back(U[k * 2]); v.push_back(U[k * 2 + 1]); for (int c = 0; c < 3; ++c) v.push_back(N[k * 3 + c]); }
        m.vbo.Init(0x8892 /*GL_ARRAY_BUFFER*/, v.data(), (uint32_t)(v.size() * 4));
        m.ibo.Init(0x8893 /*GL_ELEMENT_ARRAY_BUFFER*/, sm.indices.data(), (uint32_t)(sm.indices.size() * 4));
        m.index_count = (uint32_t)sm.indices.size();
        m.radius = m.mesh.has_bounds && m.mesh.bounds_radius > 0.f ? m.mesh.bounds_radius : 1.f;
        const float l = std::sqrt(dirs[i][0] * dirs[i][0] + dirs[i][1] * dirs[i][1] + dirs[i][2] * dirs[i][2]);
        for (int c = 0; c < 3; ++c) m.dir[c] = dirs[i][c] / l;
        m.size = sizes[i]; m.speed = speeds[i];
        GpuSamplerDesc sd; sd.min_filter = GpuSamplerDesc::Filter::LINEAR; sd.gen_mipmap = false;
        if (!m.tex.InitFromFile((mats + "/" + tex[i]).c_str(), sd)) { fprintf(stderr, "[SkyMoons] %s failed\n", tex[i]); return; }
    }
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/sky_install_moon.vert"; pd.frag_path = "shaders/sky_install_moon.frag";
    pd.vert_spv_path = cv.cache_path.c_str(); pd.frag_spv_path = cf.cache_path.c_str();
    pd.layout.count = 3; pd.layout.stride = 32;
    pd.layout.attribs[0] = { 0, 0, GpuAttribFmt::F3 }; pd.layout.attribs[1] = { 1, 12, GpuAttribFmt::F2 }; pd.layout.attribs[2] = { 2, 20, GpuAttribFmt::F3 };
    pd.vert_uniform_bufs = 1; pd.frag_uniform_bufs = 1; pd.frag_samplers = 1;
    pd.has_depth_target = true; pd.raster.depth_test = true; pd.raster.depth_write = false; pd.raster.cull_back = false;
    if (!g.pipe.Create(pd)) { fprintf(stderr, "[SkyMoons] pipeline create failed\n"); return; }
    g.ok = true;
    fprintf(stderr, "[SkyMoons] moon.hlsl from the install: vertex %s %.1f ms, fragment %s %.1f ms\n", cv.from_cache ? "cache" : "compiled", cv.compile_ms, cf.from_cache ? "cache" : "compiled", cf.compile_ms);
}

} // namespace

namespace SkyMoons {
void SetDay(int day) { g_day = day; }
void Draw(GpuPassView& pv, const MdCamera& cam, const float sun[3], float hours) {
    if (!getenv("MD_SKY_INSTALL")) return;
    if (!g.tried) Init();
    if (!g.ok) return;
    const md::KenshiAtmosphere A;
    const float PI = 3.14159265f, thick = A.outer_radius - A.inner_radius, scale = 1.f / thick, depth = thick * 0.5f;
    const float asp = (float)window_get_width() / (float)window_get_height();
    const float d = 0.5f * md::ViewFarClipM();   // only the direction and the angular size matter: the vertex function forces the depth to the far plane
    const float tan_s = std::tan(0.01f * PI / 180.f);
    pv.BindPipeline(&g.pipe);
    for (int i = 0; i < 2; ++i) {
        Moon& m = g.moon[i];
        const float s = m.size * d * tan_s;
        const float ang = ((hours + 24.f * (float)g_day) * 60.f) * m.speed / 1440.f;   // minutes since day 0 x speed per minute
        Mat4 world = mat4_mul(mat4_translate(cam.pos.x + m.dir[0] * d, cam.pos.y + m.dir[1] * d, cam.pos.z + m.dir[2] * d), mat4_mul(mat4_rotate_y(ang), mat4_scale(s, s, s)));
        Mat4 wvp = mat4_mul(cam.ViewProjMatrix(asp), world);
        struct { float world[16], wvp[16], cam[4], sun[4], iw[4], k0[4], k1[4]; } u;
        std::memcpy(u.world, mat4_ptr(world), 64); std::memcpy(u.wvp, mat4_ptr(wvp), 64);
        u.cam[0] = cam.pos.x; u.cam[1] = cam.pos.y; u.cam[2] = cam.pos.z; u.cam[3] = A.inner_radius;
        u.sun[0] = sun[0]; u.sun[1] = sun[1]; u.sun[2] = sun[2]; u.sun[3] = A.exposure;
        for (int c = 0; c < 3; ++c) u.iw[c] = 1.f / std::pow((&A.wavelength.x)[c], 4.f);
        u.iw[3] = scale;
        u.k0[0] = A.rayleigh * A.sun_intensity; u.k0[1] = A.rayleigh * 4.f * PI; u.k0[2] = A.mie * 4.f * PI; u.k0[3] = depth;
        u.k1[0] = scale / depth; u.k1[1] = A.inner_radius + thick * A.height_position; u.k1[2] = u.k1[3] = 0.f;
        float sf[4] = { sun[0], sun[1], sun[2], 0.f };
        SDL_GPUTextureSamplerBinding tsb{ m.tex.SDLTexture(), m.tex.SDLSampler() };
        pv.BindVertexBuffer(&m.vbo); pv.BindIndexBuffer(&m.ibo, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        pv.BindFragmentSamplers(0, &tsb, 1);
        pv.PushVertexUniforms(0, &u, sizeof(u)); pv.PushFragmentUniforms(0, sf, sizeof(sf));
        pv.DrawIndexed(m.index_count);
    }
}
}
#else
namespace SkyMoons { void Draw(GpuPassView&, const MdCamera&, const float[3], float) {} void SetDay(int) {} }
#endif
