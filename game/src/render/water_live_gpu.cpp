#include "water_live_gpu.h"
#include <monkey_dust/platform/md_log.h>
#include <cmath>
#include <cstring>
#include <utility>

namespace md {
namespace {

// Neutral inputs (the engine has none of these yet; the values are the ones of the oracle harness tests/hlsl_runtime/water_fs_rt.cpp where it uses constants):
// s2 turbulence, s3 depth (0 = ground at farClip: deep water), s5 rain (rainAmount is 0), s6 reflection (grey), s7 scum, s8 scum normal, s9/s10 irradiance / specularity cubes, s11 blend map (zero: blend 1).
const float kNeutral[12][4] = {
    { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f, 1.f }, { 0.40f, 0.60f, 0.f, 1.f }, { 0.f, 0.f, 0.f, 1.f }, { 0.56f, 0.62f, 0.47f, 1.f }, { 0.55f, 0.45f, 0.65f, 0.35f },
    { 0.65f, 0.65f, 0.65f, 1.f }, { 0.30f, 0.45f, 0.20f, 0.60f }, { 0.52f, 0.70f, 0.48f, 0.80f }, { 0.30f, 0.34f, 0.40f, 0.8f }, { 0.25f, 0.30f, 0.45f, 0.6f }, { 0.f, 0.f, 0.f, 0.f } };

SDL_GPUTexture* NeutralTex(SDL_GPUDevice* dev, bool cube, const float* rgba) {
    const int layers = cube ? 6 : 1;
    SDL_GPUTextureCreateInfo ti = {}; ti.type = cube ? SDL_GPU_TEXTURETYPE_CUBE : SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width = 1; ti.height = 1; ti.layer_count_or_depth = layers; ti.num_levels = 1;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(dev, &ti); if (!t) return nullptr;
    SDL_GPUTransferBufferCreateInfo bi = {}; bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; bi.size = 16;
    SDL_GPUTransferBuffer* up = SDL_CreateGPUTransferBuffer(dev, &bi);
    memcpy(SDL_MapGPUTransferBuffer(dev, up, false), rgba, 16); SDL_UnmapGPUTransferBuffer(dev, up);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev); SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    for (int l = 0; l < layers; ++l) { SDL_GPUTextureTransferInfo s = { up, 0, 1, 1 }; SDL_GPUTextureRegion d = {}; d.texture = t; d.layer = l; d.w = 1; d.h = 1; d.d = 1; SDL_UploadToGPUTexture(cp, &s, &d, false); }
    SDL_EndGPUCopyPass(cp); SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f); SDL_ReleaseGPUTransferBuffer(dev, up);
    return t;
}

bool SetNamed(const std::vector<hlsl::UniformMember>& mem, std::vector<unsigned char>& ub, const char* name, const float* v, int count) {
    for (const auto& u : mem) if (u.name == name) { memcpy(&ub[u.offset], v, 4 * count); return true; }
    return false;   // a missing name is not an error here: the original renames written parameters (name__w) and the inventory is printed by md_water_live_dry
}

// farClip of the material (UNCHECKED, see docs/research/WATER_LIVE_INPUTS.md): the depth map and `farClip` of waterFP must use the same value
const float kFarClip = 40000.f;

bool Invert4(const float m[16], float out[16]) {   // general 4x4 inverse (column-major in, column-major out)
    float a[4][8] = {};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) a[r][c] = m[c * 4 + r];
    for (int r = 0; r < 4; ++r) a[r][4 + r] = 1.f;
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c + 1; r < 4; ++r) if (fabsf(a[r][c]) > fabsf(a[p][c])) p = r;
        if (fabsf(a[p][c]) < 1e-20f) return false;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
        const float d = a[c][c]; for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const float f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[c * 4 + r] = a[r][4 + c];
    return true;
}

}   // namespace

