#include "sgsr2_pass.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <monkey_dust/render/md_camera.h>

namespace {
// column-major 4x4 inverse (cofactor form), double precision; false when singular
bool Inv4(const double m[16], double o[16]) {
    double inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    const double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (std::fabs(det) < 1e-300) return false;
    for (int i = 0; i < 16; ++i) o[i] = inv[i] / det;
    return true;
}
void Mul4(const double a[16], const double b[16], double o[16]) {   // o = a * b, column-major
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) { double s = 0; for (int k = 0; k < 4; ++k) s += a[k*4 + r] * b[c*4 + k]; o[c*4 + r] = s; }
}
void Unproj(const double inv[16], double x, double y, double z, double out[3]) {
    const double v[4] = { x, y, z, 1.0 }; double r[4];
    for (int i = 0; i < 4; ++i) r[i] = inv[i]*v[0] + inv[4+i]*v[1] + inv[8+i]*v[2] + inv[12+i]*v[3];
    for (int i = 0; i < 3; ++i) out[i] = r[i] / r[3];
}
struct Params {   // std140, the UBO of both shaders (128 B)
    float clip_to_prev[16];
    float render_size[2], output_size[2], render_rcp[2], output_rcp[2], jitter[2], scale_ratio[2];
    float fov_tan, min_lerp, reset; uint32_t same_camera;
};
static_assert(sizeof(Params) == 128, "std140 layout of the SGSR 2 UBO");
}

void Sgsr2Pass::BeginFrame(bool on) {
    if (!on || !rw_) { g_proj_jitter_ndc[0] = g_proj_jitter_ndc[1] = 0.f; jit_px_[0] = jit_px_[1] = 0.f; return; }
    auto halton = [](int i, int b) { float f = 1.f, r = 0.f; while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; } return r; };
    const int i = (jit_n_++ & 15) + 1;
    jit_px_[0] = halton(i, 2) - 0.5f; jit_px_[1] = halton(i, 3) - 0.5f;
    g_proj_jitter_ndc[0] = jit_px_[0] * 2.f / (float)rw_;
    g_proj_jitter_ndc[1] = -jit_px_[1] * 2.f / (float)rh_;   // texture v points down, NDC y up (the other three sign combinations were measured worse)
}

bool Sgsr2Pass::Init() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/upscale.vert";
    pd.layout.count = 0; pd.layout.stride = 0;
    pd.raster.depth_test = false; pd.raster.depth_write = false; pd.raster.cull_back = false;
    pd.has_depth_target = false; pd.frag_uniform_bufs = 1;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    pd.frag_path = "shaders/sgsr2_convert.frag"; pd.frag_samplers = 2;
    if (!convert_.Create(pd)) { fprintf(stderr, "[Sgsr2Pass] convert pipeline create failed\n"); return false; }
    pd.frag_path = "shaders/sgsr2_upscale.frag"; pd.frag_samplers = 3;
    if (!upscale_.Create(pd)) { fprintf(stderr, "[Sgsr2Pass] upscale pipeline create failed\n"); return false; }
    pd.frag_path = "shaders/upscale.frag"; pd.frag_samplers = 1; pd.color_format = SDL_GPU_TEXTUREFORMAT_INVALID;   // the history onto the swapchain: plain copy (sharp = 0)
    if (!blit_.Create(pd)) { fprintf(stderr, "[Sgsr2Pass] blit pipeline create failed\n"); return false; }
    SDL_GPUSamplerCreateInfo si{};
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST; nearest_ = GpuCreateSampler(dev, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;  linear_  = GpuCreateSampler(dev, &si);
    ready_ = nearest_ && linear_;
    return ready_;
}

void Sgsr2Pass::Shutdown() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev) {
        if (motion_) GpuReleaseTexture(dev, motion_);
        if (zero_) GpuReleaseTexture(dev, zero_);
        for (SDL_GPUTexture*& t : hist_) if (t) GpuReleaseTexture(dev, t);
        if (nearest_) GpuReleaseSampler(dev, nearest_);
        if (linear_) GpuReleaseSampler(dev, linear_);
    }
    motion_ = zero_ = nullptr; hist_[0] = hist_[1] = nullptr; nearest_ = linear_ = nullptr;
    convert_.Destroy(); upscale_.Destroy(); blit_.Destroy(); ready_ = false; rw_ = rh_ = ow_ = oh_ = 0;
}

