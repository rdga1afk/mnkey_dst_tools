#include "town_buildings.h"
#include "sky_panorama.h"
#include "sky_moons.h"
#include "sky_clouds.h"
#include "ground_live_driver.h"
#include "ground_live_hdr.h"
#include "water_live_gpu.h"
#include <monkey_dust/render/ground_slot.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <string>
#include "town_foliage.h"
#include "npc_render_internal.h"
#include <monkey_dust/render/kenshi_sky.h>
#include <monkey_dust/render/kenshi_hdr.h>
extern int g_opt_ground_split;
extern bool g_opt_ground_reuse;
extern float g_frame_dt_s, g_adapt_override_mean, g_adapt_override_dt;
int g_kenshi_water = 1;   // #173: 1 = Kenshi water (shaders/water_kenshi.frag) at its own level (coordinator 22.10: default on, ABBA within +1.5 ms); 0 = the old plane
static constexpr float kKenshiWaterY = 100.f * 980.f / 65535.f;   // raw 100 of fullmap.tif = 1.495 m (scripts/water_frame_estimate.py, docs/research/WATER_173_FINDINGS.md)
// the three maps of data/materials: water.png (normals, repeats), flowmap.png and watercolourmap.png (clamp, whole world); loaded before the scene pass (a texture upload is a copy pass)
static GpuTexture s_kw_normal, s_kw_flow, s_kw_colour;
static const TerrainShadingProjected::KLightParams* UpdateKLight();
static bool KenshiWaterTexturesReady() {
    static int state = 0;   // 0 not tried, 1 ready, -1 failed
    if (state == 0) {
        const std::string m = md::KenshiDir() + "/data/materials/", l = md::KenshiDir() + "/data/newland/land/overlaymaps/";
        GpuSamplerDesc rep; rep.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP; rep.gen_mipmap = true;
        GpuSamplerDesc clamp = rep; clamp.wrap_s = clamp.wrap_t = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        GpuSamplerDesc clampLin = clamp; clampLin.min_filter = GpuSamplerDesc::Filter::LINEAR; clampLin.gen_mipmap = false;
        state = (s_kw_normal.InitFromFile((m + "water.png").c_str(), rep) && s_kw_flow.InitFromFile((l + "flowmap.png").c_str(), clamp) &&
                 s_kw_colour.InitFromFile((l + "watercolourmap.png").c_str(), clampLin)) ? 1 : -1;
        if (state < 0) fprintf(stderr, "[Water] Kenshi water textures failed\n");
    }
    return state > 0;
}
// Water live (path 1): the original waterVP / waterFP instead of shaders/water_kenshi.frag; default OFF, MD_WATER_LIVE=1. Created before the scene pass (it uploads through its own command buffers).
static md::WaterLivePass s_water_live;
static bool WaterLiveReady() {
    static int state = 0;   // 0 not tried, 1 ready, -1 off / failed
    if (state == 0) {
        const char* e = getenv("MD_WATER_LIVE");
        state = (e && e[0] == '1' && s_water_live.Init(md::GpuDevice::Get().SDLDevice(), md::GpuDevice::Get().Window(), md::KenshiDir())) ? 1 : -1;
    }
    return state > 0;
}
extern int g_kenshi_lighting_mode;   // lua_scenario_api_misc.cpp, md.set_kenshi_lighting   // npc_render_deferred.cpp

void NpcRender::DrawScene(FrameCtx& fc, const MdCamera& cam, float now_s,
                          MdEntity player_entity,
                          float& cam_x, float& cam_z,
                          float cam_az)
{
// render-pass-graph-2026-09-03: whole-scene kill switch (terrain/water/
// NPCs/everything this function draws) -- debug/profiling toggle to
// isolate postprocess (SSAO) GPU cost from scene-draw
// cost. Returns before GpuProfiler::BeginPass so no unmatched Begin/End
// pair; scene_color/gbuffer targets simply keep last frame's contents,
// same accepted behavior as evsm_shadow's gate.
if (!md::RenderPassGraph::Get().IsEnabled("draw_scene")) return;
md::GpuProfiler::Get().BeginPass("Scene");
SDL_GPURenderPass* rp = nullptr;

// G-3: water/land mask — must be sampled + uploaded to its SSBO BEFORE the
// main scene render pass opens below (SSBO::UploadInCmd opens its own
// short-lived copy pass on fc.cmd; SDL_GPU disallows nesting a copy pass
// inside an already-active render pass, same constraint as BakeAlbedo
// just below). See the draw site further down (G-3 comment) for why this
// replaced the old camera-position-only CPU gate (task #158i-4/9).
bool water_any = false;
{
    constexpr int N = SceneRender::kWaterMaskN;
    static float water_mask[N * N];
    // task #158i-8/9: the REAL bug behind the translucent "water sheet" over
    // dry mountains (user-reported, screenshots 2026-07-15 21:00-21:04) was
    // never the 50m shore margin (tightening it to 3m in the previous commit
    // did NOT fix it -- confirmed via a [DIAG-water] instrumented run:
    // wet_cells=496/1024, i.e. 48% of the grid, over ordinary highland
    // terrain nowhere near real water). Root cause: the water quad's
    // world_sz half-extent was LARGER than the loaded TNKN=9 chunk window's
    // half-extent, so most of the sample grid fell OUTSIDE loaded terrain.
    // TerrainQuery::GetHeight(wx,wz)'s no-bool convenience overload silently
    // returns 0.0 for out-of-range positions (its doc comment: "safe for
    // non-entity use") -- and 0.0 is BELOW WATER_LEVEL+margin, so every
    // off-the-loaded-map sample got counted as "wet". Fixed by using the
    // bool-returning overload directly: a position with no real terrain data
    // is neither wet nor dry, so it must NOT set the mask at all (leave it 0,
    // don't count it in water_any).
    //
    // kWaterWorldSz/kWaterMaskN now live on SceneRender (see its own doc
    // comment, task water-mask-false-positive-bleed) -- a second, LARGER
    // false-positive-bleed bug distinct from the one above: even a genuinely
    // real, correctly-detected wet sample (inside loaded terrain) got
    // bilinear-smeared across its whole coarse grid cell by water.frag,
    // painting dry ground far from the actual pocket. Both call sites here
    // and the draw call's WaterVertUBO::world_sz below must read the same
    // SceneRender constant, never restate their own literal.
    constexpr float kWaterShoreMarginM = 3.f;
    constexpr float kWaterWorldSz      = SceneRender::kWaterWorldSz;
    const auto& tq = TerrainQuery::Get();
    static bool raw_wet[N * N];
    static float depth_m[N * N];   // #173: metres below the Kenshi water level
    for (int gz = 0; gz < N; ++gz) {
        float wz = cam.pos.z - kWaterWorldSz + (2.f * kWaterWorldSz) * gz / (N - 1);
        for (int gx = 0; gx < N; ++gx) {
            float wx = cam.pos.x - kWaterWorldSz + (2.f * kWaterWorldSz) * gx / (N - 1);
            float h = 0.f;
            bool has_data = tq.GetHeight(wx, wz, h);
            raw_wet[gz * N + gx] = has_data && (g_kenshi_water ? h < kKenshiWaterY : h < TerrainQuery::WATER_LEVEL + kWaterShoreMarginM);
            depth_m[gz * N + gx] = (has_data && h < kKenshiWaterY) ? kKenshiWaterY - h : 0.f;
        }
    }

    // task water-mask-isolated-sample-bleed (2026-07-31, round 3): tightening
    // the discard/alpha threshold (round 2) still left isolated single-cell
    // false positives visible -- direct probing at a user-reported location
    // found only 3/4096 raw-wet cells, ALL clustered at one distant grid
    // edge, yet water.frag's bilinear smear from even ONE such isolated cell
    // is enough to paint a visible patch. A real lake is never a single
    // isolated cell -- it spans a contiguous cluster. Erode: a cell only
    // survives into the final mask (and water_any) if at least 5 of its own
    // 3x3 neighbourhood (itself + 8 neighbours) are ALSO raw-wet, killing
    // isolated single/double-cell noise outright while leaving any real
    // multi-cell lake's interior fully intact (its neighbours are wet too).
    constexpr int kMinNeighborWet = 5;
    for (int gz = 0; gz < N; ++gz) {
        for (int gx = 0; gx < N; ++gx) {
            int count = 0;
            for (int dz = -1; dz <= 1; ++dz) {
                int nz = gz + dz;
                if (nz < 0 || nz >= N) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = gx + dx;
                    if (nx < 0 || nx >= N) continue;
                    if (raw_wet[nz * N + nx]) ++count;
                }
            }
            bool wet = count >= kMinNeighborWet;
            water_mask[gz * N + gx] = wet ? (g_kenshi_water ? depth_m[gz * N + gx] + 0.05f : 1.f) : 0.f;   // #173: the Kenshi water reads metres of depth
            water_any |= wet;
        }
    }
    if (g_kenshi_water && KenshiWaterTexturesReady()) WaterLiveReady();   // at the first frame, not when water first comes into view (town: a 225 ms frame on the first water)
    if (water_any)
        SceneRender::Get().water_mask_ssbo.UploadInCmd(fc.cmd, water_mask, (int)sizeof(water_mask));
}

