#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/io/data_files.h>
#include "scene_render.h"
#include "splash_screen.h"
#include <algorithm>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/ambient_probe.h>
#include <monkey_dust/render/md_shader.h>
#include <monkey_dust/render/md_texture.h>
#include <monkey_dust/world/terrain_query.h>
#include <monkey_dust/render/render_quality.h>
#include <monkey_dust/physics/physics_terrain_region.h>
#include <monkey_dust/platform/window.h>
#include <monkey_dust/world/feature_scatter.h>
#include <monkey_dust/tools/graphics_settings.h>
#include "../world/world_registry.h"
#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/render/gpu_compute.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/render_pass_graph.h>
#include "../world/world_map_ui.h"
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#endif
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include "../bugcap.h"

// ── Camera recording icon generation ────────────────────────────────────────

void SceneRender::GenCamIcon(uint8_t* p, bool rec) {
    const int W = CAM_BTN_SZ;
    auto px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x<0||x>=W||y<0||y>=W) return;
        int i=(y*W+x)*4; p[i]=r;p[i+1]=g;p[i+2]=b;p[i+3]=255;
    };
    auto rect = [&](int x,int y,int w,int h,uint8_t r,uint8_t g,uint8_t b){
        for(int py=y;py<y+h;++py) for(int px_=x;px_<x+w;++px_) px(px_,py,r,g,b);
    };
    auto circ = [&](int cx,int cy,int rad,uint8_t r,uint8_t g,uint8_t b){
        for(int py=cy-rad;py<=cy+rad;++py)
        for(int px_=cx-rad;px_<=cx+rad;++px_)
            if((px_-cx)*(px_-cx)+(py-cy)*(py-cy)<=rad*rad)
                px(px_,py,r,g,b);
    };
    rect(0,0,W,W,30,30,40);
    rect(3,3,W-6,W-6,45,45,55);
    rect(12,26,49,31,195,195,205);
    rect(22,17,18,12,195,195,205);
    circ(36,41,13,55,55,65);
    circ(36,41, 9,195,195,205);
    circ(36,41, 5,55,55,65);
    circ(36,41, 3,100,130,155);
    if (rec) {
        circ(59,12, 8,180,18,18);
        circ(59,12, 5,235,55,55);
        for(int i=0;i<W;++i){
            px(i,0,195,20,20);px(i,1,195,20,20);px(i,2,195,20,20);
            px(i,W-1,195,20,20);px(i,W-2,195,20,20);px(i,W-3,195,20,20);
            px(0,i,195,20,20);px(1,i,195,20,20);px(2,i,195,20,20);
            px(W-1,i,195,20,20);px(W-2,i,195,20,20);px(W-3,i,195,20,20);
        }
    } else {
        for(int i=0;i<W;++i){
            px(i,0,80,80,90);px(i,W-1,80,80,90);
            px(0,i,80,80,90);px(W-1,i,80,80,90);
        }
    }
}

bool SceneRender::MakeCamTex(md::GpuDeviceHandle dev, const uint8_t* pixels, GpuColorTexture& out) {
    return MdUploadSquareRGBA8ToGpuColorTexture(dev, pixels, CAM_BTN_SZ, out);
}

// ── Recording ────────────────────────────────────────────────────────────────

// The camera button records the bug video in process (bugcap.cpp, owner 19.10); it was scripts/game_capture.py before.
void SceneRender::StartRecording() {
    if (recording) return;
    recording = bugcap::Start();
}