bool WaterLivePass::InitDepthMap() {
    if (!hlsl::Compile(kWaterDepthVert, {}, 'v', dvs_c_) || !hlsl::Compile(kWaterDepthFrag, {}, 'f', dfs_c_)) { MD_LOG(MD_LOG_WARNING, "water live: depth glue: %s %s", dvs_c_.log.c_str(), dfs_c_.log.c_str()); return false; }
    uint32_t sz = 0; if (!hlsl::GlobalBlockMembers(dfs_c_.spv, dmem_, sz)) return false;
    dsz_ = sz; for (const auto& u : dmem_) if (u.offset + 64 > dsz_ && u.name == "invVP") dsz_ = u.offset + 64; for (const auto& u : dmem_) if (u.offset + 16 > dsz_) dsz_ = u.offset + 16;
    SDL_GPUShaderCreateInfo sv = {}; sv.code = (const Uint8*)dvs_c_.spv.data(); sv.code_size = dvs_c_.spv.size() * 4; sv.entrypoint = "main"; sv.format = SDL_GPU_SHADERFORMAT_SPIRV; sv.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    SDL_GPUShaderCreateInfo sf = sv; sf.code = (const Uint8*)dfs_c_.spv.data(); sf.code_size = dfs_c_.spv.size() * 4; sf.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; sf.num_samplers = 1; sf.num_uniform_buffers = 1;
    dvs_ = SDL_CreateGPUShader(dev_, &sv); dfs_ = SDL_CreateGPUShader(dev_, &sf);
    if (!dvs_ || !dfs_) { MD_LOG(MD_LOG_WARNING, "water live: depth shader: %s", SDL_GetError()); return false; }
    SDL_GPUColorTargetDescription ct = {}; ct.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = dvs_; gi.fragment_shader = dfs_; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE; gi.target_info.color_target_descriptions = &ct; gi.target_info.num_color_targets = 1;
    dpipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &gi);
    SDL_GPUSamplerCreateInfo sci = {}; sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_NEAREST; sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = sci.address_mode_v = sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; nsmp_ = SDL_CreateGPUSampler(dev_, &sci);
    return dpipe_ && nsmp_;
}

void WaterLivePass::BuildDepthMap(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* scene_depth, uint32_t w, uint32_t h, const float vp[16], const float cam[3]) {
    dvalid_ = false;
    if (!pipe_ || !dpipe_ || !scene_depth || !w || !h) return;
    if (!dtex_ || dw_ != w || dh_ != h) {
        if (dtex_) SDL_ReleaseGPUTexture(dev_, dtex_);
        SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT; ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = w; ti.height = h; ti.layer_count_or_depth = 1; ti.num_levels = 1; dtex_ = SDL_CreateGPUTexture(dev_, &ti); dw_ = w; dh_ = h;
        if (!dtex_) { MD_LOG(MD_LOG_WARNING, "water live: depth map texture: %s", SDL_GetError()); return; }
    }
    std::vector<unsigned char> ub(dsz_, 0); const float cf[4] = { cam[0], cam[1], cam[2], kFarClip };
    float inv_vp[16]; if (!Invert4(vp, inv_vp)) return;
    SetNamed(dmem_, ub, "invVP", inv_vp, 16); SetNamed(dmem_, ub, "camFar", cf, 4);
    SDL_GPUColorTargetInfo ci = {}; ci.texture = dtex_; ci.load_op = SDL_GPU_LOADOP_DONT_CARE; ci.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ci, 1, nullptr); if (!p) return;
    SDL_BindGPUGraphicsPipeline(p, dpipe_);
    SDL_GPUTextureSamplerBinding tb = { scene_depth, nsmp_ }; SDL_BindGPUFragmentSamplers(p, 0, &tb, 1);
    SDL_PushGPUFragmentUniformData(cmd, 0, ub.data(), (Uint32)ub.size());
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0); SDL_EndGPURenderPass(p);
    static bool logged = false; if (!logged) { logged = true; MD_LOG(MD_LOG_INFO, "water live: depth map built (%ux%u, scene depth sampler bound, far clip %.0f)", w, h, kFarClip); }
    dvalid_ = true;
}