// AmbientProbeSystem (docs/RESOLVE_OPT.md session): per-frame update so the directional sky term tracks the live sun.
// SetSkyLight's L1 coefficients are ADDITIVE (see ambient_probe.cpp's doc comment) -- Clear()+PlaceProbe() first so
// re-running this every frame doesn't accumulate. Same maths TerrainRenderer::SunParams uses in DrawTerrainAndProps
// (-sun_dir, mean sun colour as strength, ambient colour). Uploaded as a copy pass inside this frame's command buffer
// BEFORE any render pass opens (SDL_GPU disallows nesting); slotted SSBO -- no extra submit, no fence wait (this used
// to be Upload() = 3 synchronous submits per frame, FRAME_AUDIT #5).
{
    const auto& ls_p = LightSystem::Get();
    const float p_str = (ls_p.sun_color.x + ls_p.sun_color.y + ls_p.sun_color.z) / 3.f;
    AmbientProbeSystem::Get().Clear();
    int probeIdx = AmbientProbeSystem::Get().PlaceProbe(0.f, 0.f, 0.f, 0.f);
    if (probeIdx >= 0) {
        AmbientProbeSystem::Get().SetAmbientColor(probeIdx, ls_p.ambient_color.x, ls_p.ambient_color.y, ls_p.ambient_color.z);
        AmbientProbeSystem::Get().SetSkyLight(probeIdx, -ls_p.sun_dir.x, -ls_p.sun_dir.y, -ls_p.sun_dir.z,
                                              p_str * 0.45f, p_str * 0.45f, p_str * 0.45f);
    }
    AmbientProbeSystem::Get().UploadInCmd(fc.cmd);
}

// #163 5b-2: the ground resolve in its own pass before the main pass (plain scene colour only).
SceneRender::Get().ground_split_active_ = false;
SceneRender::Get().lum_adapt_active_ = false;
SceneRender::Get().gr_have_ &= SceneRender::Get().gr_wanted_;
if (g_opt_ground_split > 0 && depth.SDLTexture() && SceneRender::Get().terrain_granite_ready &&
    SceneRender::Get().terrain_resolve_repeat_debug == 1) {
    int gw = 0, gh = 0;
    RenderSize(gw, gh);
    auto& gsp = SceneRender::Get().ground_split_pass_;
    if (gsp.Ready() && gsp.HasTarget(gw, gh) && SceneRender::Get().still_skip_ && SceneRender::Get().gr_have_) {   // #207 H14: the ground target of the last full frame is still valid for this view
        SceneRender::Get().ground_split_active_ = true;
        SceneRender::Get().lum_adapt_active_ = g_kenshi_lighting_mode >= 4 && gsp.AdaptReady() && gsp.IsFloat();
        --SceneRender::Get().gr_frame_;   // the reuse live-block rotation advances on full frames only
    } else
    if (gsp.Ready() && gsp.HasTarget(gw, gh)) {
        GpuPushDebugGroup(fc.cmd, "GroundResolve");
        GpuCommandBuffer gcb;
        GpuCommandBuffer::ColorPassDesc gcpd;
        gcpd.cmd            = fc.cmd;
        auto& sr_gr = SceneRender::Get();
        if (sr_gr.gr_wanted_) {   // #163 5b-2b: this frame writes the other target; the previous frame's is the history
            gsp.Flip();
            sr_gr.gr_params_.hist_colour = gsp.ReuseHistTex(); sr_gr.gr_params_.hist_sampler = gsp.LinearSampler();
            sr_gr.terrain_shading_projected_.SetReuse(&sr_gr.gr_params_);
        }
        gcpd.color_tex[0]   = gsp.Tex();
        gcpd.clear_color[0] = gcpd.clear_color[1] = gcpd.clear_color[2] = gcpd.clear_color[3] = 0.f;   // alpha 0 = no ground written
        gcpd.load_color     = false;
        gcpd.depth_tex      = depth.SDLTexture();
        gcpd.clear_depth    = 1.0f;
        gcpd.load_depth     = true;
        // #207 stencil split (default on, MD_STENCIL_SPLIT=0 off): this pass only draws the ground resolve, so it can carry its own depth-stencil target instead of the scene depth
        bool stencil_pass = false;
        if (sr_gr.terrain_shading_projected_.StencilSplitReady() && g_kenshi_lighting_mode > 0 && sr_gr.terrain.KenshiLightCubesReady()) {   // the split pipelines are the klight ones: the same condition as the resolve's use_klight   // the same condition as lum_adapt_active_ below: the float-target klight pipelines
            if (SDL_GPUTexture* dst = gsp.StencilDepthTex(gw, gh)) { gcpd.depth_tex = dst; gcpd.load_depth = false; gcpd.clear_stencil = true; stencil_pass = true; static const bool gdc = getenv("MD_GROUND_DEPTH_DONTCARE") != nullptr; if (gdc) gcpd.depth_dont_care = true; }   // H13.1: only the stencil of this attachment is used
        }

        sr_gr.terrain_shading_projected_.SetStencilSplitActive(stencil_pass);
        if (stencil_pass && (sr_gr.terrain_shading_projected_.PvtReady() || TerrainShadingProjected::OptionsWanted())) DrawGroundResolve(fc, cam, nullptr);   // #207 PVT: tile bake before the pass opens
        // MD_GROUND_LIVE_LIGHT: the live ground pass runs here, before the ground pass (the consumer reads its result in that pass).
        auto& gtsp = sr_gr.terrain_shading_projected_;
        const bool live_on = md::GroundLiveLightOn() && md::GroundLiveDriver::Wanted() && gtsp.GBufferDepth().SDLTexture() && gtsp.GBufferSlot();
        if (live_on) {
            md::GpuProfiler::Get().BeginPass("Ground Live");
            { SDL_GPUTextureSamplerBinding gsb[7]; sr_gr.terrain.GetSharedGroundSamplers(gsb); md::GroundLiveDriver::Get().SetMaps(gsb[0].texture, gsb[3].texture); }
            { const auto& hm = sr_gr.terrain_granite_hmap; md::GroundLiveDriver::Get().SetHeightmap(hm.IsReady() ? hm.Texture() : nullptr, hm.HeightMin(), hm.HeightMax(), hm.WorldExtent(), hm.Resolution()); }   // the cache bake (MD_GROUND_CACHE)
            static md::GroundLiveLightParams llp;   // the original main_fs (MD_GROUND_LIVE_ORIGLIGHT): sun from the same weather as the translated light, the ambient map and the two cubes of the install
            const auto* klp = UpdateKLight();
            const bool orig_light = md::GroundLiveDriver::OrigLight() && klp && sr_gr.terrain.KenshiLightCubesReady() && sr_gr.terrain.AmbientMapReady();
            if (orig_light) {
                for (int i = 0; i < 3; ++i) llp.sun_dir[i] = klp->sun_dir[i];
                for (int i = 0; i < 4; ++i) llp.sun_colour[i] = klp->sun_colour[i];
                llp.env_w = klp->params[0]; llp.ambient_w = klp->params[1];
                llp.ambient_map = sr_gr.terrain.AmbientMapTexture(); llp.ambient_smp = sr_gr.terrain.AmbientMapSampler();
                llp.irr_cube = sr_gr.terrain.KenshiIrradianceCube(); llp.irr_smp = sr_gr.terrain.KenshiIrradianceSampler();
                llp.spec_cube = sr_gr.terrain.KenshiSpecularityCube(); llp.spec_smp = sr_gr.terrain.KenshiSpecularitySampler();
            }
            md::GroundLiveDriver::Get().SetLight(orig_light ? &llp : nullptr);
            md::GroundLiveDriver::Get().Frame(md::GpuDevice::Get().SDLDevice(), fc.cmd, gtsp.GBufferDepth().SDLTexture(), gtsp.GBufferColor().SDLTexture(), gtsp.GBufferSlot(), gtsp.GBufferDepth().Width(), gtsp.GBufferDepth().Height(),
                                              gtsp.S7InvVp(), sr_gr.granite_abs_cam_, (uint64_t)sr_gr.frame_counter_pvt_);
            gtsp.SetLiveTargets(orig_light && md::GroundLiveDriver::Get().Pass().Lit() ? md::GroundLiveDriver::Get().Pass().Lit() : md::GroundLiveDriver::Get().Pass().AlbedoRgb(), md::GroundLiveDriver::Get().Pass().AuxB());   // the ground resolve of this pass reads the live material
            md::GpuProfiler::Get().EndPass();
        }
        sr_gr.lum_adapt_active_ = g_kenshi_lighting_mode >= 4 && gsp.AdaptReady() && gsp.IsFloat();   // #140 L4
        const bool live_hdr = !sr_gr.lum_adapt_active_ && gsp.IsFloat() && md::GroundLiveHdr::Active();   // release (1c): the ground target is before the exposure, the install HDR chain exposes it
        gtsp.SetFloatTarget(sr_gr.lum_adapt_active_);
        gcb.BeginColorPass(gcpd);
        if (SDL_GPURenderPass* grp = gcb.SDLPass()) {
            if (live_hdr) DrawSkyHdr(cam, grp, fc.cmd, stencil_pass);   // the luminance chain measures the sky too; the ground overwrites its own pixels
            DrawGroundResolve(fc, cam, grp);
            if (!gtsp.HasTranslatedResolve() && live_on) {   // release (no translated resolve): the live consumer = the original fog quad over the lit live ground + the fixed exposure
                md::GroundLiveFogParams fp;
                if (const auto* k = UpdateKLight()) { for (int i = 0; i < 3; ++i) fp.sun_real[i] = k->sun_real[i]; for (int i = 0; i < 4; ++i) fp.horizon[i] = k->horizon[i]; fp.fog_near = k->fog[0]; fp.fog_far = k->fog[1]; fp.skydome_radius = k->fog[2]; fp.exposure = live_hdr ? 1.f : k->params[2]; }
                md::GroundLiveDriver::Get().DrawFog(grp, fc.cmd, fp, gsp.Fmt(), stencil_pass ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT : SDL_GPU_TEXTUREFORMAT_D32_FLOAT);
            }
            SceneRender::Get().ground_split_active_ = true;
        }
        sr_gr.terrain_shading_projected_.SetReuse(nullptr);
        sr_gr.terrain_shading_projected_.SetStencilSplitActive(false);
        gcb.EndPass();
        if (sr_gr.lum_adapt_active_) {   // after the ground pass, before the main pass: this frame's adapted luminance, Kenshi's [MIN_LUMINANCE, MAX_LUMINANCE] (sky.GetMainLight min_lum, GLOBAL CONSTANTS)
            const md::KenshiSkySettings sky_a;
            const auto ml_a = sky_a.GetMainLight(LightSystem::Get().current_hours);
            gsp.Adapt(fc.cmd, g_adapt_override_dt > 0.f ? g_adapt_override_dt : g_frame_dt_s, ml_a.min_lum, sky_a.exposure_max, g_adapt_override_mean);
        } else if (live_hdr) {   // the same frame time and luminance limits as the translated adaptation
            const md::KenshiSkySettings sky_a;
            md::GroundLiveHdrParams hp; hp.min_luminance = sky_a.GetMainLight(LightSystem::Get().current_hours).min_lum; hp.max_luminance = sky_a.exposure_max;
            md::GroundLiveHdr::Get().Run(fc.cmd, gsp.Tex(), gw, gh, g_adapt_override_dt > 0.f ? g_adapt_override_dt : g_frame_dt_s, hp);
        }
        GpuPopDebugGroup(fc.cmd);
    }
}