void Sgsr2Pass::Ensure(int rw, int rh, int ow, int oh) {
    if (!ready_ || rw <= 0 || rh <= 0 || ow <= 0 || oh <= 0) return;
    if (motion_ && rw == rw_ && rh == rh_ && ow == ow_ && oh == oh_) return;
    md::GpuDevice::Get().WarnIfActive("Sgsr2Pass::Ensure");
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (motion_) GpuReleaseTexture(dev, motion_);
    if (zero_) GpuReleaseTexture(dev, zero_);
    zero_ready_ = false; zero_ = nullptr;
    for (SDL_GPUTexture*& t : hist_) if (t) { GpuReleaseTexture(dev, t); t = nullptr; }
    motion_ = nullptr;
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (uint32_t)rw; ti.height = (uint32_t)rh; motion_ = GpuCreateTexture(dev, &ti); zero_ = GpuCreateTexture(dev, &ti);
    ti.width = (uint32_t)ow; ti.height = (uint32_t)oh; hist_[0] = GpuCreateTexture(dev, &ti); hist_[1] = GpuCreateTexture(dev, &ti);
    if (motion_ && zero_ && hist_[0] && hist_[1]) { rw_ = rw; rh_ = rh; ow_ = ow; oh_ = oh; } else { fprintf(stderr, "[Sgsr2Pass] targets %dx%d / %dx%d create failed\n", rw, rh, ow, oh); rw_ = rh_ = ow_ = oh_ = 0; }
    have_prev_ = false;   // the history is empty: the first frame resets the accumulation
}