bool WaterLivePass::Init(SDL_GPUDevice* dev, SDL_Window* win, const std::string& kd) {
    dev_ = dev;
    std::string log;
    if (!CompileWaterLive(kd, sh_, log)) { MD_LOG(MD_LOG_WARNING, "water live: %s", log.c_str()); return false; }
    SDL_GPUShaderCreateInfo sv = {}; sv.code = (const Uint8*)sh_.vs.spv.data(); sv.code_size = sh_.vs.spv.size() * 4; sv.entrypoint = "main"; sv.format = SDL_GPU_SHADERFORMAT_SPIRV;
    sv.stage = SDL_GPU_SHADERSTAGE_VERTEX; sv.num_uniform_buffers = 1;
    SDL_GPUShaderCreateInfo sf = sv; sf.code = (const Uint8*)sh_.fs.spv.data(); sf.code_size = sh_.fs.spv.size() * 4; sf.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; sf.num_samplers = 12;
    vs_ = SDL_CreateGPUShader(dev, &sv); fs_ = SDL_CreateGPUShader(dev, &sf);
    if (!vs_ || !fs_) { MD_LOG(MD_LOG_WARNING, "water live: shader: %s", SDL_GetError()); return false; }
    // vertex input: float4 position (location 0), float4 normal (location 1) — the locations glslang gave TEXCOORD6 / TEXCOORD7 are printed by md_water_live_dry
    SDL_GPUVertexBufferDescription vbd = {}; vbd.slot = 0; vbd.pitch = 32; vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute va[2] = {}; va[0].location = 0; va[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[0].offset = 0; va[1].location = 1; va[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[1].offset = 16;
    SDL_GPUColorTargetDescription ct = {}; ct.format = SDL_GetGPUSwapchainTextureFormat(dev, win);
    ct.blend_state.enable_blend = true; ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA; ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD; ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE; ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = vs_; gi.fragment_shader = fs_; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gi.vertex_input_state.vertex_buffer_descriptions = &vbd; gi.vertex_input_state.num_vertex_buffers = 1; gi.vertex_input_state.vertex_attributes = va; gi.vertex_input_state.num_vertex_attributes = 2;
    gi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE; gi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; gi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    gi.depth_stencil_state.enable_depth_test = true; gi.depth_stencil_state.enable_depth_write = false; gi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    gi.target_info.color_target_descriptions = &ct; gi.target_info.num_color_targets = 1; gi.target_info.has_depth_stencil_target = true; gi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    pipe_ = SDL_CreateGPUGraphicsPipeline(dev, &gi);
    if (!pipe_) { MD_LOG(MD_LOG_WARNING, "water live: pipeline: %s", SDL_GetError()); return false; }
    // the plane: two triangles of a unit square in object space (y = 0), scaled / moved by pWorldMatrix; normal (0,1,0,0)
    const float c[4][2] = { { -1.f, -1.f }, { 1.f, -1.f }, { 1.f, 1.f }, { -1.f, 1.f } }; const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    float v[6][8]; for (int i = 0; i < 6; ++i) { v[i][0] = c[idx[i]][0]; v[i][1] = 0.f; v[i][2] = c[idx[i]][1]; v[i][3] = 1.f; v[i][4] = 0.f; v[i][5] = 1.f; v[i][6] = 0.f; v[i][7] = 0.f; }
    SDL_GPUBufferCreateInfo bci = {}; bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX; bci.size = sizeof(v); vb_ = SDL_CreateGPUBuffer(dev, &bci);
    SDL_GPUTransferBufferCreateInfo tci = {}; tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tci.size = sizeof(v); SDL_GPUTransferBuffer* up = SDL_CreateGPUTransferBuffer(dev, &tci);
    memcpy(SDL_MapGPUTransferBuffer(dev, up, false), v, sizeof(v)); SDL_UnmapGPUTransferBuffer(dev, up);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev); SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = { up, 0 }; SDL_GPUBufferRegion dst = { vb_, 0, sizeof(v) }; SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp); SDL_GPUFence* fe = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); SDL_WaitForGPUFences(dev, true, &fe, 1); SDL_ReleaseGPUFence(dev, fe); SDL_ReleaseGPUTransferBuffer(dev, up);
    for (int i = 0; i < 12; ++i) neutral_[i] = NeutralTex(dev, i == 9 || i == 10, kNeutral[i]);
    SDL_GPUSamplerCreateInfo sci = {}; sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_LINEAR; sci.address_mode_u = sci.address_mode_v = sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    smp_ = SDL_CreateGPUSampler(dev, &sci);
    // GlobalBlockMembers sizes a trailing float4x4 by its first row: every matrix member extends the block to offset + 64 (as ground_live_pass.cpp does for the light block)
    uint32_t vsz = sh_.vsz, fsz = sh_.fsz;
    for (const auto& u : sh_.vmem) if (u.name[0] == 'p' && (u.name.find("Matrix") != std::string::npos || u.name.find("ViewProj") != std::string::npos) && u.offset + 64 > vsz) vsz = u.offset + 64;
    for (const auto& u : sh_.fmem) if (u.offset + 16 > fsz) fsz = u.offset + 16;
    vub_.assign(vsz, 0); fub_.assign(fsz, 0);
    for (int i = 0; i < 12; ++i) if (!neutral_[i]) return false;
    if (!InitDepthMap()) MD_LOG(MD_LOG_WARNING, "water live: depth map not available, s3 stays the neutral 0 (deep water everywhere)");
    MD_LOG(MD_LOG_INFO, "water live: original waterVP / waterFP(BLEND) ready (vertex %zu words, fragment %zu words)", sh_.vs.spv.size(), sh_.fs.spv.size());
    return true;
}