// #207 H13.3 (MD_DEPTH_NOCOPY): the scene depth is the G-buffer depth; the NPC prepass runs here, after the ground resolve has sampled it
GpuDepthTexture& scene_depth = SceneRender::Get().depth_alias_ ? SceneRender::Get().terrain_shading_projected_.GBufferDepth() : depth;
if (SceneRender::Get().depth_alias_ && fc.ac > 0 && prepass_pipeline.SDLPipeline() && !npc_prepass_debug_disable) NpcDepthPrepass(fc, cam, true);
{
    // Forward path: render directly to swapchain. Uses AcquireSwapchainCached
    // (not GpuRenderPass::BeginColor's own internal acquire) — task #158f: a
    // second real SDL_AcquireGPUSwapchainTexture call later in this same
    // command buffer (e.g. an ad-hoc screenshot capture) deadlocked
    // SDL_SubmitGPUCommandBufferAndAcquireFence at the kernel level
    // (drm_syncobj_array_wait_timeout). All swapchain acquires on fc.cmd
    // must go through the cache so there is ever only one real acquire.
    uint32_t sc_w = 0, sc_h = 0;
    md::GpuTextureHandle sc_tex = AcquireSwapchainCached(fc, &sc_w, &sc_h);
    if (sc_tex) {
        GpuCommandBuffer cb;
        GpuCommandBuffer::ColorPassDesc cpd;
        cpd.cmd            = fc.cmd;
        cpd.color_tex[0]      = sc_tex;
        cpd.clear_color[0] = 0.f; cpd.clear_color[1] = 0.f;
        cpd.clear_color[2] = 0.f; cpd.clear_color[3] = 1.f;
        cpd.load_color     = false; // CLEAR
        {   // #207 H13.1: the swapchain image has no CCS (external image), so its clear is a slow blorp clear (0.42 ms at 1080p, INTEL_MEASURE "slow color clear"). The sky (fullscreen triangle) is drawn first and the ground / props / NPCs
            // cover the pixels it depth-rejects, so the clear value is never visible: frames identical at the Hub (PSNR 99). MD_SWAP_CLEAR=1 restores the clear.
            static const bool keep_clear = getenv("MD_SWAP_CLEAR") != nullptr;
            if (!keep_clear && SceneRender::Get().sky_pipeline.SDLPipeline() && md::RenderPassGraph::Get().IsEnabled("draw_sky")) cpd.color_dont_care = true;
        }
        bool load_depth = (fc.ac > 0 && prepass_pipeline.SDLPipeline()) ||
                          SceneRender::Get().terrain_granite_ready;
        if (scene_depth.SDLTexture()) {
            cpd.depth_tex   = scene_depth.SDLTexture();
            cpd.clear_depth = 1.0f;
            cpd.load_depth  = load_depth;
        }
        if (WaterLiveReady() && scene_depth.SDLTexture()) {   // MD_WATER_LIVE=1: the screen-space depth map the original's depthmap (s3) reads, built from the scene depth before the pass that samples it
            const float asp = (float)window_get_width() / (float)window_get_height();
            const Mat4 vpd = cam.ViewProjMatrix(asp); const float c3[3] = { cam.pos.x, cam.pos.y, cam.pos.z };
            s_water_live.BuildDepthMap(fc.cmd, scene_depth.SDLTexture(), sc_w, sc_h, mat4_ptr(vpd), c3);
        }
        cb.BeginColorPass(cpd);
        rp = cb.SDLPass();
        if (!rp) fprintf(stderr, "[GPU] Forward BeginColor FAILED: %s\n", SDL_GetError());
    }
}
if (rp) {
    // task #142 (2026-09-16): per-pass RenderPassGraph gates added purely
    // for live GPU-ms ablation (toward the user's ~240fps near-empty-
    // terrain target) -- RenderDoc's replay-time GPU-duration counter
    // proved unreliable here (near-identical ~8.2ms readings across 3
    // unrelated draws -- fullscreen terrain resolve, fullscreen HUD quad,
    // a 6312-index hair mesh -- pointed to query-instrumentation
    // serialization overhead on this driver, not real per-draw cost).
    // All default-enabled (RenderPassGraph's own "unknown pass -> true"
    // rule already made this safe pre-registration; Register() below
    // makes SetEnabled() actually take effect for a diagnostic toggle).
    // Zero behaviour change for normal gameplay.
    if (md::RenderPassGraph::Get().IsEnabled("draw_sky")) {
        GpuPushDebugGroup(fc.cmd, "DrawSky");
        DrawSky(fc, cam, rp);
        GpuPopDebugGroup(fc.cmd);
    }

    if (SceneRender::Get().terrain_granite_ready && md::RenderPassGraph::Get().IsEnabled("draw_terrain_props")) {
        // The ground goes in before the NPCs: its resolve writes no depth, the NPCs depth-test against the shared depth that already holds the ground depth.
        auto& sr_g = SceneRender::Get();
        const int resolve_repeat = sr_g.terrain_resolve_repeat_debug >= 0 ? sr_g.terrain_resolve_repeat_debug : 1;
        if (sr_g.ground_split_active_) { if (g_opt_ground_split == 1) { if (sr_g.lum_adapt_active_) sr_g.ground_split_pass_.CompositeExposed(rp, fc.cmd);
                                                                  else if (!(sr_g.ground_split_pass_.IsFloat() && md::GroundLiveHdr::Active() && md::GroundLiveHdr::Get().Composite(rp, fc.cmd, SDL_GetGPUSwapchainTextureFormat(md::GpuDevice::Get().SDLDevice(), _wnd::ptr()), SDL_GPU_TEXTUREFORMAT_D32_FLOAT))) sr_g.ground_split_pass_.Composite(rp, fc.cmd); } }   // #163 5b-2: the resolve already ran in its own pass
        else for (int _rr = 0; _rr < resolve_repeat; ++_rr) DrawGroundResolve(fc, cam, rp);
    }

    if (md::RenderPassGraph::Get().IsEnabled("draw_npc_forward")) {
        GpuPushDebugGroup(fc.cmd, "DrawNpcForward");
        DrawNpcForwardAndDissolve(fc, cam, rp);
        GpuPopDebugGroup(fc.cmd);
    }

    // -- Terrain: LOD0+POM close, LOD1-3 distant --
    if (md::RenderPassGraph::Get().IsEnabled("draw_terrain_props")) {
        GpuPushDebugGroup(fc.cmd, "DrawTerrainAndProps");
        DrawTerrainAndProps(fc, cam, now_s, rp);
        GpuPopDebugGroup(fc.cmd);
    }

    if (md::RenderPassGraph::Get().IsEnabled("draw_water")) {
        GpuPushDebugGroup(fc.cmd, "DrawWaterPlane");
        DrawWaterPlane(fc, cam, now_s, water_any, rp);
        GpuPopDebugGroup(fc.cmd);
    }

    if (md::RenderPassGraph::Get().IsEnabled("draw_hud")) {
        GpuPushDebugGroup(fc.cmd, "DrawHudOverlays");
        DrawHudOverlays(fc, cam, now_s, player_entity, cam_x, cam_z, cam_az, rp);
        GpuPopDebugGroup(fc.cmd);
    }
}
// ── Player clothing draw ─────────────────────────────────
if (md::RenderPassGraph::Get().IsEnabled("draw_player_cloth")) {
    GpuPushDebugGroup(fc.cmd, "DrawPlayerCloth");
    DrawPlayerCloth(fc, cam, rp);
    GpuPopDebugGroup(fc.cmd);
}
// ── Player hair draw ──────────────────────────────────────
if (md::RenderPassGraph::Get().IsEnabled("draw_player_hair")) {
    GpuPushDebugGroup(fc.cmd, "DrawPlayerHair");
    DrawPlayerHair(fc, cam, rp);
    GpuPopDebugGroup(fc.cmd);
}

