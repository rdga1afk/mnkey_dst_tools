#include <monkey_dust/io/data_files.h>
#include "game_common.h"
#include "render/first_run_dialog.h"
#include <monkey_dust/io/kenshi_dir.h>
extern const char* const md_build_title;   // generated at build time: cmake/build_info.cmake

// Definition (declaration/extern in game_common.h); only InitGame writes this.
#ifdef MONKEY_DUST_EDITOR
float s_overlay_top = 0.f;
#endif

// ─── InitGame ────────────────────────────────────────────────────────────────
// One-shot init: platform, GPU, systems, ECS player entity.
// All heavy singletons are initialised here before the game loop.
bool g_no_dialogs = false;   // scenario / gate runs (--exec): no modal windows (set by main)
bool g_choose_kenshi_folder = false;   // --choose-kenshi-folder: show the first-run window again (set by main)

void InitGame(MdEntity& player_entity, GameState& gs, MdCamera& camera, float& cam_x, float& cam_z)
{
#ifdef MONKEY_DUST_EDITOR
    if (const char* ov = getenv("MD_OVERLAY_TOP_OFFSET")) s_overlay_top = (float)atof(ov);
#endif
    // Startup loading-time breakdown, outer half (task #158c continued —
    // SceneRender::Init's own internal breakdown, ~3.3-3.6s, only accounts
    // for part of the reported 7-8s blank-screen window; this covers
    // everything from window creation through NpcRender::Init that runs
    // BEFORE SceneRender::Init). Permanent diagnostic, same rationale as
    // scene_render.cpp's _mark.
    auto _gt0 = std::chrono::steady_clock::now();
    int  _splash_step = 0;
    auto _gmark = [&](const char* label) {
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - _gt0).count();
        fprintf(stdout, "[STARTUP-TIMING] %-28s %8.1fms\n", label, ms);
        // Splash progress bar. A naive "N checkpoints -> N equal slices"
        // split (the first version of this code) looked like it filled
        // instantly: these 8 outer checkpoints (JobSystem::Init through
        // NpcRender::Init -- window_init/audio_init/GpuDevice::Init fire
        // before the splash even exists, and the final "SceneRender::Init"
        // checkpoint fires AFTER that function's own internal checkpoints
        // below have already driven progress to 1.0, so neither counts
        // here) together take ~700-800ms out of a ~5.5-5.8s total startup
        // (measured this session's [STARTUP-TIMING] logs) -- SceneRender::
        // Init alone is the other ~85-90%. Reserve only a small outer
        // share so the bar doesn't rocket to "done" before the real work
        // (scene_render.cpp's OWN _mark checkpoints, driving the rest of
        // the range) even starts.
        constexpr int   kOuterSteps = 8;
        constexpr float kOuterShare = 0.12f;
        if (SplashScreen::Get().IsReady() && _splash_step < kOuterSteps) {
            ++_splash_step;
            SplashScreen::Get().SetProgress(
                kOuterShare * (float)_splash_step / (float)kOuterSteps);
            SplashScreen::Get().Present();
        }
    };
    MdConfig::Get().Load(md::DevFile("data/md_config.txt").c_str());  // VBfA pattern: runtime thread/physics config
    md::RegisterCoreComponents();  // editor Inspector reflection (WorldTransform, ...)
    md::WarmUpEngineComponents();  // flecs type registration before any JobGraph batch (task #248)
    fprintf(stderr, "[monkeydust] %s\n", md_build_title);
    window_init(0, 0, md_build_title);
    _gmark("window_init");
    input_init();
    audio_init();
    _gmark("audio_init");

    if (!md::GpuDevice::Get().Init(_wnd::ptr())) {   // Vulkan is the only renderer: no device = no game
        const std::string msg = std::string("The Vulkan device could not be created: ") + SDL_GetError() + "\nCheck the Vulkan layers (VK_LOADER_DEBUG=layer); an implicit layer such as lsfg-vk can break device creation (DISABLE_LSFGVK=1 turns it off).";
        MD_LOG(MD_LOG_ERROR, "%s", msg.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "monkeydust", msg.c_str(), _wnd::ptr());
        std::exit(1);
    }
    _gmark("GpuDevice::Init");
    // M2 (docs/archive/GRANITE_MIGRATION_PLAN_M0_M6.md): dual-run Granite Vulkan
    // device alongside SDL_GPU on the SAME window -- no-op when built
    // without MD_USE_GRANITE (USE_GRANITE=OFF). Not on the critical
    // startup path: SDL_GPU stays the real render backend until M5.
    if (md::GraniteBackend::Get().IsBuilt() && !md::GraniteBackend::Get().Init(_wnd::ptr()))
        MD_LOG(MD_LOG_WARNING, "GraniteBackend: Init failed (USE_GRANITE build)");
    // M2's own exit criterion ("обидва збірки компілюються й запускаються"
    // -- an empty frame clears): one-time verification burst, not wired
    // into the steady-state game loop yet (that's M5's job, with real
    // resize handling). Same teal proof-color as the probes.
    if (md::GraniteBackend::Get().IsReady())
        for (int _gf = 0; _gf < 10; ++_gf) md::GraniteBackend::Get().RenderEmptyFrame();
    // First real frame the window shows: splash background + progress bar,
    // not whatever garbage the compositor had in this buffer (see
    // SplashScreen::Init's doc comment). Reveal the window here too (belt-
    // and-suspenders alongside window.h's SDL_WINDOW_HIDDEN) so even the
    // brief window_init()..here gap never shows an unpainted frame.
    // First launch (no folder saved yet) or --choose-kenshi-folder: ask where the Kenshi files are before anything reads the install.
    {
        if (g_choose_kenshi_folder || md::KenshiFolderNeedsPrompt()) {
            SDL_ShowWindow(_wnd::ptr());
            if (!firstrun::Run(_wnd::ptr())) std::exit(0);
        }
    }
    {   // only Kenshi 1.0.68 is supported: warn visibly (not in scenario / gate runs), then start anyway
        const std::string vw = md::KenshiVersionWarning(md::KenshiDir());
        if (!vw.empty()) {
            MD_LOG(MD_LOG_WARNING, "%s", vw.c_str());
            if (!g_no_dialogs) { SDL_ShowWindow(_wnd::ptr()); SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "monkeydust: unsupported Kenshi version", vw.c_str(), _wnd::ptr()); }
        }
    }
    SplashScreen::Get().Init(md::GpuDevice::Get().SDLDevice());
    SplashScreen::Get().Present();
    SDL_ShowWindow(_wnd::ptr());
    window_set_vsync(0);  // mailbox (supported on ANV); vsync FIFO has tearing bug on Intel
    // LoadFromCfg BEFORE Init() -- Init() reads cfg_worker_override_ to size
    // the worker pool. Was implemented but never wired up (dead code path);
    // wiring it lets game/data/tasks.cfg's worker_threads actually take
    // effect instead of always silently falling back to cores-1.
    JobSystem::Get().Init();
    _gmark("JobSystem::Init");
    TerrainStreamQueue::Get().start();  // Item 5: async terrain gen worker thread
    RenderQualityConfig::LoadOverrides();  // optional game/data/render_quality_overrides.txt, no recompile needed to tune
    GraphicsSettings::Get().ApplyPreset(GraphicsPreset::Low);
    // InitNpcSDL() called after OpenGL init below to avoid "GPU not ready" warnings.