void WaterLivePass::Draw(SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd, const WaterLiveFrame& f) {
    if (!pipe_) return;
    std::fill(vub_.begin(), vub_.end(), 0); std::fill(fub_.begin(), fub_.end(), 0);
    // pWorldMatrix = T(cam.x, water_y, cam.z) * S(half, 1, half) (glm column-major memory); the object-space camera is then (0, (cam.y-water_y), 0) / scale on x,z
    const float h = f.half_size;
    const float world[16] = { h, 0, 0, 0,  0, 1, 0, 0,  0, 0, h, 0,  f.cam[0], f.water_y, f.cam[2], 1 };
    const float inv[16]   = { 1.f / h, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1.f / h, 0,  -f.cam[0] / h, -f.water_y, -f.cam[2] / h, 1 };
    float wvp[16];   // vp * world, column-major
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) { float s = 0.f; for (int k = 0; k < 4; ++k) s += f.vp[k * 4 + r] * world[c * 4 + k]; wvp[c * 4 + r] = s; }
    const float cam_obj[4] = { 0.f, f.cam[1] - f.water_y, 0.f, 1.f };   // camera in object space: x,z are 0 because the plane is centred on the camera
    const float scale[4] = { 0.004f, 0.004f, 0.f, 0.f };                 // material water: scale 0.004 (the translated port uses the same)
    const float bounds[4] = { 0.f, 0.f, 29500.f, 29500.f };              // flowmap / watercolourmap cover the whole world (MAP_SIZE of the port)
    const float off[3] = { f.abs_x, 0.f, f.abs_z }, fog0[4] = { 0.f, 0.f, 0.f, 0.f };
    SetNamed(sh_.vmem, vub_, "scale", scale, 4); SetNamed(sh_.vmem, vub_, "mapBounds", bounds, 4); SetNamed(sh_.vmem, vub_, "pCameraPos", cam_obj, 4); SetNamed(sh_.vmem, vub_, "pFogParams", fog0, 4);
    SetNamed(sh_.vmem, vub_, "worldOffset", off, 3); SetNamed(sh_.vmem, vub_, "sunDirection", f.to_sun, 3);
    SetNamed(sh_.vmem, vub_, "pWorldMatrix", world, 16); SetNamed(sh_.vmem, vub_, "pInverseWorldMatrix", inv, 16); SetNamed(sh_.vmem, vub_, "pWorldViewProj", wvp, 16);
    const float sunc[4] = { f.sun_colour[0], f.sun_colour[1], f.sun_colour[2], 1.f }, ambp[4] = { 0.f, 0.f, 0.f, 1.f }, env[4] = { f.ambient[0], f.ambient[1], f.ambient[2], 1.f };
    const float far_clip = kFarClip, inv_strength = 1.f, inv_opacity = 0.01f, dist[2] = { 120.f, 0.01f }, gloss = 0.9f, glow = 0.f, rain = 0.f, blend_ch = 4.f, fogf[4] = { 40000.f, 0.f, 0.f, 0.f };
    SetNamed(sh_.fmem, fub_, "sunColour", sunc, 4); SetNamed(sh_.fmem, fub_, "GROUND_Colour", f.ambient, 3); SetNamed(sh_.fmem, fub_, "ambientParams", ambp, 4); SetNamed(sh_.fmem, fub_, "sunDirection", f.to_sun, 3);
    SetNamed(sh_.fmem, fub_, "envColour__w", env, 4); SetNamed(sh_.fmem, fub_, "FOG_FAR", fogf, 4); SetNamed(sh_.fmem, fub_, "farClip", &far_clip, 1); SetNamed(sh_.fmem, fub_, "invStrength", &inv_strength, 1);
    SetNamed(sh_.fmem, fub_, "invOpacity", &inv_opacity, 1); SetNamed(sh_.fmem, fub_, "distortion", dist, 2); SetNamed(sh_.fmem, fub_, "gloss__w", &gloss, 1); SetNamed(sh_.fmem, fub_, "glow", &glow, 1);
    SetNamed(sh_.fmem, fub_, "gameTime__w", &f.time_s, 1); SetNamed(sh_.fmem, fub_, "rainAmount__w", &rain, 1); SetNamed(sh_.fmem, fub_, "blendChannel", &blend_ch, 1);
    SDL_GPUTextureSamplerBinding tb[12];
    for (int i = 0; i < 12; ++i) tb[i] = { neutral_[i], smp_ };
    if (dvalid_) tb[3] = { dtex_, nsmp_ };
    static bool dlogged = false; if (!dlogged) { dlogged = true; MD_LOG(MD_LOG_INFO, "water live: Draw binds s3 = %s", dvalid_ ? "the depth map" : "the neutral texture (depth map not built this frame)"); }
    tb[0] = { f.colour, f.colour_smp }; tb[1] = { f.flow, f.flow_smp }; tb[4] = { f.normal, f.normal_smp };
    SDL_BindGPUGraphicsPipeline(rp, pipe_);
    SDL_GPUBufferBinding vbb = { vb_, 0 }; SDL_BindGPUVertexBuffers(rp, 0, &vbb, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, vub_.data(), (Uint32)vub_.size());
    SDL_PushGPUFragmentUniformData(cmd, 0, fub_.data(), (Uint32)fub_.size());
    SDL_BindGPUFragmentSamplers(rp, 0, tb, 12);
    SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
}