#if defined(MONKEY_DUST_EDITOR)
// ImGui rendered at end of scene pass — covers editor, anim debug, pause overlay.
// (resolution_scale: with the upscale active it is drawn at full resolution by DrawFullResUi instead.)
if (rp && !UpscaleActive()) {
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd && dd->CmdListsCount > 0)
        ImGui_ImplSDLGPU3_RenderDrawData(dd, fc.cmd, rp);
}
#endif
// End scene render pass (both branches now build rp directly — see the
// AcquireSwapchainCached forward-path above, task #158f).
if (rp) SDL_EndGPURenderPass(rp);
if (HubSpike::Occlusion() && HubSpike::AnyActive() && scene_depth.SDLTexture()) {   // #119 occlusion: reduce this frame's scene depth for the culling of later frames
    HzbPass& hz = HzbPass::Get();
    if (!hz.Ready()) { static bool tried = false; if (!tried) { tried = true; hz.Init(); } }
    if (hz.Ready() && hz.Tick(HubSpike::occ_share_)) {
        HubSpike::occ_share_ = 0.f;
        MdCamera ac = SceneRender::Get().GraniteAbsCam(cam);
        hz.Record(fc.cmd, scene_depth, mat4_ptr(ac.ViewProjMatrix((float)window_get_width() / (float)window_get_height())), &ac.pos.x, 0.5f, md::ViewFarClipM());
    }
}
md::GpuProfiler::Get().EndPass(); // Scene
}

int g_kenshi_sky = 1;   // #173: 1 = SkyX sky (shaders/sky_kenshi.frag), default on (owner 22.10, s1); md.set_kenshi_sky(0) = old gradient

void NpcRender::DrawSky(FrameCtx& fc, const MdCamera& cam, SDL_GPURenderPass* rp)
{
    // -- Sky gradient FIRST (depth_test=false — must be before opaque geo) --
    if (SceneRender::Get().sky_pipeline.SDLPipeline()) {
        SkyUBO sky;
        Mat4 view = cam.ViewMatrix();
        const float* v = mat4_ptr(view);
        sky.cam_right[0] = v[0]; sky.cam_right[1] = v[4]; sky.cam_right[2] = v[8];  sky.cam_right[3] = 0;
        sky.cam_up[0]    = v[1]; sky.cam_up[1]    = v[5]; sky.cam_up[2]    = v[9];  sky.cam_up[3]    = 0;
        sky.cam_fwd[0]   =-v[2]; sky.cam_fwd[1]   =-v[6]; sky.cam_fwd[2]   =-v[10]; sky.cam_fwd[3]   = 0;
        const auto& ls = LightSystem::Get();
        sky.sun_dir[0] = ls.sun_dir.x; sky.sun_dir[1] = ls.sun_dir.y;
        sky.sun_dir[2] = ls.sun_dir.z; sky.sun_dir[3] = 0;
        float asp = (float)window_get_width() / (float)window_get_height();
        sky.fov_tan = tanf(cam.fovy * 0.00872664f);
        sky.aspect  = asp;
        {
            // Per-biome sky_horizon override removed: real Kenshi's sky/fog
            // is a global screen-space post-process driven by sun angle/
            // time-of-day, not per-biome data (see scene_render.cpp for the
            // full rationale) -- one fixed neutral sky-blue for every zone.
            sky.horizon_col[0] = 0.38f;
            sky.horizon_col[1] = 0.58f;
            sky.horizon_col[2] = 0.82f;
            sky.horizon_col[3] = 0.f;
        }
        GpuPipeline* sky_pipe = &SceneRender::Get().sky_pipeline; bool sky_pano = false;
        if (g_kenshi_sky && SceneRender::Get().sky_kenshi_pipeline.SDLPipeline() && SkyPanorama::Get().Ready()) {   // #173: SkyX sky; sun_dir = toward the real sun, horizon_col.x = the ground's fixed exposure
            const md::KenshiSkySettings ks;
            const float hours = LightSystem::Get().current_hours;
            const md::KsVec3 real = ks.SunDirection(hours);
            const auto ml = ks.GetMainLight(hours);
            const float day = std::min(std::max(ml.dir.y * 5.f, 0.f), 1.f);
            const float lum = ml.min_lum + (ks.exposure_max - ml.min_lum) * day;
            sky.sun_dir[0] = real.x; sky.sun_dir[1] = real.y; sky.sun_dir[2] = real.z;
            sky.horizon_col[0] = SceneRender::Get().lum_adapt_active_ ? 1.f : 0.55f / lum; sky.horizon_col[1] = sky.horizon_col[2] = sky.horizon_col[3] = 0.f;
            sky_pipe = &SceneRender::Get().sky_kenshi_pipeline; sky_pano = true;
        }
        GpuPassView pv = GpuPassView::FromRaw(rp, fc.cmd);
        pv.BindPipeline(sky_pipe);
        if (sky_pano) { SDL_GPUTextureSamplerBinding tsb{ SkyPanorama::Get().Tex(), SkyPanorama::Get().Sampler() }; pv.BindFragmentSamplers(0, &tsb, 1); }
        pv.PushVertexUniforms(0, &sky, sizeof(sky));
        pv.PushFragmentUniforms(0, &sky, sizeof(sky));
        pv.Draw(3, 1, 0, 0);
        if (sky_pano) { const md::KsVec3 rs = md::KenshiSkySettings().SunDirection(LightSystem::Get().current_hours); const float sd3[3] = { rs.x, rs.y, rs.z }; SkyMoons::Draw(pv, cam, sd3, LightSystem::Get().current_hours); { const SceneRender& sr = SceneRender::Get(); SkyClouds::SetCameraWorld((float)sr.zone_ox * CHUNK_SIZE + cam.pos.x - sr.tnoff_x, (float)sr.zone_oz * CHUNK_SIZE + cam.pos.z - sr.tnoff_z); } SkyClouds::Draw(pv, cam, LightSystem::Get().current_hours); }   // path 1: the two moons from the install (MD_SKY_INSTALL)
    }
}

// Release 1c: the install sky panorama into the float ground target before the ground, unexposed (factor 1) and RGB only, so its alpha stays 0:
// the original rt_hdr holds the sky too, and our HDR chain measures the whole target, while the composite (alpha < 0.5 discard) leaves the sky drawn by DrawSky.
// DEVIATION: moons, clouds, props and NPCs are not in this target (docs/DEVIATIONS.md).
void NpcRender::DrawSkyHdr(const MdCamera& cam, SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd, bool stencil_depth)
{
    GpuPipeline& pipe = SceneRender::Get().sky_kenshi_hdr_pipeline[stencil_depth ? 1 : 0];
    if (!g_kenshi_sky || !pipe.SDLPipeline() || !SkyPanorama::Get().Ready()) return;
    SkyUBO sky = {};
    const Mat4 view = cam.ViewMatrix();
    const float* v = mat4_ptr(view);
    sky.cam_right[0] = v[0]; sky.cam_right[1] = v[4]; sky.cam_right[2] = v[8];
    sky.cam_up[0]    = v[1]; sky.cam_up[1]    = v[5]; sky.cam_up[2]    = v[9];
    sky.cam_fwd[0]   =-v[2]; sky.cam_fwd[1]   =-v[6]; sky.cam_fwd[2]   =-v[10];
    const md::KsVec3 real = md::KenshiSkySettings().SunDirection(LightSystem::Get().current_hours);
    sky.sun_dir[0] = real.x; sky.sun_dir[1] = real.y; sky.sun_dir[2] = real.z;
    sky.fov_tan = tanf(cam.fovy * 0.00872664f);
    sky.aspect  = (float)window_get_width() / (float)window_get_height();
    sky.horizon_col[0] = 1.f;   // unexposed: the HDR chain exposes
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&pipe);
    SDL_GPUTextureSamplerBinding tsb{ SkyPanorama::Get().Tex(), SkyPanorama::Get().Sampler() };
    pv.BindFragmentSamplers(0, &tsb, 1);
    pv.PushVertexUniforms(0, &sky, sizeof(sky));
    pv.PushFragmentUniforms(0, &sky, sizeof(sky));
    pv.Draw(3, 1, 0, 0);
}