// ── SceneRender::Shutdown ────────────────────────────────────────────────────
// Never existed before -- ShutdownGame() (game_frame_helpers.cpp) only called
// StopRecording()/WaitRecordingChild() on this class, never released any of
// its GPU resources. Every Shutdown() called below was already correct and
// already used by tools/editor's/tools/flare_demo's own separate instances
// (confirmed by grep before writing this) -- this is "wire up what already
// works", not new resource-teardown logic. Excludes sky_pipeline/
// overlay_pipeline/water_pipeline (GpuPipeline)
// and every PropRenderer/TerrainQuadtreeRenderer/TerrainShadingProjected's own
// internal GpuPipeline members deliberately -- see splash_screen.cpp's own
// comment: GpuPipeline::Create() dedupes into gpu_hal_pipeline.cpp's
// process-global s_pipe_cache with no refcounting, already fully released by
// MdPipeCache_Shutdown() (wired into GpuDevice::Shutdown()).
void SceneRender::Shutdown() {
    if (!scene_init) return;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();

    terrain.Shutdown();
    terrain_granite_hmap.Shutdown(dev);
    terrain_shading_projected_.Shutdown();
    terrain_skyvis_ao_.Shutdown();
    terrain_quadtree_renderer_.Shutdown(dev);


    water_mask_ssbo.Shutdown();
    cam_idle_tex.Shutdown();
    cam_rec_tex.Shutdown();
    cam_sampler.Shutdown();

    // Not a SceneRender member -- a separate singleton this function itself
    // Init()'d (see Init()'s "FeatureScatterSystem::Get().Init()" call) but
    // whose Shutdown() had no caller anywhere in the repo (real Kenshi
    // features.dat scatter meshes -- the dozens of distinct
    // "[PropMesh] Loaded ... features/*.glb" instances at startup, a much
    // larger mesh count than any fixed set of members would cover).
    FeatureScatterSystem::Get().Shutdown();

    scene_init = false;
}

void SceneRender::StopRecording() {
    if (!recording) return;
    bugcap::Stop();
    recording = false;
}

void SceneRender::WaitRecordingChild() {}   // no child process any more: the encoder threads are joined in bugcap::Stop

// ── SceneRender::Init ────────────────────────────────────────────────────────

