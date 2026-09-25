#include <monkey_dust/platform/window.h>
#include "font_loader.h"
#include <monkey_dust/platform/input.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/granite_backend.h>
#include "editor_granite_imgui_bridge.h"
#include <monkey_dust/render/light_system.h>
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/ecs/component_reflect.h>
#include <monkey_dust/ecs/component_warmup.h>
#include <monkey_dust/scripting/lua_system.h>
#include "lua_editor_scenario_api.h"
#include "lua_editor_automation_api.h"
#include "editor_std_commands.h"
#include "editor_scenario_driver.h"
#include "editor_cmd_file.h"
#include "editor_screenshot.h"
#include <ctime>
#include <SDL3/SDL.h>
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "imgui.h"
#include "editor_core.h"
#include "editor_ui.h"
#include "item_editor.h"
#include "faction_editor.h"
#include "settings_editor.h"
#include "editor_world_3d_sdlgpu.h"
#include "editor_world_panel.h"
#include "editor_3d_bridge.h"
#include "editor_char_preview_sdlgpu.h"
#include "character_editor.h"
#include "npc_archetype_editor.h"
#include "editor_map_view.h"
#include "editor_node_graph.h"
#include "editor_layout.h"
#include "bug_capture.h"
#include "editor_reflect_bridge.h"
#include "editor_reflect_inspector.h"
#include <cstdio>
#include <cstring>

// ── called by /reload-shaders console command ─────────────────────
void EditorReloadAllShaderPipelines() {
    CharPreviewSDLGPU::ReloadPipelines();
    fprintf(stdout, "[Editor] Shader pipelines reloaded\n");
}

// ── Bridge: PCG terrain upload (defined here — only TU that includes W3D header) ─
void EditorW3D_UploadTerrainHeightmap(const float* hmap, int W, int H,
                                       float world_size_m, int chunk_x, int chunk_z) {
    WorldEditor3D_SDLGPU::UploadTerrainHeightmap(hmap, W, H, world_size_m, chunk_x, chunk_z);
}

// ──────────────────────────────────────────────────────────────────────────────
// monkey_dust EDITOR v1.0 — SDL_GPU (Vulkan) backend.
// Tabs: Items | Factions | NPCs | World | 3D World | Characters | Settings
// ──────────────────────────────────────────────────────────────────────────────

static constexpr const char* CFG_PATH    = "data/editor_config.json";
static constexpr const char* LAYOUT_PATH = "data/editor_layout.json";

// Extra Y offset when running under RenderDoc (overlay occupies top ~50px).
// Set via env var MD_OVERLAY_TOP_OFFSET=50 in .cap file or mde.sh --rd.
float s_overlay_top = 0.f;

// Backend world pointer, passed to the Inspector tab every frame.
static EcsBridgeWorldT* s_ecs_world = nullptr;

// Persistent panel layout (all tabs). Loaded at startup, saved at shutdown.
static EditorLayout::Layout s_lay;