#  if defined(DEBUG) && defined(MONKEY_DUST_EDITOR)
    if (md::GpuDevice::Get().IsReady()) {
        SDL_GPUTextureFormat sc_fmt =
            SDL_GetGPUSwapchainTextureFormat(md::GpuDevice::Get().SDLDevice(), _wnd::ptr());
        imgui_sdlgpu_init(md::GpuDevice::Get().SDLDevice(), sc_fmt);
        {
            // Mirror tools/editor/main.cpp: 85% of display height / 720 — same font size.
            SDL_DisplayID disp = SDL_GetDisplayForWindow(_wnd::ptr());
            SDL_Rect b = {};
            int dh = window_get_height();
            if (SDL_GetDisplayUsableBounds(disp, &b) && b.h > 0) dh = b.h;
            float sc = (dh * 0.85f) / 720.f;
            if (sc < 0.5f) sc = 0.5f;
            MdFonts::Load(14.f * sc, 13.f * sc);
        }
        EditorUI::SetupTheme();
    }
#  endif

#ifdef MONKEY_DUST_EDITOR
    EditorCore::Get().Init();
    // EDITOR_AUTOMATION_PLAN_v1.md Phase 1.3: the game's F3 editor never
    // dlopens libeditor_panels.so (unlike monkey_dust_editor) — it links
    // editor_core.cpp/editor_toolbar.cpp/editor_command_palette.cpp/
    // editor_console.cpp statically, so without this call every palette/
    // toolbar/console Dispatch() here would hit an empty EditorCmdRegistry
    // and silently no-op. Own module id (distinct from kPanelsModuleId in
    // editor_panels_entry.cpp) since this is a wholly separate registration
    // lifetime from that .so's.
    RegisterStdEditorCommands(kGameEditorModuleId);