void SceneRender::Init() {
    if (scene_init) return;
    scene_init = true;

    // task #142 (2026-09-16): register DrawScene's per-sub-pass names with
    // RenderPassGraph so a diagnostic script can SetEnabled(name,false) and
    // measure the real GPU-ms delta -- RenderPassGraph::IsEnabled() already
    // defaults unregistered names to true (opt-out model), so these
    // Register() calls only make SetEnabled() take effect; they change
    // nothing about normal rendering. All 7 fit under MAX_PASSES=16
    // alongside the existing deferred_ambient/ssao/
    // cull_prepass/evsm_shadow/draw_scene registrations (none of which
    // are actually Register()'d anywhere either -- this codebase's
    // RenderPassGraph was wired for IsEnabled() checks but never had a
    // real Register() call site until this one).
    md::RenderPassGraph::Get().Register("draw_scene");
    md::RenderPassGraph::Get().Register("draw_sky");
    md::RenderPassGraph::Get().Register("draw_npc_forward");
    md::RenderPassGraph::Get().Register("draw_terrain_props");
    md::RenderPassGraph::Get().Register("draw_water");
    md::RenderPassGraph::Get().Register("draw_hud");
    md::RenderPassGraph::Get().Register("draw_player_cloth");
    md::RenderPassGraph::Get().Register("draw_player_hair");
    md::RenderPassGraph::Get().Register("cull_prepass");
    md::RenderPassGraph::Get().Register("terrain_depth_copy");
    md::RenderPassGraph::Get().Register("terrain_depth_prepass", /*default_enabled=*/false);
    // S7: depth-only terrain G-buffer + world position/normal reconstructed in the resolve pass
    // (docs/TERRAIN_SURFACE_PLAN_2026-10-02.md). Off until its visual/perf gates pass.
    md::RenderPassGraph::Get().Register("terrain_s7", /*default_enabled=*/false); // S7 OFF: flickers (owner-verified 2026-10-06, twice)
    md::RenderPassGraph::Get().Register("terrain_units10", /*default_enabled=*/true); // Kenshi ground UV scale x10 + analytic mip (default ON since 2026-10-04, owner decision)
    md::RenderPassGraph::Get().Register("skyvis_ao", /*default_enabled=*/false); // OFF: Kenshi has no AO by default (compositors.cfg #SSAO), owner decision 2026-10-09

    // Startup loading-time breakdown (task #158c: user reported 7-8s blank
    // screen before terrain appears). Measured on this dev box: 81-chunk
    // build+upload (~660ms) and the two 125-layer DDS ground-texture arrays
    // (~1.4s combined, already parallel-file-read via JobSystem — see
    // InitFromDDSArray's own comment) dominate; total ~3.3s warm-cache.
    // Kept as a permanent, low-cost diagnostic (matches "[Game] Terrain:
    // N/N chunks ready" below) rather than removed, so future startup-time
    // regressions are visible without re-instrumenting.
    auto _t0 = std::chrono::steady_clock::now();
    int  _splash_inner_step = 0;
    auto _mark = [&](const char* label) {
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - _t0).count();
        fprintf(stdout, "[STARTUP-TIMING] %-28s %8.1fms\n", label, ms);
        // This function alone is ~85-90% of total startup wall-clock
        // (game_init.cpp's _gmark reserves the outer 12% for everything
        // else) -- its own checkpoints below drive the splash bar through
        // the remaining share. kInnerSteps == number of _mark(...) calls
        // in this function (BiomeRegistry load .. "Init() total" below).
        if (SplashScreen::Get().IsReady()) {
            constexpr int   kInnerSteps = 13;
            constexpr float kOuterShare = 0.12f;
            ++_splash_inner_step;
            float frac = kOuterShare + (1.f - kOuterShare) *
                         (float)_splash_inner_step / (float)kInnerSteps;
            SplashScreen::Get().SetProgress(frac);
            SplashScreen::Get().Present();
        }
    };

    // Sync zone_ox/oz from the saved world state so the terrain loads the correct
    // Kenshi zone heightmaps and the atlas UV samples the right biome colours.
    {
        ZoneRecord* nz = WorldRegistry::Get().Find(WorldRegistry::Get().CurrentZone());
        if (nz && nz->grid_x > 0) {
            zone_ox = nz->grid_x - TNKN / 2;
            zone_oz = nz->grid_z - TNKN / 2;
        } else {
            // Fresh game, no save: current_zone_ is still its "border_zone"
            // default (not a real registered zone), so the block above
            // left zone_ox/oz at whatever they were already initialised to
            // (the real spawn zone). Sync current_zone_ to match NOW, so
            // the world map's exact player marker (WorldMapUI's state==2
            // branch) is correct from frame 1 instead of only after the
            // player's first real zone-boundary crossing (HandleTerrainStreaming
            // in main.cpp does the same sync for every crossing after this).
            if (ZoneRecord* cz = WorldRegistry::Get().FindByGrid(
                    zone_ox + TNKN / 2, zone_oz + TNKN / 2))
                WorldRegistry::Get().SetCurrentZone(cz->name);
        }
    }

    // Real per-biome data (ground textures, colours) lives in a private data
    // file, not compiled into the public engine/tools repos — see
    // docs/ENGINE_AUDIT.md / TERRAIN_FIX_PROMPT.md for the rationale. Must
    // load before any TerrainGen_Build call resolves a biome.
    // ground textures: the original install (KENSHI_DIR, data/newland/land/textures), else the old mirror
    const std::string ground_tex_dir = md::KenshiDir().empty() ? std::string("tmp_/kenshi_re/terrain_textures") : md::KenshiDir() + "/data/newland/land/textures";
    BiomeRegistry::Get().LoadPreferFcs("game/data/biome_table.txt", ground_tex_dir.c_str());  // FCS first, the generated table as fallback
    _mark("BiomeRegistry load");

    TerrainAtlas_Load("game/data/terrain/world_hmap");
    _mark("TerrainAtlas_Load");

    // Ogre-quadtree (geomorph+skirts) terrain init (see terrain_granite_hmap's
    // doc comment, scene_render.h): one-time init of the static world-wide
    // heightmap + quadtree renderer -- THE game's terrain system.
    {
        terrain_granite_ready = terrain_granite_hmap.Init(md::GpuDevice::Get().SDLDevice());

        // VTF+Variant A is the ONE supported shading path (see
        // TerrainShadingProjected field's doc comment, scene_render.h,
        // for why this is the sole path and what alternates were
        // measured/removed).
        if (terrain_granite_ready) {
            terrain_granite_ready = terrain_shading_projected_.Init(
                md::GpuDevice::Get().SDLDevice(), window_get_width(), window_get_height());
        }
        // КРОК 7 (docs/KROK7_SKYVIS_AO_DESIGN.md): failure here is non-
        // fatal -- terrain_skyvis_ao_ stays !IsEnabled() and UpdatePass/
        // ApplyPass become no-ops (same tolerance SSAOSystem's own tier
        // gate already relies on), so it doesn't gate terrain_granite_ready.
        if (terrain_granite_ready) {
            terrain_skyvis_ao_.Init(md::GpuDevice::Get().SDLDevice());
        }
        // 2026-09-19 (docs/RESOLVE_OPT.md session finding): AmbientProbeSystem
        // existed since some earlier session but was never Init()'d, populated,
        // or read by any shader -- confirmed fully dead before this. Only the
        // SSBO allocation happens here (once); the actual probe (PlaceProbe/
        // SetAmbientColor/SetSkyLight/Upload) is refreshed every frame in
        // NpcRender::DrawTerrainAndProps (npc_render_draw_scene.cpp), from the
        // SAME live LightSystem sun this whole block's neighbours already use
        // for NPC lighting -- terrain's own direct sun term was frozen at
        // TerrainRenderer::SunParams::Default() until that same fix, not
        // tracking the real day/night cycle either (owner-caught, verified
        // live: LightSystem::Tick(gs.game_time_hours) runs every frame,
        // game_time_hours genuinely advances in logic_tick_orchestration.cpp).
        if (terrain_granite_ready) {
            AmbientProbeSystem::Get().Init();
        }
        // world_extent must be an exact multiple of CHUNK_SIZE -- true for
        // TerrainWorldHeightmap's real 64-zone Kenshi world (see
        // terrain_quadtree.h's own doc comment).
        //
        // #400 (2026-08-23): detail_multiplier tuned 3.0->2.0 -- randomized
        // interleaved A/B at South Hive (md.set_terrain_detail_multiplier
        // debug override) found NO measurable FPS difference (median 55.56
        // both, spread 5.6-8.8 exceeds the delta -- noise, not a real
        // effect) but a real, deterministic -23% node/draw-call count
        // (209->160, md.granite_terrain_stats().node_count) with no visual
        // regression (near/far screenshot check, no new popping/cracks).
        // 4.0 was measured strictly worse (272 nodes). See CLAUDE_STATE.md.
        if (terrain_granite_ready) {
            terrain_quadtree_.Init(0.f, 0.f, terrain_granite_hmap.WorldExtent(), CHUNK_SIZE,
                                    TerrainAtlas_SampleWorld);
            terrain_quadtree_ready_ = terrain_quadtree_renderer_.Init(md::GpuDevice::Get().SDLDevice());
            terrain_granite_ready = terrain_granite_ready && terrain_quadtree_ready_;
            // B3 (Granite terrain finding): instanced draw path for the
            // default G-buffer resolve (used by npc_render_frame_prep.cpp
            // when NOT in forward mode) -- see TerrainQuadtreeRenderer::
            // InitBatched's own doc comment for the texture-not-SSBO
            // rationale.
            terrain_quadtree_renderer_.InitBatched(md::GpuDevice::Get().SDLDevice());
            terrain_quadtree_renderer_.InitLod(md::GpuDevice::Get().SDLDevice(), terrain_granite_hmap); // #154 (md.set_terrain_lod)

            // FRAME_AUDIT_2026-09-27 follow-up (audit #2): diagnostic depth-
            // only prepass pipelines, toggled via "terrain_depth_prepass"
            // (md.set_render_pass_enabled) -- see TerrainQuadtreeRenderer::
            // InitDepthOnly's own doc comment.
            terrain_quadtree_renderer_.InitDepthOnly(md::GpuDevice::Get().SDLDevice());

            // TEMP DEBUG TOOL (2026-09-04, CLAUDE_CONSTITUTION.md sec 7.7):
            // real mesh wireframe overlay, toggled live via
            // md.set_terrain_wireframe(bool) -- see terrain_wireframe_debug_.
            terrain_quadtree_renderer_.InitWireframe(md::GpuDevice::Get().SDLDevice());
        }
    }
    _mark("GraniteTerrainInit");

    {
        TerrainGenParams tgp = TerrainGenParams::ProcWorld();
        tgp.nav_cs         = 50.0f;
        tgp.nav_ch         = 1.0f;
        tgp.world_offset_x = tnoff_x;
        tgp.world_offset_z = tnoff_z;
        tgp.force_noise    = false;
        tgp.zone_origin_x  = zone_ox;
        tgp.zone_origin_z  = zone_oz;

        auto upload_chunk = [&](int cx, int cz, ChunkCoord coord) {
            TerrainGen_Build(terrain_chunks[cz][cx], coord, tgp);
            TerrainGen_Upload(terrain_chunks[cz][cx]);
            auto& chunk = terrain_chunks[cz][cx];
            if (!chunk.loaded) return;
            JoltWorld::Get().RemoveBody(terrain_jolt_ids[cz][cx]);
            terrain_jolt_ids[cz][cx] = JoltWorld::Get().AddTerrainMesh(
                chunk.heightmap.h, TERRAIN_GRID, TERRAIN_STEP,
                chunk.center_x - CHUNK_SIZE * 0.5f,
                chunk.center_z - CHUNK_SIZE * 0.5f);
        };

        int loaded_count = 0;
        for (int cz = 0; cz < TNKN; ++cz)
            for (int cx = 0; cx < TNKN; ++cx) {
                upload_chunk(cx, cz, {cx, cz});
                if (terrain_chunks[cz][cx].loaded) ++loaded_count;
            }
        _mark("81-chunk build+upload loop");

        terrain.Init();
        _mark("terrain.Init");
        terrain.InitKenshiOverlay("game/data/textures/md_terrain.dds");
        _mark("InitKenshiOverlay");
        terrain.InitGroundTextureArray();
        _mark("InitGroundTextureArray (2x125-layer DDS)");
        terrain.InitDetailArray("", 1);   // a built-in 1-layer stub: no shader reads the array (#138), it only has to exist as a binding
        _mark("InitDetailArray (stub, 1 layer)");
        terrain.InitGroundBaked("game/data/textures/md_ground_baked.dds");
        _mark("InitGroundBaked");
        terrain.InitSteepnessSmoothed("game/data/textures/md_ground_steepness_smoothed.png");
        _mark("InitSteepnessSmoothed");
        terrain.InitBiomeBlend(md::KenshiLandFile("blendmap.png", "game/data/textures/md_biome_blend.png").c_str());
        _mark("InitBiomeBlend");
        terrain.InitAmbientMap(md::KenshiLandFile("overlaymaps/ambientmap.png", "game/data/textures/md_ambientmap.png").c_str());
        _mark("InitAmbientMap");
        { const std::string kd = md::KenshiDir(); if (!kd.empty()) terrain.InitKenshiLightCubes((kd + "/data/materials/mp_irradiance.dds").c_str(), (kd + "/data/materials/mp_specularity.dds").c_str()); }
        _mark("InitKenshiLightCubes (#140 L2)");
        // task-terrain-kenshi-parity (2026-09-26): Kenshi-parity biome-blend
        // path's two remaining data sources -- see TerrainRenderer's own doc
        // comments. Must run after BiomeRegistry::LoadFromFile (above) so
        // BiomeCount()/ForIndex() have real data.
        terrain.InitKbi1BlendLookup(md::DevFile("game/data/textures/md_kbi1_blend_lookup.png").c_str());
        _mark("InitKbi1BlendLookup");
        terrain.UploadBiomeLayersTex();
        _mark("UploadBiomeLayersTex");
        terrain.InitOverlayMask(md::DevFile("game/data/textures/md_overlay_mask.png").c_str());
        _mark("InitOverlayMask");

        // 2026-07-21: populate the global per-zone ground-layer LUT (same
        // one the editor's World3D synthesis mesh already builds via
        // s_build_zone_ground_layers, tools/editor/editor_world_3d_sdlgpu.cpp)
        // so the game's own ShadeTerrainGround (shaders/terrain_shading_
        // common.glsl) can bilinear-blend the 4 nearest zone corners' REAL
        // ground layers instead of using each chunk's own fixed
        // uFrag.ground_layers_a/b — see that file's cross-zone blend for
        // the hard chunk-boundary seam this replaces (two neighbouring
        // chunks assigned different Kenshi zones showed a stark all-6-
        // layers-at-once discontinuity with zero blend at the shared edge).
        {
            // Stride 28, all used (was 27, task БОРГ-VISUAL-3 2026-09-06):
            // slot 27 adds BiomeDef::biome_id (plain int, not bit-cast) for
            // the shader's cross-zone "same biome" test. Before that, was
            // 24 (task #13 follow-up cliff band 2026-09-06): slots 24..26
            // add the CLIFF layer's dual-sided blend band (min/max/fade,
            // bit-cast float triple) -- see BiomeDef::cliff_min/max/fade. Before
            // that, stride was 24 (was 19, task #13 slope layer
            // 2026-09-05): slots 19/20 add real per-biome slope-layer UV
            // tiling, slots 21..23 add the slope-layer's own blend band
            // (min/max/fade, bit-cast float triple) -- see BiomeDef::
            // tile_slope_x/slope_min etc. Before that, stride was 19 (was
            // 17, "wavy cliff lines" distortion 2026-09-04): slots 17/18
            // add real per-biome vertical-UV distortion amplitude/
            // wavelength (bit-cast float pair, same pattern as slots 6/7's
            // cliff tiling). Before that, stride was 17 (was 9, task #12
            // "ground-texture realism"): slots 9..16 add real per-layer UV
            // tiling for base/grass/dirt/road (bit-cast float pairs) -- was
            // one shared DETAIL_TILING=90 constant for every layer in
            // every biome before this.
            static uint32_t s_zone_layers[64 * 64 * 28];
            static_assert(sizeof(uint32_t) == sizeof(float), "bit-cast size mismatch");
            for (int zy = 0; zy < 64; ++zy) {
                for (int zx = 0; zx < 64; ++zx) {
                    const BiomeDef& b = TerrainGen_ResolveBiome(zx, zy);
                    int idx = (zy * 64 + zx) * 28;
                    s_zone_layers[idx + 0] = (uint32_t)b.tex_base;
                    s_zone_layers[idx + 1] = (uint32_t)b.tex_slope;
                    s_zone_layers[idx + 2] = (uint32_t)b.tex_cliff;
                    s_zone_layers[idx + 3] = (uint32_t)b.tex_grass;
                    s_zone_layers[idx + 4] = (uint32_t)b.tex_dirt;
                    s_zone_layers[idx + 5] = (uint32_t)b.tex_road;
                    // Real per-biome cliff UV tiling scale (task #306
                    // follow-up) -- bit-cast float, reinterpreted in the
                    // shader via uintBitsToFloat.
                    float ctx = b.cliff_tiling_x, cty = b.cliff_tiling_y;
                    memcpy(&s_zone_layers[idx + 6], &ctx, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 7], &cty, sizeof(uint32_t));
                    float bf = b.brightness_fix;
                    memcpy(&s_zone_layers[idx + 8], &bf, sizeof(uint32_t));
                    float tbx = b.tile_base_x,  tby = b.tile_base_y;
                    float tgx = b.tile_grass_x, tgy = b.tile_grass_y;
                    float tdx = b.tile_dirt_x,  tdy = b.tile_dirt_y;
                    float trx = b.tile_road_x,  try_ = b.tile_road_y;
                    memcpy(&s_zone_layers[idx + 9],  &tbx, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 10], &tby, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 11], &tgx, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 12], &tgy, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 13], &tdx, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 14], &tdy, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 15], &trx, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 16], &try_, sizeof(uint32_t));
                    // Real "wavy cliff lines" distortion (Ghidra-verified,
                    // see BiomeDef's field comment).
                    float da = b.distort_amplitude, dw = b.distort_wavelength;
                    memcpy(&s_zone_layers[idx + 17], &da, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 18], &dw, sizeof(uint32_t));
                    // task #13: real per-biome slope-layer UV tiling + blend
                    // band (see BiomeDef's field comment).
                    float tsx = b.tile_slope_x, tsy = b.tile_slope_y;
                    memcpy(&s_zone_layers[idx + 19], &tsx, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 20], &tsy, sizeof(uint32_t));
                    float smn = b.slope_min, smx = b.slope_max, sfd = b.slope_fade;
                    memcpy(&s_zone_layers[idx + 21], &smn, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 22], &smx, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 23], &sfd, sizeof(uint32_t));
                    // task #13 follow-up: real per-biome CLIFF blend band
                    // (see BiomeDef's field comment).
                    float cmn = b.cliff_min, cmx = b.cliff_max, cfd = b.cliff_fade;
                    memcpy(&s_zone_layers[idx + 24], &cmn, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 25], &cmx, sizeof(uint32_t));
                    memcpy(&s_zone_layers[idx + 26], &cfd, sizeof(uint32_t));
                    // task БОРГ-VISUAL-3 (2026-09-06): plain int, NOT
                    // bit-cast -- the shader's cross-zone blend uses this to
                    // cheaply test "same biome" between corner zones.
                    s_zone_layers[idx + 27] = (uint32_t)b.biome_id;
                }
            }
            terrain.UploadZoneGroundLayers(s_zone_layers, 64 * 64 * 28);
        }
        _mark("UploadZoneGroundLayers (cross-chunk biome blend)");

        // Per-biome fog colour override removed: real Kenshi's fog is a global
        // screen-space post-process (fog.hlsl/atmospherefog.hlsl, confirmed from
        // the real uncompiled shader source in tmp_/kenshi/data/materials/post/)
        // driven by sun angle/time-of-day, NOT per-biome data — there is no
        // per-biome fog colour field in the real game to derive this from.
        // GraphicsSettings::fog_color keeps its single global default.
        fprintf(stdout, "[Game] Terrain: %d/%d chunks ready (%.0f×%.0fm)\n",
                loaded_count, TNKN * TNKN,
                (float)TNKN * CHUNK_SIZE, (float)TNKN * CHUNK_SIZE);

        TerrainQuery::Get().Init(&terrain_chunks[0][0], TNKN,
                                  CHUNK_SIZE, tnoff_x, tnoff_z,
                                  base_cx, base_cz, zone_ox, zone_oz);
        PhysicsTerrainRegion::Get().SetOrigin(
            tnoff_x, tnoff_z, zone_ox * CHUNK_SIZE, zone_oz * CHUNK_SIZE);
    }

    FeatureScatterSystem::Get().Init();  // real Kenshi features.dat scatter placement

    if (auto* dev = md::GpuDevice::Get().SDLDevice())
        WorldMapUI::Get().Init(dev);

    // Overlay pipeline (camera recording button)
    {
        md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
        if (dev) {
            GpuPipeline::Desc od;
            od.vert_path              = "shaders/overlay.vert";
            od.frag_path              = "shaders/overlay.frag";
            od.vert_uniform_bufs      = 1;
            od.frag_samplers          = 1;
            od.has_depth_target       = false;
            od.raster.blend_enable    = true;
            od.raster.depth_test      = false;
            od.raster.depth_write     = false;
            od.raster.cull_back       = false;
            od.layout.count           = 0;
            overlay_pipeline.Create(od);

            GpuSamplerDesc cam_sdesc;
            cam_sdesc.min_filter = GpuSamplerDesc::Filter::LINEAR;
            cam_sdesc.mag_filter = GpuSamplerDesc::Filter::LINEAR;
            cam_sdesc.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
            cam_sdesc.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
            cam_sdesc.gen_mipmap = false;
            cam_sampler.Init(cam_sdesc);

            uint8_t px[CAM_BTN_SZ * CAM_BTN_SZ * 4] = {};
            GenCamIcon(px, false); MakeCamTex(dev, px, cam_idle_tex);
            GenCamIcon(px, true);  MakeCamTex(dev, px, cam_rec_tex);
        }
    }

    // Sky pipeline
    {
        GpuPipeline::Desc sd;
        sd.vert_path           = "shaders/sky.vert";
        sd.frag_path           = "shaders/sky.frag";
        sd.vert_uniform_bufs   = 1;
        sd.frag_uniform_bufs   = 1;
        sd.has_depth_target    = true;
        sd.raster.depth_test   = true;   // #191 №11: sky.vert z=0.9999, so prepass-covered (ground/NPC) pixels are rejected early; they are overwritten later anyway
        sd.raster.depth_write  = false;
        sd.raster.cull_back    = false;
        sd.layout.count        = 0;
        sky_pipeline.Create(sd);
        sd.frag_path = "shaders/sky_kenshi.frag"; sd.frag_samplers = 1;
        sky_kenshi_pipeline.Create(sd);
    }

    // G-3: Water plane (translucent, depth test ON, depth write OFF)
    {
        GpuPipeline::Desc wd;
        wd.vert_path           = "shaders/water.vert";
        wd.frag_path           = "shaders/water.frag";
        wd.vert_uniform_bufs   = 1;
        wd.frag_uniform_bufs   = 1;
        wd.frag_storage_bufs   = 1;  // b0: per-frame water/land height mask (kWaterMaskN grid)
        wd.has_depth_target    = true;
        wd.raster.blend_enable = true;
        wd.raster.depth_test   = true;
        wd.raster.depth_write  = false;
        wd.raster.cull_back    = false;
        wd.layout.count        = 0;
        water_pipeline.Create(wd);
        wd.frag_path = "shaders/water_kenshi.frag"; wd.frag_samplers = 3;
        water_kenshi_pipeline.Create(wd);
        water_mask_ssbo.Init(kWaterMaskN * kWaterMaskN * (int)sizeof(float));
    }

    // 2026-07-25: this Init() submits many ad-hoc upload command buffers
    // (each GpuTexture::InitFromDDSArray/InitFromMemory call does its own
    // SDL_SubmitGPUCommandBuffer with no fence wait — correct as long as
    // ENOUGH real frames render before anything samples the uploaded data,
    // since normal interactive play naturally idles at the spawn point for
    // many frames first). A --exec scenario's first REAL render (triggered
    // by md.screenshot(), scenario_driver.cpp) can be the very first
    // swapchain render pass of the whole process, sampling straight from
    // textures whose upload might still be in flight — reproducibly
    // crashed inside libvulkan_intel.so (heap corruption / SIGSEGV,
    // several different signatures) ONLY in that exact sequence, never
    // during normal interactive use. Isolated via tests/scenarios/
    // verify_quadtree_no_render.lua (loads everything, never renders,
    // 3/3 clean) vs. verify_quadtree_phase1.lua (same load + one
    // md.screenshot(), crashed most runs) — same code, same machine, same
    // build. Waiting here, once, after every upload this Init() will ever
    // issue and before the interactive loop's first real frame, is
    // cheap (Init() already dominates startup time) and closes the gap.
    md::GpuDevice::Get().WaitForIdle();
    _mark("Init() total (props/water/sky/solid pipelines)");
}