void NpcRender::DrawNpcForwardAndDissolve(FrameCtx& fc, const MdCamera& cam, SDL_GPURenderPass* rp)
{
    if (!pipeline.SDLPipeline()) return;   // no NPC shader in this build (npc_instanced.frag is not public yet): the NPCs are skipped, the world is drawn
    GpuPassView pv = GpuPassView::FromRaw(rp, fc.cmd);
    pv.BindPipeline(&pipeline);

    // Vertex buffer: combined SkinVertex + pre-baked body color, slot=0.
    pv.BindVertexBuffer(&body_color_buf);

    // Index buffer.
    pv.BindIndexBuffer(&ibuf, idx_elem);

    // Vertex storage buffers (set=0): Transform Visible Faction Bones Skin.
    SDL_GPUBuffer* v_storage[5] = {
        TransformSoA::Get().SDLTransformBuffer(),
        vis_buf.SDLBuffer(),
        TransformSoA::Get().SDLFactionBuffer(),
        AnimationSoA::Get().SDLBonesBuffer(),
        TransformSoA::Get().SDLSkinBuffer()
    };
    pv.BindVertexStorageBuffers(0, v_storage, 5);

    // Vertex UBO (set=1, slot 0): viewProj + factionColors.
    float r_asp2 = (float)window_get_width() / (float)window_get_height();
    Mat4 vp2 = cam.ViewProjMatrix(r_asp2);
    struct { float vp[16]; float fc[16]; } v_ubo;
    memcpy(v_ubo.vp, mat4_ptr(vp2), 64);
    static const float kFC[16] = {
        0,0,0,1, 0.86f,0.2f,0.2f,1, 0.2f,0.59f,0.86f,1, 0.86f,0.78f,0.2f,1
    };
    memcpy(v_ubo.fc, kFC, 64);
    pv.PushVertexUniforms(0, &v_ubo, sizeof(v_ubo));

    // Fragment UBO (set=3, slot 0): LightUniforms.
    NpcFragUBO f_ubo = {};
    const auto& ls = LightSystem::Get();
    float cp2[3]; cam.CamPosToArr(cp2);
    const auto& gs2 = GraphicsSettings::Get();
    const auto& ss  = ShadowSystem::Get();
    f_ubo.sunDir[0] = -ls.sun_dir.x; f_ubo.sunDir[1] = -ls.sun_dir.y;
    f_ubo.sunDir[2] = -ls.sun_dir.z;
    memcpy(f_ubo.sunColor,    &ls.sun_color,     12);
    memcpy(f_ubo.ambientColor, &ls.ambient_color, 12);
    memcpy(f_ubo.cameraPos, cp2, 12);
    memcpy(f_ubo.fogColor,  gs2.fog_color, 12);
    f_ubo.fogNear = gs2.fog_near; f_ubo.fogFar = gs2.fog_far;
    for (int k = 0; k < 3; ++k)
        memcpy(f_ubo.lightViewProj[k], mat4_ptr(ss.lightViewProj[k]), 64);
    f_ubo.cascadeSplits[0] = ss.cascade_splits[0];
    f_ubo.cascadeSplits[1] = ss.cascade_splits[1];
    f_ubo.cascadeSplits[2] = ss.cascade_splits[2];
    // VBfA-R3: fill 6-sample light probe
    ls.ComputeProbe(reinterpret_cast<float*>(f_ubo.probe));
    // VBfA-R6: snow level from current zone biome
    {
        const char* biome = "";
        const ZoneRecord* zr = WorldRegistry::Get().Find(
            WorldRegistry::Get().CurrentZone());
        if (zr) biome = zr->biome;
        float snow = 0.f;
        if (!strncmp(biome, "snow", 4) || !strncmp(biome, "ice", 3))
            snow = 1.0f;
        else if (!strncmp(biome, "highlands", 9))
            snow = 0.15f;
        f_ubo.snow_level = snow;
    }
    // VBfA-R7: shadow fade defaults
    f_ubo.shadow_dist_scalar   = 1.f / 120.f;  // fade at 120m
    f_ubo.shadow_height_scalar = 0.f;           // height fade off (flat world)
    f_ubo.shadow_height_offset = 1.f;           // always 1.0
    pv.PushFragmentUniforms(0, &f_ubo, sizeof(f_ubo));

    // CPU-driven, not indirect, since 2026-08-09 -- t01_count_/t2_count_ were
    // computed this same frame by CullAndPrepast (runs earlier in RenderFrame),
    // no GPU round-trip needed.
    pv.DrawIndexed(index_count, t01_count_, 0, 0, 0);
    pv.DrawIndexed(index_count, t2_count_, 0, 0, (uint32_t)MAX_ANIMATED_NPC);

    // VBfA-R9: dissolve pass — dead NPCs rendered with animated_dissolve.frag.
    // Dead NPCs are excluded from the main indirect draw (cull.comp dead_mask).
    // Each dissolving NPC gets its own DrawGPUIndexedPrimitives with a per-NPC
    // DissolveUBO pushed as a fragment uniform.
    if (dissolve_pipeline.SDLPipeline() && n_dissolving > 0 &&
        dissolve_vis_buf.SDLBuffer()) {
        pv.BindPipeline(&dissolve_pipeline);
        // Vertex buffer: combined SkinVertex + body colors, slot=0.
        pv.BindVertexBuffer(&body_color_buf);
        // Slot 1 below: dissolve_vis_buf instead of vis_buf — dead NPC slots.
        SDL_GPUBuffer* d_st[5] = {
            TransformSoA::Get().SDLTransformBuffer(),
            dissolve_vis_buf.SDLBuffer(),
            TransformSoA::Get().SDLFactionBuffer(),
            AnimationSoA::Get().SDLBonesBuffer(),
            TransformSoA::Get().SDLSkinBuffer()
        };
        pv.BindVertexStorageBuffers(0, d_st, 5);
        // Reuse vertex UBO (same viewProj + faction colors)
        pv.PushVertexUniforms(0, &v_ubo, sizeof(v_ubo));
        for (int di = 0; di < n_dissolving; ++di) {
            struct { float dissolve_t; float edge_width; float _p[2]; } d_ubo;
            d_ubo.dissolve_t = dissolve_prog[di];
            d_ubo.edge_width = 0.08f;
            d_ubo._p[0] = d_ubo._p[1] = 0.f;
            pv.PushFragmentUniforms(0, &d_ubo, sizeof(d_ubo));
            // firstInstance=di → gl_InstanceIndex=di → dissolve_vis_buf[di]=slot
            pv.DrawIndexed(index_count, 1, 0, 0, (uint32_t)di);
        }
    }
}