#endif

    TransformSoA::Get().Init();
    _gmark("NavSystem::Init+StartWorker");

    ParticleRenderer::Get().Init();
    AnimationSoA::Get().Init();
    _gmark("AnimationSoA::Init+LoadClips");
    // 2026-09-04: NpcRender::Init() moved ahead of the light init.
    // Its early-Z prepass pipeline (animated_prepass.vert/shadow_csm.frag,
    // vert_storage_bufs=4, depth_only=true) SIGSEGVs inside libvulkan_intel.so
    // on Deferred tiers specifically when it's created AFTER ~10 SSAO
    // pipelines -- confirmed via Vulkan validation layers that the identical
    // Desc/shader pair creates cleanly with zero errors on Forward tier,
    // where it's only the 3rd pipeline of the run. Three isolated tests ruled
    // out depth_only, zero-frag-resources, and shader content as the cause
    // (see docs/VISUAL_GAP_ANALYSIS.md §9.3) -- the only correlating factor
    // is pipeline-creation order/count, so creating it first sidesteps the
    // trigger without touching the pipeline itself.
    if (md::GpuDevice::Get().IsReady()) {
        NpcRender::Get().Init();
        _gmark("NpcRender::Init");
    }
    LightSystem::Get().Init();
    _gmark("Light Init");
    if (md::GpuDevice::Get().IsReady()) {
        SceneRender::Get().Init();
        _gmark("SceneRender::Init (see [STARTUP-TIMING] breakdown above)");
        SplashScreen::Get().Shutdown();
    }

    camera.pos    = { 0.0f, 30.0f, 30.0f };
    camera.target = { 0.0f,  0.0f,  0.0f };
    camera.up     = { 0.0f,  1.0f,  0.0f };
    camera.fovy   = 45.0f;





    LuaSystem::Get().Init("");
#if defined(MONKEY_DUST_EDITOR) || defined(MD_PERF_TEST_HOOKS)
    // Normally RegisterLuaScenarioAPI only runs inside RunScenarioMode
    // (--exec), which has its own dedicated loop separate from this
    // normal interactive one. Registering it here too makes md.screenshot/
    // md.set_camera_pose/md.ui.* available during a NORMAL (non--exec)
    // launch as well — needed for game_cmd_file.h's live command-file
    // driving (GameCmdFile_Poll, below in the main loop) to have anything
    // to call. Debug-tool-only surface, hence gated on MONKEY_DUST_EDITOR
    // OR MD_PERF_TEST_HOOKS (2026-08-12: split out so a no-ImGui release
    // build can still be live-driven for perf measurement — see that
    // option's doc comment, top-level CMakeLists.txt). Idempotent if
    // RunScenarioMode also registers it later (just re-sets the same
    // md.* table fields).
    RegisterLuaScenarioAPI(&cam_x, &cam_z);
#endif
#ifdef MONKEY_DUST_EDITOR
    RegisterLuaUiDriverAPI(LuaSystem::Get());
    UiDriver_Init();
#endif

    player_entity = MdRegistry::Get().Create();
    {
        auto& reg = MdRegistry::Get();
        // Add +1.5m so player capsule (feet=0) starts above terrain even if
        // GetHeight returns slightly stale data before first terrain chunk upload.
        //
        // Coordinate audit (task #158i-5/9, 2026-07-15): game terrain is a
        // FLOATING LOCAL WINDOW, not absolute Kenshi world metres. SceneRender::
        // Init() streams the real Kenshi zone (resolved via WorldRegistry::
        // CurrentZone(), e.g. "the_hub") into a fixed TNKN×TNKN chunk grid
        // positioned at tnoff_x/tnoff_z — NOT at zone_grid_index*CHUNK_SIZE.
        // TerrainQuery (which GetHeight below reads) is Init()ed with that same
        // tnoff_x/z origin, so any world position passed to it must be in THIS
        // local space. The previous hardcoded (11750,14250) spawn was carried
        // over from EDITOR screenshots — but EDITOR_TNKN=64 loads the WHOLE
        // world with chunks at their real absolute zone*CHUNK_SIZE position, a
        // fundamentally different convention. In-game that value landed
        // ~10km outside the loaded window: no terrain there, camera floats
        // over blue sky with nothing below it. Spawn at the loaded window's
        // centre instead (same formula as HandleTerrainStreaming's cx_w/cz_w
        // above) — by construction (zone_ox = grid_x - TNKN/2 in
        // SceneRender::Init()) this is always the CURRENT zone's own chunk.
        const float spawn_half = (SceneRender::TNKN / 2) * CHUNK_SIZE;
        const float spawn_x    = SceneRender::Get().tnoff_x + spawn_half;
        const float spawn_z    = SceneRender::Get().tnoff_z + spawn_half;
        float p_spawn_y = TerrainQuery::Get().GetHeight(spawn_x, spawn_z) + 1.5f;
        reg.Handle(player_entity).emplace<WorldTransform>(spawn_x, p_spawn_y, spawn_z, 0.0f);
        // the player entity is only the camera / streaming focus: position, movement input and a nav component the movement code reads
        PlayerController pctrl_wo = {};
        pctrl_wo.move_speed = 5.0f;
        reg.Handle(player_entity).emplace<PlayerController>(pctrl_wo);
        NavAgent nav_wo = {};
        reg.Handle(player_entity).emplace<NavAgent>(nav_wo);
    }

}  // InitGame