int main(int argc, char** argv) {
    EditorScenarioConfig scenario_cfg;
    if (!ParseEditorScenarioArgs(argc, argv, scenario_cfg)) return 1;

    const char* ov = getenv("MD_OVERLAY_TOP_OFFSET");
    if (ov) s_overlay_top = (float)atof(ov);

    // ── Window ────────────────────────────────────────────────────────────────
    window_init(0, 0, "monkey_dust EDITOR v1.0");
    input_init();
    {
        int ww, wh;
        if (scenario_cfg.active) {
            // Etap 5 VERIFY finding: display-bounds-derived sizing (else
            // branch below) varies run-to-run in this environment
            // (SDL_GetDisplayUsableBounds isn't perfectly stable across
            // launches), which made screenshot RMSE comparisons spuriously
            // fail — editor_screenshot_compare.py resizes on a size
            // mismatch, and THAT resize is what produced a 7.5% RMSE
            // between two runs of the identical scenario+seed, not any
            // actual rendering difference. Fixed size for --exec mode
            // removes this non-determinism source at its root (not a
            // loosened threshold — fixing the criterion's real cause, per
            // AUTONOMY_PROTOCOL.md point 4, not papering over it).
            ww = 1366; wh = 706;
        } else {
            SDL_DisplayID disp = SDL_GetDisplayForWindow(_wnd::ptr());
            SDL_Rect b = {};
            int mw = window_get_width(), mh = window_get_height();
            if (SDL_GetDisplayUsableBounds(disp, &b) && b.w > 0) { mw=b.w; mh=b.h; }
            wh = (mh*85)/100; ww = (wh*16)/9;
            if (ww > (mw*90)/100) { ww=(mw*90)/100; wh=(ww*9)/16; }
            if (wh < 480) { wh=480; ww=854; }
        }
        SDL_SetWindowSize(_wnd::ptr(), ww, wh);
        SDL_SetWindowPosition(_wnd::ptr(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        _wnd::width()=ww; _wnd::height()=wh;
        EditorUI::ui_scale = (float)wh/720.f;
    }

    // ── SDL_GPU ───────────────────────────────────────────────────────────────
    if (!md::GpuDevice::Get().Init(_wnd::ptr())) {
        fprintf(stderr, "[Editor] SDL_GPU init failed\n"); return 1;
    }
    // window.cpp creates every window with SDL_WINDOW_HIDDEN so the
    // compositor never shows an unpainted first frame; game_init.cpp
    // un-hides it once its splash screen is ready to present. The editor
    // has no splash screen and no equivalent call anywhere in its init
    // path -- reproduced live once with xwininfo reporting the window
    // stuck at "Map State: IsUnMapped" indefinitely despite the process
    // rendering normally internally. No-op if the window is already
    // shown by the time this runs.
    SDL_ShowWindow(_wnd::ptr());
    md::GpuDeviceHandle gpu = md::GpuDevice::Get().SDLDevice();
    SDL_GPUTextureFormat sc_fmt = SDL_GetGPUSwapchainTextureFormat(gpu, _wnd::ptr());

    // ── ImGui ─────────────────────────────────────────────────────────────────
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->Clear();
    float sc      = EditorUI::ui_scale;
    float ui_sz   = 14.f * sc; if (ui_sz   < 8.f) ui_sz   = 8.f;
    float mono_sz = 13.f * sc; if (mono_sz < 8.f) mono_sz = 8.f;
    MdFonts::Load(ui_sz, mono_sz);  // embedded Arimo + UbuntuMono — no system dependency
    EditorUI::font_regular = MdFonts::regular;
    EditorUI::font_bold    = MdFonts::bold;
    EditorUI::font_mono    = MdFonts::mono;

    ImGui_ImplSDL3_InitForSDLGPU(_wnd::ptr());
#if defined(MD_RENDER_BACKEND_GRANITE) && defined(MD_USE_GRANITE)
    // RENDER-BACKEND-STAGE-4 (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md
    // §2.4): the editor's own UI chrome (toolbar/panels/menus, built below
    // built below against THIS SAME ImGui context)
    // renders through Granite instead of imgui_impl_sdlgpu3 when selected --
    // ImGui_ImplSDLGPU3_Init() is skipped entirely in this branch (not just
    // unused): both backends write their own state into the SAME io.
    // BackendRendererUserData slot, so initializing both on one context
    // would have the second call's init silently clobber the first's,
    // leaking whatever GPU resources (font texture, sampler) the first
    // one had already created. The 3D viewports (editor_world_3d_sdlgpu.cpp
    // etc) keep their own SdlGpuBackend instances regardless (§5.4 "власний
    // екземпляр"), so they stay blank this frame (their SDL_GPU present is
    // skipped below too, not just their content -- Granite and SDL_GPU are
    // two independent swapchains on the same window; whichever presents
    // last is what's actually visible, they don't composite).
    if (!md::GraniteBackend::Get().Init(_wnd::ptr()))
        fprintf(stderr, "[Editor] GraniteBackend::Init failed -- Крок 4 UI switch will not render\n");
    else if (!md::editor::GraniteImGuiBridge_Init(_wnd::ptr()))
        fprintf(stderr, "[Editor] GraniteImGuiBridge_Init failed -- Крок 4 UI switch will not render\n");
#else
    ImGui_ImplSDLGPU3_InitInfo info = {};
    info.Device = gpu; info.ColorTargetFormat = sc_fmt;
    ImGui_ImplSDLGPU3_Init(&info);
#endif
    EditorUI::SetupTheme();

    // ── Data ──────────────────────────────────────────────────────────────────
    // Field metadata for the reflect-driven Inspector tab (editor_reflect_bridge.h
    // resolves these names against the live flecs world). Same call game/src/main.cpp
    // makes — populates md::ComponentReflect only, no ECS world interaction.
    md::RegisterCoreComponents();
    md::WarmUpEngineComponents();  // flecs type registration before any JobGraph batch (task #248)
    // Field metadata for the reflect-driven Inspector tab.
    s_ecs_world = &MdRegistry::Get().Raw();
    EcsReflectBridge::Get().Init(s_ecs_world);
    // Autonomy system (Etap 4) — same call game/src/main.cpp makes; must
    // happen before RegisterLuaEditorScenarioAPI/StartScenario, which
    // assume L_ is already a live sandboxed Lua state.
    LuaSystem::Get().Init("data/scripts");
    SettingsEditor::Load(CFG_PATH);
    ItemEditor::Load("data/items/items.json");
    FactionEditor::Load("data/factions/factions.json");
    NpcArchetypeEditor::Load("game/data/defs/npc_archetypes.json");
    WorldPanel::Init();
    // Terrain atlas (editable heightmap) + light system for editor 3D view
    LightSystem::Get().Init();
    TerrainAtlas_Load("game/data/terrain/world_hmap");
    TerrainAtlas_SmoothBoundaries();
    WorldEditor3D_SDLGPU::Init(
        "game/data/textures/md_terrain.dds",
        29, 25);  // 7×7 view centred near The Hub area
    CharacterEditor::LoadJSON("game/data/chars/player.chardef");
    CharacterEditor::LoadMorphNames("game/data/chars/morph_names.txt");
    MapViewPanel::Get().Init();
    EditorCore::Get().Init();
    // Single binary, no dlopen boundary — LuaSystem::Get() here is the
    // same instance the scenario driver resumes against.
    RegisterLuaEditorScenarioAPI(LuaSystem::Get());
    RegisterLuaEditorAutomationAPI(LuaSystem::Get());
    static constexpr uint32_t kEditorModuleId = 3;
    RegisterStdEditorCommands(kEditorModuleId);

    // Restore panel layout (map/world/terrain/world3d used directly from
    // s_lay below; items/factions/npcs/chars/settings copied into each
    // panel's own header-namespace state, matching their DrawContent API).
    {
        s_lay = EditorLayout::Layout{};
        EditorLayout::Load(LAYOUT_PATH, s_lay);
        ItemEditor::g_detached         = s_lay.items.detached;    ItemEditor::g_win_pos         = s_lay.items.pos;    ItemEditor::g_win_size         = s_lay.items.size;
        FactionEditor::g_detached      = s_lay.factions.detached; FactionEditor::g_win_pos      = s_lay.factions.pos; FactionEditor::g_win_size      = s_lay.factions.size;
        NpcArchetypeEditor::g_detached = s_lay.npcs.detached;     NpcArchetypeEditor::g_win_pos = s_lay.npcs.pos;     NpcArchetypeEditor::g_win_size = s_lay.npcs.size;
        CharacterEditor::g_detached    = s_lay.chars.detached;    CharacterEditor::g_win_pos    = s_lay.chars.pos;    CharacterEditor::g_win_size    = s_lay.chars.size;
        SettingsEditor::g_detached     = s_lay.settings.detached; SettingsEditor::g_win_pos     = s_lay.settings.pos; SettingsEditor::g_win_size     = s_lay.settings.size;
    }

    SDL_FlushEvent(SDL_EVENT_QUIT);

    // ── Scenario mode (Etap 4, --exec) ───────────────────────────────────────
    // Reuses the SAME render loop below rather than a second driver function
    // that duplicates window/ImGui/hot-reload plumbing — ResumeScenario() is
    // called once per rendered frame (editor has no logic-tick concept the
    // way the game does; a "tick" here IS a frame).
    int  scenario_exit_code = 0;
    bool scenario_done      = false;
    long scenario_frame_count = 0;
    double scenario_wall_start_s = 0.0;
    auto wall_now = []() -> double {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    };
    if (scenario_cfg.active) {
        FILE* sf = fopen(scenario_cfg.script_path, "rb");
        if (!sf) {
            fprintf(stderr, "[EditorScenario] cannot open script: %s\n", scenario_cfg.script_path);
            return 1;
        }
        fseek(sf, 0, SEEK_END);
        long sz = ftell(sf);
        fseek(sf, 0, SEEK_SET);
        static char s_script_buf[1 << 20];  // 1MB cap, no heap
        if (sz <= 0 || sz >= (long)sizeof(s_script_buf)) {
            fprintf(stderr, "[EditorScenario] script too large or empty: %s\n", scenario_cfg.script_path);
            fclose(sf);
            return 1;
        }
        size_t nread = fread(s_script_buf, 1, (size_t)sz, sf);
        s_script_buf[nread] = '\0';
        fclose(sf);

        lua_Integer n_ticks_arg = scenario_cfg.max_frames > 0 ? scenario_cfg.max_frames : 100000;
        char parse_err[256] = {};
        if (!LuaSystem::Get().StartScenario(s_script_buf, n_ticks_arg, parse_err, sizeof(parse_err))) {
            fprintf(stderr, "[EditorScenario] parse error in %s: %s\n", scenario_cfg.script_path, parse_err);
            return 1;
        }
        scenario_wall_start_s = wall_now();
    }

    // ── Main loop ─────────────────────────────────────────────────────────────
    char  status_msg[64] = {};
    float status_timer   = 0.f;
    uint64_t last_ticks  = SDL_GetTicks();

    while (!input_should_quit()) {
        if (scenario_cfg.active && !scenario_done) {
            bool watchdog_hit = false;
            if (scenario_cfg.max_frames > 0 && scenario_frame_count >= scenario_cfg.max_frames) {
                fprintf(stderr, "[EditorScenario] watchdog: max-frames exceeded\n");
                scenario_exit_code = 124; watchdog_hit = true;
            }
            double now_wall = wall_now();
            if (!watchdog_hit && scenario_cfg.max_seconds > 0.0 &&
                (now_wall - scenario_wall_start_s) >= scenario_cfg.max_seconds) {
                fprintf(stderr, "[EditorScenario] watchdog: max-seconds exceeded\n");
                scenario_exit_code = 124; watchdog_hit = true;
            }
            if (watchdog_hit) {
                scenario_done = true;
                _sdl3_input::s_quit = true;
            } else {
                LuaSystem::ScenarioResult r = LuaSystem::Get().ResumeScenario();
                ++scenario_frame_count;
                if (r.status == LuaSystem::ScenarioStatus::Failed) {
                    printf("[EditorScenario] FAILED: %s\n", r.error_msg);
                    scenario_exit_code = 1; scenario_done = true; _sdl3_input::s_quit = true;
                } else if (r.status == LuaSystem::ScenarioStatus::Quit) {
                    scenario_exit_code = r.quit_code; scenario_done = true; _sdl3_input::s_quit = true;
                } else if (r.status == LuaSystem::ScenarioStatus::Finished) {
                    scenario_exit_code = 0; scenario_done = true; _sdl3_input::s_quit = true;
                }
                // Yielded — fall through, render this frame normally, resume again next frame.
            }
        }

        // Command-file automation (task #123) — independent of --exec, runs
        // every frame so an already-running editor can be driven live.
        EditorCmdFile_Poll();

        // Frame cap — editor targets 60 fps; iGPU shares cooling with CPU.
        // --fast skips this entirely (scenario mode wants max throughput).
        if (!(scenario_cfg.active && scenario_cfg.fast)) {
            static Uint64 s_prev_ns = 0;
            static constexpr Uint64 TARGET_NS = 1000000000ULL / 60;
            Uint64 now_ns = SDL_GetTicksNS();
            if (s_prev_ns && now_ns - s_prev_ns < TARGET_NS)
                SDL_DelayNS(TARGET_NS - (now_ns - s_prev_ns));
            s_prev_ns = SDL_GetTicksNS();
        }
        uint64_t now = SDL_GetTicks();
        float dt = (float)(now-last_ticks)/1000.f;
        last_ticks = now;
        if (status_timer > 0.f) status_timer -= dt;

        // F9: dump editor state → tmp_/bug_editor_TIMESTAMP.txt
        if (input_key_pressed(SDL_SCANCODE_F9)) {
            char path[256];
            FILE* f = BugCapture::Open("editor", path, sizeof(path));
            if (f) {
                fprintf(f, "[Editor]\n");
                fprintf(f, "  chars_detached=%d\n\n", CharacterEditor::g_detached ? 1 : 0);
#ifdef MD_SDL_GPU
                CharPreviewSDLGPU::DumpState(f);
#endif
                BugCapture::Close(f);
                snprintf(status_msg, sizeof(status_msg), "[F9] %s", path + 7);
                status_timer = 4.f;
            }
        }

        window_begin_frame();

        // ── SDL event pump — required for io.MouseWheel and quit detection ───
        // SDL_GetMouseState() covers position+buttons (realtime), but
        // SDL_EVENT_MOUSE_WHEEL is queue-only: without PollEvent, io.MouseWheel
        // is always 0 and scroll never reaches the terrain viewport.
        {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                ImGui_ImplSDL3_ProcessEvent(&ev);
                if (ev.type == SDL_EVENT_QUIT)
                    _sdl3_input::s_quit = true;
                if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                    int sc = (int)ev.key.scancode;
                    if (sc >= 0 && sc < SDL_SCANCODE_COUNT)
                        _sdl3_input::s_next[sc] = true;
                }
                if (ev.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED)
                    SDL_MaximizeWindow(_wnd::ptr());
            }
            input_begin_frame();
        }

        // ── ImGui frame ───────────────────────────────────────────────────────
#if defined(MD_RENDER_BACKEND_GRANITE) && defined(MD_USE_GRANITE)
        md::editor::GraniteImGuiBridge_NewFrame();
#else
        ImGui_ImplSDLGPU3_NewFrame();
#endif
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& fio = ImGui::GetIO();

        // Toolbar draws the menu bar (~20px) + button bar (30px fixed)
        // f3_passthrough: pass-through mouse input when a fullscreen viewport tab is active.
        // Uses prev-frame flag (1-frame lag is imperceptible).
        float toolbar_h = s_overlay_top + ImGui::GetFrameHeight() + 30.f;

        static bool s_world3d_was_active  = false;
        static bool s_charpreview_active  = false;
        static bool s_mapview_active      = false;
        EditorCore::Get().f3_passthrough = s_world3d_was_active;
        s_world3d_was_active = false;
        s_charpreview_active = false;
        s_mapview_active     = false;
        EditorCore::Get().Update(dt);

        // Autonomy system: md.editor_open_panel(name) forces a tab select
        // this frame. Consumed ONCE here (not per-tab) — a single local
        // copy must be compared against every candidate tab below.
        const char* forced_tab = EditorPanels_ConsumeForcedTab();

        ImGui::SetNextWindowPos({0, toolbar_h});
        ImGui::SetNextWindowSize({fio.DisplaySize.x, fio.DisplaySize.y - toolbar_h});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
        ImGui::Begin("##editor",nullptr,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
            ImGuiWindowFlags_NoScrollWithMouse|
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar();
        ImGui::SetCursorPos({0,0});
        ImGui::Separator();
        ImGui::SetCursorPosX(4);

        static constexpr ImGuiWindowFlags FLOAT_FLAGS = ImGuiWindowFlags_NoSavedSettings;
        static int s_active_tab = 0;

        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Items")) { s_active_tab = 0;
                if (!ItemEditor::g_detached) {
                    ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                    if (ItemEditor::DrawContent("data/items/items.json")) {
                        snprintf(status_msg, sizeof(status_msg), "Items saved!");
                        status_timer = 3.f;
                    }
                } else {
                    ImVec2& pos = ItemEditor::g_win_pos;
                    ImVec2& sz  = ItemEditor::g_win_size;
                    const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                    if (pos.y < min_y) pos.y = min_y;
                    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                    ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                    if (ImGui::Begin("Items##float", &ItemEditor::g_detached, FLOAT_FLAGS)) {
                        if (ItemEditor::DrawContent("data/items/items.json")) {
                            snprintf(status_msg, sizeof(status_msg), "Items saved!");
                            status_timer = 3.f;
                        }
                    }
                    pos = ImGui::GetWindowPos();
                    if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                    sz = ImGui::GetWindowSize();
                    ImGui::End();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Factions")) { s_active_tab = 1;
                if (!FactionEditor::g_detached) {
                    ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                    if (FactionEditor::DrawContent("data/factions/factions.json")) {
                        snprintf(status_msg, sizeof(status_msg), "Factions saved!");
                        status_timer = 3.f;
                    }
                } else {
                    ImVec2& pos = FactionEditor::g_win_pos;
                    ImVec2& sz  = FactionEditor::g_win_size;
                    const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                    if (pos.y < min_y) pos.y = min_y;
                    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                    ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                    if (ImGui::Begin("Factions##float", &FactionEditor::g_detached, FLOAT_FLAGS)) {
                        if (FactionEditor::DrawContent("data/factions/factions.json")) {
                            snprintf(status_msg, sizeof(status_msg), "Factions saved!");
                            status_timer = 3.f;
                        }
                    }
                    pos = ImGui::GetWindowPos();
                    if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                    sz = ImGui::GetWindowSize();
                    ImGui::End();
                }
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags map_flags = (forced_tab && strcmp(forced_tab, "Map") == 0)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Map", nullptr, map_flags)) { s_active_tab = 2;
                s_mapview_active = true;
                auto draw_map = [&]() {
                    ImGuiIO& mio = ImGui::GetIO();
                    if (mio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) MapViewPanel::Get().Undo();
                    if (mio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) MapViewPanel::Get().Redo();
                    MapViewPanel::Get().Draw(dt);
                };
                if (!s_lay.map.detached) {
                    ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                    draw_map();
                } else {
                    ImVec2& pos = s_lay.map.pos; ImVec2& sz = s_lay.map.size;
                    const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                    if (pos.y < min_y) pos.y = min_y;
                    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                    ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                    if (ImGui::Begin("Map##float", &s_lay.map.detached, FLOAT_FLAGS)) draw_map();
                    pos = ImGui::GetWindowPos();
                    if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                    sz = ImGui::GetWindowSize();
                    ImGui::End();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("World")) { s_active_tab = 3;
                if (!s_lay.world.detached) {
                    ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                    WorldPanel::Draw(dt);
                } else {
                    ImVec2& pos = s_lay.world.pos; ImVec2& sz = s_lay.world.size;
                    const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                    if (pos.y < min_y) pos.y = min_y;
                    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                    ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                    if (ImGui::Begin("World##float", &s_lay.world.detached, FLOAT_FLAGS)) WorldPanel::Draw(dt);
                    pos = ImGui::GetWindowPos();
                    if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                    sz = ImGui::GetWindowSize();
                    ImGui::End();
                }
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags world3d_flags = (forced_tab && strcmp(forced_tab, "3D World") == 0)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("3D World", nullptr, world3d_flags)) { s_active_tab = 4;
                s_world3d_was_active = true;
                ImVec2 avail = ImGui::GetContentRegionAvail();
                WorldEditor3D_SDLGPU::DrawImGui(avail.x, avail.y - 2, dt);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("NPCs")) { s_active_tab = 5;
                if (!NpcArchetypeEditor::g_detached) {
                    ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                    NpcArchetypeEditor::DrawContent();
                } else {
                    ImVec2& pos = NpcArchetypeEditor::g_win_pos;
                    ImVec2& sz  = NpcArchetypeEditor::g_win_size;
                    const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                    if (pos.y < min_y) pos.y = min_y;
                    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                    ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                    if (ImGui::Begin("NPC Archetypes##float", &NpcArchetypeEditor::g_detached, FLOAT_FLAGS))
                        NpcArchetypeEditor::DrawContent();
                    pos = ImGui::GetWindowPos();
                    if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                    sz = ImGui::GetWindowSize();
                    ImGui::End();
                }
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags characters_flags = (forced_tab && strcmp(forced_tab, "Characters") == 0)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Characters", nullptr, characters_flags)) { s_active_tab = 6;
                s_charpreview_active = true;
                if (!CharacterEditor::g_detached) {
                    ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                    CharacterEditor::Draw(false);
                } else {
                    ImVec2& pos = CharacterEditor::g_win_pos;
                    ImVec2& sz  = CharacterEditor::g_win_size;
                    const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                    if (pos.y < min_y) pos.y = min_y;
                    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                    ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                    if (ImGui::Begin("Characters##float", &CharacterEditor::g_detached, FLOAT_FLAGS))
                        CharacterEditor::Draw(false);
                    pos = ImGui::GetWindowPos();
                    if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                    sz = ImGui::GetWindowSize();
                    ImGui::End();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Inspector")) { s_active_tab = 7;
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                EditorReflectInspector::DrawContent(s_ecs_world);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) { s_active_tab = 8;
                if (!SettingsEditor::g_detached) {
                    ImGui::SetCursorPos({12, ImGui::GetCursorPosY() + 4});
                    SettingsEditor::DrawContent(CFG_PATH, status_msg, &status_timer);
                } else {
                    ImVec2& pos = SettingsEditor::g_win_pos;
                    ImVec2& sz  = SettingsEditor::g_win_size;
                    const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                    if (pos.y < min_y) pos.y = min_y;
                    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                    ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                    if (ImGui::Begin("Settings##float", &SettingsEditor::g_detached, FLOAT_FLAGS))
                        SettingsEditor::DrawContent(CFG_PATH, status_msg, &status_timer);
                    pos = ImGui::GetWindowPos();
                    if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                    sz = ImGui::GetWindowSize();
                    ImGui::End();
                }
                ImGui::EndTabItem();
            }
            // Trailing "Detach" rendered directly in the tab bar's own row
            // (ImGuiTabItemFlags_Trailing sorts it to the end regardless of
            // call order). Only shown for the currently active tab while it's
            // docked. 3D World / Inspector have no detach concept — skipped.
            bool* active_det = nullptr;
            switch (s_active_tab) {
                case 0: active_det = &ItemEditor::g_detached;         break;
                case 1: active_det = &FactionEditor::g_detached;      break;
                case 2: active_det = &s_lay.map.detached;             break;
                case 3: active_det = &s_lay.world.detached;           break;
                case 5: active_det = &NpcArchetypeEditor::g_detached; break;
                case 6: active_det = &CharacterEditor::g_detached;    break;
                case 8: active_det = &SettingsEditor::g_detached;     break;
                default: break;
            }
            if (active_det && !*active_det) {
                if (ImGui::TabItemButton("Detach", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
                    *active_det = true;
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
        ImGui::Render();

        // ── SDL_GPU: render off-screen RTTs + ImGui to swapchain ─────────────
        md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
        if (cmd) {
            // 1. Render 3D viewports to their own off-screen RTTs (consumed
            // by the ImGui::Image calls already recorded above via DrawImGui).
            if (s_world3d_was_active) WorldEditor3D_SDLGPU::RenderFrame(cmd, dt, true);
            if (s_charpreview_active) CharPreviewSDLGPU::RenderFrame(cmd);
            if (s_mapview_active)     MapViewPanel::Get().RenderFrame(cmd);

            // 2. Acquire swapchain + clear + ImGui
            uint32_t sw=0, sh=0;
            md::GpuTextureHandle sc = md::GpuDevice::Get().AcquireSwapchainTexture(cmd, &sw, &sh);
            if (sc) {
                SDL_GPUColorTargetInfo ct={};
                ct.texture=sc; ct.load_op=SDL_GPU_LOADOP_CLEAR;
                ct.store_op=SDL_GPU_STOREOP_STORE;
                ct.clear_color={0.10f,0.10f,0.13f,1.f};
                SDL_GPURenderPass* rp=SDL_BeginGPURenderPass(cmd,&ct,1,nullptr);
                if (rp) SDL_EndGPURenderPass(rp);

                ImDrawData* dd=ImGui::GetDrawData();
                if (dd && dd->CmdListsCount>0) {
                    ImGui_ImplSDLGPU3_PrepareDrawData(dd,cmd);
                    SDL_GPUColorTargetInfo ict={};
                    ict.texture=sc; ict.load_op=SDL_GPU_LOADOP_LOAD;
                    ict.store_op=SDL_GPU_STOREOP_STORE;
                    SDL_GPURenderPass* irp=SDL_BeginGPURenderPass(cmd,&ict,1,nullptr);
                    if (irp) { ImGui_ImplSDLGPU3_RenderDrawData(dd,cmd,irp); SDL_EndGPURenderPass(irp); }
                }
                char shot_path[256];
                if (EditorScreenshot_ConsumePending(shot_path, sizeof(shot_path))) {
                    // Captures the fully-composited frame (3D viewport + ImGui
                    // chrome) just rendered above. CaptureAndSubmit submits cmd
                    // itself (fence-waited, required for DownloadFromGPUTexture) —
                    // do not also call the plain submit below for this frame.
                    EditorScreenshot_CaptureAndSubmit(gpu, cmd, sc, sw, sh, sc_fmt, shot_path);
                } else {
                    md::GpuDevice::Get().Submit(cmd);
                }
            } else {
                md::GpuDevice::Get().Submit(cmd);
            }
        }
        window_end_frame();
    }

    // Save panel layout before shutdown
    {
        s_lay.items    = {ItemEditor::g_detached,         ItemEditor::g_win_pos,         ItemEditor::g_win_size};
        s_lay.factions = {FactionEditor::g_detached,      FactionEditor::g_win_pos,      FactionEditor::g_win_size};
        s_lay.npcs     = {NpcArchetypeEditor::g_detached, NpcArchetypeEditor::g_win_pos, NpcArchetypeEditor::g_win_size};
        s_lay.chars    = {CharacterEditor::g_detached,    CharacterEditor::g_win_pos,    CharacterEditor::g_win_size};
        s_lay.settings = {SettingsEditor::g_detached,     SettingsEditor::g_win_pos,     SettingsEditor::g_win_size};
        // map/world updated live above; terrain/world3d untouched here.
        EditorLayout::Save(LAYOUT_PATH, s_lay);
    }
    // See WorldEditor3D_SDLGPU::Shutdown()'s doc comment — must run before
    // GpuDevice::Get().Shutdown() below.
    WorldEditor3D_SDLGPU::Shutdown();
    EditorCore::Get().Shutdown();
#if defined(MD_RENDER_BACKEND_GRANITE) && defined(MD_USE_GRANITE)
    md::editor::GraniteImGuiBridge_Shutdown();
    md::GraniteBackend::Get().Shutdown();
#else
    ImGui_ImplSDLGPU3_Shutdown();
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    md::GpuDevice::Get().Shutdown();
    window_shutdown();
    return scenario_cfg.active ? scenario_exit_code : 0;
}