void NpcRender::DrawTerrainAndProps(FrameCtx& fc, const MdCamera& cam, float now_s, SDL_GPURenderPass* rp)
{
    float r_asp2 = (float)window_get_width() / (float)window_get_height();
    // 2026-09-19 (docs/RESOLVE_OPT.md session finding, owner-caught): this
    // used to be TerrainRenderer::SunParams::Default() -- a frozen constant,
    // completely disconnected from LightSystem::Get(), which DOES animate a
    // real day/night cycle every frame (LightSystem::Tick(gs.game_time_hours)
    // in game_render_frame.cpp, driven by logic_tick_orchestration.cpp's
    // real elapsed-time clock -- confirmed live, not assumed). NPC lighting
    // (this file's own DrawNpcsForward above, f_ubo.sunDir/sunColor/
    // ambientColor) already tracked the live sun; terrain silently didn't.
    // Same -ls.sun_dir negation as that call site (ls.sun_dir is "sun->
    // surface", terrain's ndl=dot(N,uf.sun_dir_str.xyz) needs "surface->
    // sun" to go positive when a slope faces the sun).
    const auto& ls_sun = LightSystem::Get();
    TerrainRenderer::SunParams sun{};
    sun.dir[0] = -ls_sun.sun_dir.x;
    sun.dir[1] = -ls_sun.sun_dir.y;
    sun.dir[2] = -ls_sun.sun_dir.z;
    // ProjFragUBO's sun_dir_str.w is a single scalar (no per-channel direct-
    // light tint slot, unlike ambient/AmbientProbe below) -- approximate
    // ls_sun.sun_color's day/dawn/night intensity+tint as its mean channel.
    // Loses the warm dawn/dusk tint on the DIRECT term specifically; the
    // ambient term right below keeps full RGB via AmbientProbeSystem.
    sun.strength = (ls_sun.sun_color.x + ls_sun.sun_color.y + ls_sun.sun_color.z) / 3.f;
    sun.ambient[0] = ls_sun.ambient_color.x;
    sun.ambient[1] = ls_sun.ambient_color.y;
    sun.ambient[2] = ls_sun.ambient_color.z;
    sun._pad = 0.f;
    // AmbientProbeSystem per-frame update + upload moved to the top of DrawScene (before the scene pass opens): the
    // probe is now uploaded as a copy pass inside the frame's own command buffer (UploadInCmd) instead of 3 synchronous
    // submits (docs/FRAME_AUDIT_2026-09-27.md #5).
    // L2-style TerrainCR / MeshLOD CR — from RenderQualityConfig
    auto& sr = SceneRender::Get();
    // Kenshi colour map (md_terrain.png) UV: world position -> texture
    // position. md_terrain.png (tools/md_stitch_terrain.py) is an 8x8
    // stitch of Kenshi's own colour.X.Y.png overlay tiles — the WHOLE
    // 64-zone world in one 4096x4096 image, no repeat/tiling intended.
    // Was a hardcoded (500, 250, 1/8000) snapshot tuned by eye under the
    // OLD, unverified 500m/zone assumption (32000m world) — the 1/8000
    // scale meant REPEAT-wrapping the whole map every 8km of world space
    // (not a "crop", despite the old comment), so it showed an arbitrary
    // phase of a repeating pattern rather than the real local colour;
    // that phase drifted (visible seam/crack artefacts, confirmed present
    // since earlier this session's CHUNK_SIZE fix) once CHUNK_SIZE was
    // corrected to 460.8m/zone. Fixed: real 1:1 world scale, no repeat —
    // geographically correct, ~7.2m/texel up close (coarser than the old
    // zoomed-in hack, by design/user choice).
    // Recentres colour-overlay sampling on the world's true absolute centre
    // (md_terrain.png is a whole-world, 1:1-scale, no-repeat image) — see
    // GraniteAbsCam's doc comment (scene_render.cpp) for why Granite's own
    // draw below needs an absolute-space camera/origin convention instead
    // of this file's other draws' LOCAL tnoff_x/z-anchored space.

    // Granite terrain migration, Phase 8: THE terrain draw path — draws
    // whatever SceneRender::UpdateGraniteTerrain already uploaded this
    // frame (called earlier in RenderFrame, before this render pass
    // opened). Uses its OWN view-proj (an absolute-space camera copy,
    // GraniteAbsCam) since terrain_granite_grid/hmap use the absolute
    // [0,extent) convention, not vp2's LOCAL tnoff_x/z-anchored space —
    // overlay-colour sampling origin is kWorldCenter directly for the
    // same reason. Replaces the old per-chunk TNKN=9 loop and, later,
    // TerrainQuadtreeRenderer/TerrainQuadtree (both removed this phase
    // after game+editor confirmed no regression) that used to draw here.
    if (sr.terrain_granite_ready) {
        MdCamera g_abs_cam = sr.GraniteAbsCam(cam);
        Mat4 g_vp = g_abs_cam.ViewProjMatrix(r_asp2);
        // task terrain-perf-rebuild Phase 0 (2026-08-01): CPU-side submission
        // time only (SDL3 GPU exposes no timestamp-query API as of this SDK
        // version -- confirmed via grep of SDL_gpu.h, no SDL_GPUQuery/
        // timestamp symbols at all -- so this can't capture actual GPU
        // execution time, only how long the CPU spends recording these draw
        // calls). Real GPU-side terrain cost still needs RenderDoc per
        // TERRAIN_PERF_REBUILD_PROMPT.md's Fáze 0 fallback.
        FS_BEGIN("Terrain");
        {
        // TERRAIN_CA_REBUILD_PROMPT.md Phase 4 -- Variant A: the G-buffer
        // pass (CullAndPrepass's Early-Z block) already rasterized this
        // frame's terrain geometry; here we just resolve it with one
        // fullscreen draw. This is the DEFAULT terrain shading path this
        // codebase runs (see TerrainShadingProjected field's doc comment,
        // scene_render.h, for what alternates were measured/removed, and
        // alternate was removed in #185 K4c).
        // terrain-perf-measure falsification probe (2026-08-12): repeat >1
        // only under md.set_terrain_resolve_repeat(n) -- see scene_render.h
        // field doc comment. Later iterations just overwrite the same
        // pixels (depth_test LESS_OR_EQUAL against itself), no visual
        // change, pure GPU-work multiplier for the resolution-scaling test.
        // 0 is a real ablation value (2026-08-13): skips DrawShadingResolve
        // entirely so gpu_ms(0) vs gpu_ms(1) gives the resolve pass's direct
        // frame contribution, not its cost multiplied through a serialized
        // N-deep chain into the same render target (which N>=2 systematically
        // overestimates -- consecutive fullscreen draws into one target are
        // dependent, can't overlap with other GPU work the way a single real
        // pass does in a normal frame). Screen will show whatever the
        // G-buffer pass left in the color target where terrain would be --
        // fine for a one-off ablation measurement, not a rendering mode.
        // КРОК 7 (docs/KROK7_SKYVIS_AO_DESIGN.md): probe-grid update, a
        // compute dispatch (doesn't need to be inside `rp`) -- run once per
        // frame here since this branch is the only one with a real G-buffer
        // world-position source for ApplyPass (npc_render_deferred.cpp) to
        // read later. No-op (IsEnabled()==false) if Init() didn't succeed.
        if (md::RenderPassGraph::Get().IsEnabled("skyvis_ao")) {
            sr.terrain_skyvis_ao_.UpdatePass(fc.cmd,
                sr.granite_abs_cam_[0], sr.granite_abs_cam_[2],
                sr.terrain_granite_hmap.Texture(), sr.terrain_granite_hmap.Sampler(),
                sr.terrain_granite_hmap.WorldExtent(),
                sr.terrain_granite_hmap.HeightMin(), sr.terrain_granite_hmap.HeightMax(),
                sr.terrain_granite_hmap.Resolution());
        }
        }

        // TEMP DEBUG TOOL (2026-09-04, CLAUDE_CONSTITUTION.md sec 7.7):
        // real mesh wireframe overlay -- recomputes its own SelectVisible
        // (cheap, ~1-5ms measured elsewhere this session) rather than
        // threading the G-buffer pass's node list across translation units,
        // since this is a live debug toggle, not a per-frame-critical path.
        if (sr.terrain_wireframe_debug_ && sr.terrain_quadtree_renderer_.IsWireframeReady()) {
            static TerrainQuadtree::VisibleNode wf_nodes[TerrainQuadtree::kMaxNodesPublic];
            int wf_count = sr.terrain_quadtree_.SelectVisible(
                sr.granite_abs_cam_, sr.terrain_frustum_planes_,
                wf_nodes, TerrainQuadtree::kMaxNodesPublic,
                TerrainQuadtree::kGameplayMaxRenderDistance);
            for (int i = 0; i < wf_count; ++i) {
                sr.terrain_quadtree_renderer_.DrawNodeWireframe(
                    rp, fc.cmd, sr.terrain_granite_hmap, mat4_ptr(g_vp),
                    wf_nodes[i],
                    sr.granite_abs_cam_[0], sr.granite_abs_cam_[1], sr.granite_abs_cam_[2]);
            }
        }
        FS_END("Terrain");

        // Real Kenshi scatter-object placement (features.dat) — replaces
        // the disabled noise-based PropGen_Build. Stored in the SAME
        // absolute Kenshi-world space as the Granite terrain (not this
        // file's other draws' local tnoff_x/z-anchored space), so it needs
        // g_abs_cam/g_vp too, not cam/vp2 — see this block's own comment
        // above for why. NOT gated by render_deco (unlike the old
        // per-instance prop pass below, which that flag exists to skip on
        // weak hardware) — this is real, sparse, hand-authored content
        // (757 objects world-wide), not thousands of noise-generated
        // filler instances; skipping it on Forward tier would mean Intel
        // HD 520 users (this project's primary target) never see any real
        // scatter objects at all. 300m cutoff matches ground-detail scale,
        // not distant landmarks.
        FeatureScatterSystem::Get().Draw(rp, fc.cmd, mat4_ptr(g_vp), (const float*)&sun,
                                          g_abs_cam.pos.x, g_abs_cam.pos.z, 300.0f);
        for (int hs = 0; hs < HubSpike::SLOTS; ++hs) if (HubSpike::Slot(hs).Active()) HubSpike::Slot(hs).Draw(rp, fc.cmd, mat4_ptr(g_vp), (const float*)&sun, g_abs_cam.pos.x, g_abs_cam.pos.z, sr.terrain_frustum_planes_, g_abs_cam.pos.y);
        TownBuildings::Get().Draw(rp, fc.cmd, mat4_ptr(g_vp), (const float*)&sun, g_abs_cam.pos.x, g_abs_cam.pos.z, sr.terrain_frustum_planes_);
        TownFoliage::Get().Draw(rp, fc.cmd, mat4_ptr(g_vp), (const float*)&sun, g_abs_cam.pos.x, g_abs_cam.pos.z, sr.terrain_frustum_planes_);
    }
}

void NpcRender::DrawWaterPlane(FrameCtx& fc, const MdCamera& cam, float now_s, bool water_any, SDL_GPURenderPass* rp)
{
    // -- G-3: Water plane --
    // WATER_Y (water.slang) is a hardcoded world height (0.2m) for a
    // 6000x6000m quad that always follows the camera's XZ position.
    // The CPU gate used to check ground height ONLY at the camera's own
    // XZ position (task #158b/7a7f1079) -- fine for deciding whether to
    // draw at all, but the flat quad itself still had zero per-pixel
    // awareness of real terrain: over half the loaded chunk grid
    // (TNKN=9, ~2073m half-extent) is smaller than the water quad's
    // world_sz=3000m half-extent, so it could extend past streamed
    // terrain (unoccluded, no depth written) or across genuinely dry
    // ground within its own radius that the single camera-position check
    // never saw (task #158i-4/9).
    //
    // Real fix (data prepared above, before this render pass opened --
    // SSBO::UploadInCmd needs its own copy pass, which SDL_GPU disallows
    // nesting inside an already-active render pass): water_any / the
    // water_mask_ssbo hold a kWaterMaskN grid of real TerrainQuery::
    // GetHeight samples across the exact square the quad covers;
    // water.slang bilinear-samples + fades per pixel against it instead
    // of drawing the whole plane unconditionally.
    if (!SceneRender::Get().water_pipeline.SDLPipeline() || !water_any) return;
    float r_asp2 = (float)window_get_width() / (float)window_get_height();
    Mat4 vp2 = cam.ViewProjMatrix(r_asp2);
    using WVU = SceneRender::WaterVertUBO;
    using WFU = SceneRender::WaterFragUBO;
    const auto& wls = LightSystem::Get();
    WVU wv;
    memcpy(wv.vp, mat4_ptr(vp2), 64);
    wv.cam_x    = cam.pos.x;
    wv.cam_z    = cam.pos.z;
    wv.world_sz = SceneRender::kWaterWorldSz;
    wv.time     = now_s;
    wv.water_y  = 0.2f; wv.pad0 = wv.pad1 = wv.pad2 = 0.f;
    const bool kw = g_kenshi_water && (SceneRender::Get().water_kenshi_pipeline.SDLPipeline() || WaterLiveReady()) && KenshiWaterTexturesReady();   // release: the live water stands without the translated pipeline
    if (kw) wv.water_y = kKenshiWaterY;
    WFU wf;
    wf.sun_dir[0] = wls.sun_dir.x;
    wf.sun_dir[1] = wls.sun_dir.y;
    wf.sun_dir[2] = wls.sun_dir.z;
    wf.sun_dir[3] = 0.f;
    wf.cam_pos[0] = cam.pos.x;
    wf.cam_pos[1] = cam.pos.y;
    wf.cam_pos[2] = cam.pos.z;
    wf.cam_pos[3] = 0.f;
    wf.time    = now_s;
    wf.opacity = 0.78f;
    wf.pad0 = wf.pad1 = 0.f;
    GpuPassView pv = GpuPassView::FromRaw(rp, fc.cmd);
    if (kw) {   // #173: Kenshi water; storage buffer binding 3 follows the 3 samplers
        SceneRender::WaterKenshiFragUBO kf;
        kf.sun_dir[0] = -wls.sun_dir.x; kf.sun_dir[1] = -wls.sun_dir.y; kf.sun_dir[2] = -wls.sun_dir.z; kf.sun_dir[3] = (float)SceneRender::Get().zone_ox * CHUNK_SIZE - SceneRender::Get().tnoff_x;
        kf.cam_pos[0] = cam.pos.x; kf.cam_pos[1] = cam.pos.y; kf.cam_pos[2] = cam.pos.z; kf.cam_pos[3] = (float)SceneRender::Get().zone_oz * CHUNK_SIZE - SceneRender::Get().tnoff_z;
        kf.sun_col[0] = wls.sun_color.x; kf.sun_col[1] = wls.sun_color.y; kf.sun_col[2] = wls.sun_color.z; kf.sun_col[3] = now_s;
        kf.amb[0] = wls.ambient_color.x; kf.amb[1] = wls.ambient_color.y; kf.amb[2] = wls.ambient_color.z; kf.amb[3] = 0.01f;   // invOpacity of the material
        if (WaterLiveReady()) {   // MD_WATER_LIVE=1: the original program draws the plane (same inputs as the translated path)
            md::WaterLiveFrame lf;
            memcpy(lf.vp, mat4_ptr(vp2), 64);
            lf.cam[0] = cam.pos.x; lf.cam[1] = cam.pos.y; lf.cam[2] = cam.pos.z;
            lf.abs_x = kf.sun_dir[3]; lf.abs_z = kf.cam_pos[3];
            lf.to_sun[0] = kf.sun_dir[0]; lf.to_sun[1] = kf.sun_dir[1]; lf.to_sun[2] = kf.sun_dir[2];
            for (int i = 0; i < 3; ++i) { lf.sun_colour[i] = kf.sun_col[i]; lf.ambient[i] = kf.amb[i]; }
            lf.time_s = now_s; lf.water_y = kKenshiWaterY; lf.half_size = SceneRender::kWaterWorldSz;
            lf.normal = s_kw_normal.SDLTexture(); lf.normal_smp = s_kw_normal.SDLSampler(); lf.flow = s_kw_flow.SDLTexture(); lf.flow_smp = s_kw_flow.SDLSampler();
            lf.colour = s_kw_colour.SDLTexture(); lf.colour_smp = s_kw_colour.SDLSampler();
            s_water_live.Draw(rp, fc.cmd, lf);
            return;
        }
        pv.BindPipeline(&SceneRender::Get().water_kenshi_pipeline);
        pv.PushVertexUniforms(0, &wv, sizeof(wv));
        pv.PushFragmentUniforms(0, &kf, sizeof(kf));
        SDL_GPUTextureSamplerBinding tsb[3] = { { s_kw_normal.SDLTexture(), s_kw_normal.SDLSampler() }, { s_kw_flow.SDLTexture(), s_kw_flow.SDLSampler() }, { s_kw_colour.SDLTexture(), s_kw_colour.SDLSampler() } };
        pv.BindFragmentSamplers(0, tsb, 3);
        if (SDL_GPUBuffer* wbuf = SceneRender::Get().water_mask_ssbo.SDLBuffer()) pv.BindFragmentStorageBuffers(0, &wbuf, 1);
        pv.Draw(6, 1, 0, 0);
        return;
    }
    pv.BindPipeline(&SceneRender::Get().water_pipeline);
    pv.PushVertexUniforms(0, &wv, sizeof(wv));
    pv.PushFragmentUniforms(0, &wf, sizeof(wf));
    if (SDL_GPUBuffer* wbuf = SceneRender::Get().water_mask_ssbo.SDLBuffer())
        pv.BindFragmentStorageBuffers(0, &wbuf, 1);
    pv.Draw(6, 1, 0, 0);
}