// Granite terrain migration, Phase 8 (see terrain_granite_* doc comment,
// scene_render.h): a camera copy translated from `cam`'s LOCAL floating-
// window position (tnoff_x/z anchored) into ABSOLUTE Kenshi-world space
// -- the same conversion this session's Phase 3 bug fix established
// (abs = local - tnoff + zone_ox*CHUNK_SIZE), applied to both position
// AND target so the view DIRECTION is unchanged, only translated.
// terrain_granite_hmap uses the absolute [0,extent) convention natively,
// so driving the clipmap Recenter/draw view-proj from this copy keeps
// everything in one consistent space instead of mixing local geometry
// against an absolute-space texture (exactly the bug class Phase 3 found
// and fixed).
MdCamera SceneRender::GraniteAbsCam(const MdCamera& cam) const {
    const float dx = (cam.pos.x - tnoff_x + zone_ox * CHUNK_SIZE) - cam.pos.x;
    const float dz = (cam.pos.z - tnoff_z + zone_oz * CHUNK_SIZE) - cam.pos.z;
    return MdCameraTranslated(cam, dx, dz);   // the view direction comes from the local numbers (see md_camera.h); only the position is moved
}

void SceneRender::UpdateGraniteTerrain(md::GpuCommandBufferHandle cmd, const MdCamera& cam, float aspect) {
    if (!terrain_granite_ready) return;

    MdCamera abs_cam = GraniteAbsCam(cam);
    granite_abs_cam_[0] = abs_cam.pos.x;
    granite_abs_cam_[1] = abs_cam.pos.y;
    granite_abs_cam_[2] = abs_cam.pos.z;
    // Recomputed every frame since the camera moves; DrawTerrainAndProps's
    // TerrainQuadtree::SelectVisible call reads this back later in the same
    // frame (see terrain_frustum_planes_'s own doc comment in scene_render.h
    // for why it's cached here instead of recomputed at the draw call site).
    abs_cam.FrustumPlanes(aspect, terrain_frustum_planes_);
    (void)cmd;
}