void WaterLivePass::Shutdown() {
    if (!dev_) return;
    for (auto& t : neutral_) if (t) { SDL_ReleaseGPUTexture(dev_, t); t = nullptr; }
    if (smp_) { SDL_ReleaseGPUSampler(dev_, smp_); smp_ = nullptr; }
    if (nsmp_) { SDL_ReleaseGPUSampler(dev_, nsmp_); nsmp_ = nullptr; }
    if (dtex_) { SDL_ReleaseGPUTexture(dev_, dtex_); dtex_ = nullptr; }
    if (dpipe_) { SDL_ReleaseGPUGraphicsPipeline(dev_, dpipe_); dpipe_ = nullptr; }
    if (dvs_) { SDL_ReleaseGPUShader(dev_, dvs_); dvs_ = nullptr; }
    if (dfs_) { SDL_ReleaseGPUShader(dev_, dfs_); dfs_ = nullptr; }
    if (vb_) { SDL_ReleaseGPUBuffer(dev_, vb_); vb_ = nullptr; }
    if (pipe_) { SDL_ReleaseGPUGraphicsPipeline(dev_, pipe_); pipe_ = nullptr; }
    if (vs_) { SDL_ReleaseGPUShader(dev_, vs_); vs_ = nullptr; }
    if (fs_) { SDL_ReleaseGPUShader(dev_, fs_); fs_ = nullptr; }
    dev_ = nullptr;
}

}   // namespace md