void NpcRender::DrawHudOverlays(FrameCtx& fc, const MdCamera& cam, float now_s,
                                 MdEntity player_entity, float& cam_x, float& cam_z,
                                 float cam_az, SDL_GPURenderPass* rp)
{
    float r_asp2 = (float)window_get_width() / (float)window_get_height();
    Mat4 vp2 = cam.ViewProjMatrix(r_asp2);

    // C-2: Player renders as kenshi_human via view<WorldTransform,NavAgent>
    // above — solid sphere placeholder removed.

    // -- Particles --
    ParticleRenderer::Get().DrawSDLGPU(rp, fc.cmd, fc.p_count, vp2,
                                       cam.pos);

    if (!UpscaleActive())
        DrawScreenOverlays(fc, now_s, player_entity, cam_x, cam_z, cam_az, rp);
}

// 2D screen-space overlays (world map, camera recording button). Split out of DrawHudOverlays for resolution_scale
// (#253): with the upscale active they are drawn by DrawFullResUi() onto the REAL swapchain in full resolution after
// the upscale blit, otherwise here in the scene pass as before. (Particles / selection ring / click marker are 3D and
// stay in DrawHudOverlays.)
void NpcRender::DrawScreenOverlays(FrameCtx& fc, float now_s, MdEntity player_entity,
                                   float& cam_x, float& cam_z, float cam_az, SDL_GPURenderPass* rp)
{
    GpuPassView pv = GpuPassView::FromRaw(rp, fc.cmd);

    // -- World Map overlay (M key) --
    if (WorldMapUI::Get().IsOpen()) {
        int sw2 = window_get_width(), sh2 = window_get_height();
        float wmx = input_mouse_x(), wmy = input_mouse_y();
        bool lmb_now = input_mouse_down(MOUSE_BUTTON_LEFT);
        static bool s_wmap_prev_lmb = false;
        bool lmb_click2 = lmb_now && !s_wmap_prev_lmb;
        s_wmap_prev_lmb = lmb_now;
        WorldMapUI::Get().Update(wmx, wmy, sw2, sh2, lmb_click2);
        WorldMapUI::Get().SetPlayerAngle(cam_az);
        // cam_x/cam_z are always current (updated by HandlePlayerMove every frame).
        // UV: zone float coord / 63 → [0,1] over the full 64×64 Kenshi world.
        {
            float uv_x = ((float)SceneRender::Get().zone_ox + (cam_x - SceneRender::Get().tnoff_x) / CHUNK_SIZE) / 63.f;
            float uv_z = ((float)SceneRender::Get().zone_oz + (cam_z - SceneRender::Get().tnoff_z) / CHUNK_SIZE) / 63.f;
            WorldMapUI::Get().SetPlayerMapUV(uv_x, uv_z);
        }
        WorldMapUI::Get().Draw(rp, fc.cmd, sw2, sh2, wmx, wmy);
        // Check pending travel
        if (const char* dest = WorldMapUI::Get().PendingTravel()) {
            // Copy slug before ClearPendingTravel invalidates the pointer
            char dest_copy[48];
            strncpy(dest_copy, dest, sizeof(dest_copy)-1);
            dest_copy[sizeof(dest_copy)-1] = '\0';
            WorldMapUI::Get().ClearPendingTravel();
            auto& ptr2 = MdRegistry::Get().Handle(player_entity).get_mut<WorldTransform>();
            ZoneRecord* zr = WorldRegistry::Get().Find(dest_copy);
            float nx = zr ? zr->exit_x : 0.f;
            float nz = zr ? zr->exit_z : 0.f;
            TransitionToZone(dest_copy, ptr2.x, ptr2.z);
            ptr2.x = nx; ptr2.z = nz;
            cam_x = nx; cam_z = nz;
        }
    }

    // -- Camera recording button (bottom-right overlay) --
    if (SceneRender::Get().overlay_pipeline.SDLPipeline() && SceneRender::Get().cam_idle_tex.SDLTexture() && SceneRender::Get().cam_sampler.SDLSampler()) {
        md::GpuTextureHandle btn_tex = SceneRender::Get().cam_idle_tex.SDLTexture();
        if (SceneRender::Get().recording)
            btn_tex = (((uint32_t)(now_s * 1000.f) / 500u) & 1u)
                      ? SceneRender::Get().cam_idle_tex.SDLTexture() : SceneRender::Get().cam_rec_tex.SDLTexture();
        int sw = window_get_width(), sh = window_get_height();
        float bx = (float)(sw - SceneRender::CAM_BTN_SZ - SceneRender::CAM_BTN_MARGIN) / (float)sw;
        float by = (float)(sh - SceneRender::CAM_BTN_SZ - SceneRender::CAM_BTN_MARGIN) / (float)sh;
        float bw = (float)SceneRender::CAM_BTN_SZ / (float)sw;
        float bh = (float)SceneRender::CAM_BTN_SZ / (float)sh;
        struct { float x, y, w, h; } orect = { bx, by, bw, bh };
        pv.BindPipeline(&SceneRender::Get().overlay_pipeline);
        SDL_GPUTextureSamplerBinding tsb{ btn_tex, SceneRender::Get().cam_sampler.SDLSampler() };
        pv.BindFragmentSamplers(0, &tsb, 1);
        pv.PushVertexUniforms(0, &orect, sizeof(orect));
        pv.Draw(6, 1, 0, 0);
    }
}

void NpcRender::DrawPlayerCloth(FrameCtx& fc, const MdCamera& cam, SDL_GPURenderPass* rp)
{
    if (!(cloth_visible && ClothDrawReady() && rp)) return;
    // Find player slot: entity with PlayerController + WorldTransform
    uint32_t player_slot = 0xFFFFFFFFu;
    static auto q_p3_npc_render_5 = MdRegistry::Get().Raw().query().all<WorldTransform>().all<PlayerController>();
    MdEach(q_p3_npc_render_5, [&](const WorldTransform& tr, const PlayerController&){
            if (tr.slot != TransformSoA::INVALID_SLOT)
                player_slot = (uint32_t)tr.slot;
        });

    if (player_slot != 0xFFFFFFFFu) {
        Mat4 vp_cloth = cam.ViewProjMatrix(
            (float)window_get_width()/(float)window_get_height());
        struct CU { float vp[16]; uint32_t playerSlot; float _p[3]; } cu;
        memcpy(cu.vp, mat4_ptr(vp_cloth), 64);
        cu.playerSlot = player_slot; cu._p[0]=cu._p[1]=cu._p[2]=0;

        struct ClothFU { float color[3]; float pad; } cfu;

        GpuPassView pv = GpuPassView::FromRaw(rp, fc.cmd);
        pv.BindPipeline(&s_cloth_game_pipeline);
        SDL_GPUBuffer* bones_buf = AnimationSoA::Get().SDLBonesBuffer();
        pv.BindVertexStorageBuffers(0, &bones_buf, 1);
        pv.PushVertexUniforms(0, &cu, sizeof(cu));

        for (int sl = 0; sl < 3; ++sl) {
            auto& cs = s_player_cloth[sl];
            if (!cs.active || !cs.vbo.SDLBuffer() || cs.ni==0) continue;
            cfu.color[0]=cs.color[0]; cfu.color[1]=cs.color[1];
            cfu.color[2]=cs.color[2]; cfu.pad=0;
            pv.PushFragmentUniforms(0, &cfu, sizeof(cfu));
            pv.BindVertexBuffer(&cs.vbo);
            pv.BindIndexBuffer(&cs.ibo, SDL_GPU_INDEXELEMENTSIZE_16BIT);
            pv.DrawIndexed((uint32_t)cs.ni, 1, 0, 0, 0);
        }
    }
}