void Sgsr2Pass::Apply(md::GpuCommandBufferHandle cmd, SDL_GPUTexture* scene, SDL_GPUTexture* depth, const float vp_f[16], md::GpuTextureHandle out) {
    if (!ready_ || !rw_ || !scene || !depth || !out) return;
    // the exact view-projection of this frame (with the jitter): the clip position was shifted by jitter * w, take it out; the inverse of the float inverse (S7InvVp) is useless here: near 0.5 m / far 1e5 m makes it ~1e-3 off
    double vp[16], inv[16];
    for (int i = 0; i < 16; ++i) vp[i] = (double)vp_f[i];
    const double jx = (double)g_proj_jitter_ndc[0], jy = (double)g_proj_jitter_ndc[1];
    for (int c = 0; c < 4; ++c) { vp[c*4 + 0] -= jx * vp[c*4 + 3]; vp[c*4 + 1] -= jy * vp[c*4 + 3]; }
    if (!Inv4(vp, inv)) return;
    double ctp[16];
    const bool reset = !have_prev_;
    if (have_prev_) Mul4(prev_vp_, inv, ctp); else { for (int i = 0; i < 16; ++i) ctp[i] = (i % 5 == 0) ? 1.0 : 0.0; }
    // largest screen motion (render pixels) of a few test points when going from this clip space to the previous one: the still-camera test (matrix entries carry the 1e-3 float noise of the 1e4 m translation, pixels do not)
    double motion_px = 0;
    if (have_prev_) {
        const double tp[5][3] = { { -0.8, -0.8, 0.99 }, { 0.8, 0.8, 0.99 }, { 0.8, -0.8, 0.997 }, { 0.0, 0.0, 0.9995 }, { 0.0, 0.0, 0.995 } };   // about 50 m, 50 m, 170 m, 1 km, 100 m
        for (const auto& t : tp) {
            double r[4]; for (int i = 0; i < 4; ++i) r[i] = ctp[i]*t[0] + ctp[4+i]*t[1] + ctp[8+i]*t[2] + ctp[12+i];
            const double dx = (r[0] / r[3] - t[0]) * 0.5 * rw_, dy = (r[1] / r[3] - t[1]) * 0.5 * rh_;
            motion_px = std::fmax(motion_px, std::fmax(std::fabs(dx), std::fabs(dy)));
        }
    }
    // horizontal field of view from two rays (left / right edge) at two depths: tan(fov / 2) is what the shader wants
    double a0[3], a1[3], b0[3], b1[3];
    Unproj(inv, -1, 0, 0.1, a0); Unproj(inv, -1, 0, 0.9, a1); Unproj(inv, 1, 0, 0.1, b0); Unproj(inv, 1, 0, 0.9, b1);
    double da[3], db[3], na = 0, nb = 0, dt = 0;
    for (int i = 0; i < 3; ++i) { da[i] = a1[i] - a0[i]; db[i] = b1[i] - b0[i]; na += da[i]*da[i]; nb += db[i]*db[i]; dt += da[i]*db[i]; }
    const double c = (na > 0 && nb > 0) ? dt / std::sqrt(na * nb) : 1.0;
    const double fov = std::acos(c < -1 ? -1 : (c > 1 ? 1 : c));
    Params p{};
    for (int i = 0; i < 16; ++i) p.clip_to_prev[i] = (float)ctp[i];
    p.render_size[0] = (float)rw_; p.render_size[1] = (float)rh_; p.output_size[0] = (float)ow_; p.output_size[1] = (float)oh_;
    p.render_rcp[0] = 1.f / rw_; p.render_rcp[1] = 1.f / rh_; p.output_rcp[0] = 1.f / ow_; p.output_rcp[1] = 1.f / oh_;
    const float ratio = (float)ow_ / (float)rw_;
    p.scale_ratio[0] = std::fmin(20.f, std::pow((float)(ow_ * oh_) / (float)(rw_ * rh_), 3.f)); p.scale_ratio[1] = ratio;
    p.jitter[0] = jit_px_[0]; p.jitter[1] = jit_px_[1];
    p.fov_tan = (float)std::tan(fov * 0.5); p.min_lerp = 0.25f; p.reset = reset ? 1.f : 0.f; p.same_camera = (have_prev_ && motion_px < 0.05) ? 1u : 0u;
    if (p.same_camera) for (int i = 0; i < 16; ++i) p.clip_to_prev[i] = (i % 5 == 0) ? 1.f : 0.f;   // a still camera has exactly zero motion (float noise of the matrix product is above the shader's 1e-6 test and drops the history)
    std::memcpy(prev_vp_, vp, sizeof(vp)); have_prev_ = true;

    GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd;
    if (!zero_ready_) {   // InputVelocity: x <= 0 selects the depth-derived motion, so a texture of zeros is bound
        cpd.cmd = cmd; cpd.color_tex[0] = zero_; cpd.depth_tex = nullptr; cpd.load_color = false; cpd.clear_color[3] = 0.f;
        cb.BeginColorPass(cpd); cb.EndPass(); zero_ready_ = true;
    }
    cpd.cmd = cmd; cpd.color_tex[0] = motion_; cpd.depth_tex = nullptr; cpd.load_color = false;
    cb.BeginColorPass(cpd);
    cb.BindPipeline(&convert_);
    SDL_GPUTextureSamplerBinding cin[2] = { { depth, nearest_ }, { zero_, nearest_ } };
    cb.BindFragmentSamplers(0, cin, 2);
    cb.PushFragmentUniforms(0, &p, sizeof(p));
    cb.Draw(3, 0); cb.EndPass();

    SDL_GPUTexture* prev = hist_[cur_ ^ 1]; SDL_GPUTexture* next = hist_[cur_];
    cpd.color_tex[0] = next; cpd.load_color = false;
    cb.BeginColorPass(cpd);
    cb.BindPipeline(&upscale_);
    SDL_GPUTextureSamplerBinding uin[3] = { { prev, linear_ }, { motion_, linear_ }, { scene, linear_ } };
    cb.BindFragmentSamplers(0, uin, 3);
    cb.PushFragmentUniforms(0, &p, sizeof(p));
    cb.Draw(3, 0); cb.EndPass();

    cpd.color_tex[0] = out; cpd.load_color = false;
    cb.BeginColorPass(cpd);
    cb.BindPipeline(&blit_);
    SDL_GPUTextureSamplerBinding bin{ next, linear_ };
    cb.BindFragmentSamplers(0, &bin, 1);
    const float u[4] = { 1.f / (float)ow_, 1.f / (float)oh_, 0.f, 0.f };
    cb.PushFragmentUniforms(0, u, sizeof(u));
    cb.Draw(3, 0); cb.EndPass();
    cur_ ^= 1;
}