void NpcRender::DrawPlayerHair(FrameCtx& fc, const MdCamera& cam, SDL_GPURenderPass* rp)
{
    if (!(s_player_hair.loaded && s_hair_game_pipeline.SDLPipeline() &&
          AnimationSoA::Get().SDLBonesBuffer() && rp)) return;
    uint32_t player_slot = 0xFFFFFFFFu;
    static auto q_p3_npc_render_6 = MdRegistry::Get().Raw().query().all<WorldTransform>().all<PlayerController>();
    MdEach(q_p3_npc_render_6, [&](const WorldTransform& tr, const PlayerController&){
            if (tr.slot != TransformSoA::INVALID_SLOT)
                player_slot = (uint32_t)tr.slot;
        });
    if (player_slot == 0xFFFFFFFFu) return;

    auto& mesh = s_player_hair.mesh;
    if (!mesh.vbo.SDLBuffer() || mesh.index_count == 0) return;

    Mat4 vp_hair = cam.ViewProjMatrix(
        (float)window_get_width()/(float)window_get_height());
    struct CU { float vp[16]; uint32_t playerSlot; float _p[3]; } cu;
    memcpy(cu.vp, mat4_ptr(vp_hair), 64);
    cu.playerSlot = player_slot; cu._p[0]=cu._p[1]=cu._p[2]=0;

    struct HairFU { float color[3]; float pad; } hfu;
    hfu.color[0]=s_player_hair.color[0];
    hfu.color[1]=s_player_hair.color[1];
    hfu.color[2]=s_player_hair.color[2];
    hfu.pad=0;

    GpuPassView pv = GpuPassView::FromRaw(rp, fc.cmd);
    pv.BindPipeline(&s_hair_game_pipeline);
    SDL_GPUBuffer* bones_buf = AnimationSoA::Get().SDLBonesBuffer();
    pv.BindVertexStorageBuffers(0, &bones_buf, 1);
    pv.PushVertexUniforms(0, &cu, sizeof(cu));
    pv.PushFragmentUniforms(0, &hfu, sizeof(hfu));

    pv.BindVertexBuffer(&mesh.vbo);
    pv.BindIndexBuffer(&mesh.ibo, mesh.indices_u16
        ? SDL_GPU_INDEXELEMENTSIZE_16BIT : SDL_GPU_INDEXELEMENTSIZE_32BIT);
    pv.DrawIndexed(mesh.index_count, 1, 0, 0, 0);
}



// #207 PVT: the CPU ground height at an absolute position (the convention of md.terrain_height)
static bool PvtGroundHeight(float x, float z, float& h) {
    auto& sr = SceneRender::Get();
    return TerrainQuery::Get().GetHeight(x + sr.tnoff_x - (float)sr.zone_ox * CHUNK_SIZE, z + sr.tnoff_z - (float)sr.zone_oz * CHUNK_SIZE, h);
}

// #140 L2 / #207 path 1: the main light parameters of the ground (the translated TS_KenshiLight and the original main_fs of the live light pass read the same sun); null = lighting mode off
static const TerrainShadingProjected::KLightParams* UpdateKLight()
{
    auto& sr = SceneRender::Get(); (void)sr;
    // #140 L2: Kenshi's sun (kenshi_sky.h at the hour of the last LightSystem tick) and the fixed exposure 0.55 / lerp(MIN, MAX, saturate(5 sun.y)) (Okran sky.rs terrain_light)
    static TerrainShadingProjected::KLightParams kl;
    if (g_kenshi_lighting_mode > 0) {
        const md::KenshiSkySettings sky;
        const auto ml = sky.GetMainLight(LightSystem::Get().current_hours);
        const float day = std::min(std::max(ml.dir.y * 5.f, 0.f), 1.f);
        const float lum = ml.min_lum + (sky.exposure_max - ml.min_lum) * day;
        kl.sun_dir[0] = ml.dir.x; kl.sun_dir[1] = ml.dir.y; kl.sun_dir[2] = ml.dir.z; kl.sun_dir[3] = 0.f;
        kl.sun_colour[0] = ml.colour.x; kl.sun_colour[1] = ml.colour.y; kl.sun_colour[2] = ml.colour.z; kl.sun_colour[3] = ml.w;
        kl.params[0] = 1.f; kl.params[1] = 1.f; kl.params[2] = SceneRender::Get().lum_adapt_active_ ? 1.f : 0.55f / lum; kl.params[3] = g_kenshi_lighting_mode >= 3 ? 1.f : 0.f;   // L4: the ground target holds the colour before the exposure; mode >= 3 = + the Kenshi fog (L3)
        const md::KsVec3 real = sky.SunDirection(LightSystem::Get().current_hours);
        const md::KsVec4 hz = sky.HorizonClouds(LightSystem::Get().current_hours), fp = sky.FogParams();
        kl.sun_real[0] = real.x; kl.sun_real[1] = real.y; kl.sun_real[2] = real.z; kl.sun_real[3] = 0.f;
        kl.horizon[0] = hz.x; kl.horizon[1] = hz.y; kl.horizon[2] = hz.z; kl.horizon[3] = hz.w;
        kl.fog[0] = fp.y; kl.fog[1] = fp.z; kl.fog[2] = fp.x * 0.95f; kl.fog[3] = 4000.f;   // farClip of the live pass in metres (GroundLiveDriver far_clip_ 40000 units x 0.1); the tiny live consumer turns liveB.a into a distance; envColour.w = 1, ambientParams.w = 1 (common.program defaults)
        kl.sun_real[3] = md::GroundLiveDriver::OrigLight() ? 1.f : 0.f;   // 1: the ground resolve reads the lit colour of the original main_fs (liveA) and only adds fog and exposure
        return &kl;
    }
    return nullptr;
}

// #163 5b-2: arguments of the ground resolve as DrawTerrainAndProps always built them (same light, same fog, same absolute-space origin).
void NpcRender::DrawGroundResolve(FrameCtx& fc, const MdCamera& cam, SDL_GPURenderPass* rp)
{
    (void)cam;
    const auto& ls_sun = LightSystem::Get();
    TerrainRenderer::SunParams sun{};
    sun.dir[0] = -ls_sun.sun_dir.x; sun.dir[1] = -ls_sun.sun_dir.y; sun.dir[2] = -ls_sun.sun_dir.z;
    sun.strength = (ls_sun.sun_color.x + ls_sun.sun_color.y + ls_sun.sun_color.z) / 3.f;
    sun.ambient[0] = ls_sun.ambient_color.x; sun.ambient[1] = ls_sun.ambient_color.y; sun.ambient[2] = ls_sun.ambient_color.z;
    sun._pad = 0.f;
    const auto& rqc = RenderQualityConfig::Get();
    auto& sr = SceneRender::Get();
    static constexpr float kWorldCenter = 64.f * CHUNK_SIZE * 0.5f;
    const float COL_UV = 1.f / (64.f * CHUNK_SIZE);
    const auto& gsq_g = GraphicsSettings::Get();
    const float g_fog_far  = rqc.terrain_cr_m;
    const float g_fog_near = gsq_g.fog_near_ratio * g_fog_far;
    sr.terrain_shading_projected_.SetKLight(UpdateKLight());
    sr.terrain_shading_projected_.SetPvtHeightFn(&PvtGroundHeight);
    if (!rp) {   // #207 PVT: called before the ground pass opens: plan, upload the page table, bake the tiles
        const auto& qt = sr.terrain_quadtree_renderer_;
        TerrainShadingProjected::PvtFine fine;
        fine.norm_tex = qt.LodFineNormTex(); fine.ox = qt.LodFineOx(); fine.oz = qt.LodFineOz(); fine.side = qt.LodFineCells(); fine.spacing = 1.8f;
        fine.ok = qt.LodFineReady() && qt.LodFineNormOk();
        sr.terrain_shading_projected_.PvtFrame(fc.cmd, sun, sr.granite_abs_cam_[0], sr.granite_abs_cam_[1], sr.granite_abs_cam_[2],
            kWorldCenter, kWorldCenter, COL_UV, g_fog_far, gsq_g.fog_color, g_fog_near, sr.terrain, fine, (uint32_t)sr.frame_counter_pvt_++, sr.lum_adapt_active_);
        return;
    }
    sr.terrain_shading_projected_.DrawShadingResolve(rp, fc.cmd, sun,
        sr.granite_abs_cam_[0], sr.granite_abs_cam_[1], sr.granite_abs_cam_[2],
        kWorldCenter, kWorldCenter, COL_UV,
        g_fog_far, gsq_g.fog_color, g_fog_near,
        sr.terrain);
}

