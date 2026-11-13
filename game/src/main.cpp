#include <monkey_dust/io/derived_cache.h>
#include <cstring>
#include "game_common.h"
#include <monkey_dust/io/data_files.h>
#include <cstdio>

extern const char* const md_build_title;   // generated at build time: cmake/build_info.cmake

#ifdef MONKEY_DUST_EDITOR
// Hot-reload stub: editor_console.cpp /reload-shaders — no-op in game build.
void EditorReloadAllShaderPipelines() {}
// scenario bursts paced by the real game loop's frame cap (md.set_burst_paced, power runs); the platform header stays in main.cpp
bool g_dynres_input_event = false; // a key, wheel or button-held mouse motion since the last rendered frame (dynres v2 step 4; the controller clears it)
void ScenarioPacedFrameWait(uint64_t& prev_ns) { window_frame_cap_wait(prev_ns); }
void ScenarioNoteActivity() { window_note_activity(); }
#endif


// ─── main ─────────────────────────────────────────────────────────────────────
// Etap 5 VERIFY: trivial comment-only change for the protocol session-simulation
// (docs/AUTONOMY_LOG.md §VERIFY) — no behavior change.
int main(int argc, char** argv)
{
    // Autonomy system (Etap 1, AUTONOMY_SYSTEM_PROMPT_v2.md) — parsed before
    // anything else so a malformed --exec flag fails fast, before paying for
    // InitGame()'s window/GPU/world bootstrap.
    // Answered before anything is created: no window, no GPU, no install read.
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--version")) { printf("%s\n", md_build_title); return 0; }
        if (!strcmp(argv[i], "--licenses")) {
            size_t n = 0; const unsigned char* t = md::EmbeddedFileBytes("engine/THIRD_PARTY.md", &n);
            if (!t) { fprintf(stderr, "no licence text in this build\n"); return 1; }
            fwrite(t, 1, n, stdout); return 0;
        }
    }
    ScenarioConfig scenario_cfg;
    if (!ParseScenarioArgs(argc, argv, scenario_cfg)) return 1;
    g_no_dialogs = scenario_cfg.active;
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--choose-kenshi-folder")) g_choose_kenshi_folder = true;
    // Objects that survive the full game loop — initialized in InitGame.
    MdEntity player_entity = MdEntity::Null();
    GameState    gs            = {};
    MdCamera     camera;
    float cam_x = 0.f, cam_z = 5.f;

    InitGame(player_entity, gs, camera, cam_x, cam_z);
    md::CacheCollectGarbage();   // superseded derived-data files (an encoder or input change leaves the old key behind)

    // Script mode (--exec): own tick-driven loop, bypasses the interactive
    // while(!input_should_quit()) loop below entirely. See
    // game/src/autonomy/scenario_driver.cpp for why (headless-without-GPU
    // isn't safely separable from InitGame() yet — docs/AUTONOMY_LOG.md
    // §Recon.3 — so InitGame() still runs unconditionally; --headless only
    // skips per-tick rendering, not window/GPU creation).
    if (scenario_cfg.active) {
        int code = RunScenarioMode(scenario_cfg, player_entity, gs, camera, cam_x, cam_z);
        ShutdownGame(player_entity, /*save_world=*/false);
        return code;
    }

    float logic_accum = 0.0f;
    float now_s       = 0.0f;
    bool  paused      = false;
    // Orbit camera state (arrow keys = rotate, scroll = zoom).
    float g_cam_az   = 0.f;     // azimuth  (radians, 0 = south)
    float g_cam_el   = 0.70f;   // elevation (radians, ~40° — L2-style default)
    float g_cam_dist = 22.f;    // distance from target
#if defined(MONKEY_DUST_EDITOR) || defined(MD_PERF_TEST_HOOKS)
    // terrain-perf-measure (2026-08-13): wires md.set_camera_orbit to this
    // loop's own orbit-camera state -- must happen here, not in InitGame(),
    // since g_cam_az/el/dist don't exist until this point (RegisterLuaScena
    // rioAPI, called inside InitGame(), only wires cam_x/z which ARE
    // declared before InitGame()). See RegisterLuaScenarioOrbitCamera's doc
    // comment (lua_scenario_api.cpp).
    RegisterLuaScenarioOrbitCamera(&g_cam_az, &g_cam_el, &g_cam_dist);
#endif
    char  save_msg[64]     = "";
    float save_msg_timer   = 0.0f;

    // Click-to-move target — set in UpdateOrbitCamera, consumed in HandlePlayerMove.
    static float        s_ctm_x = 0.f, s_ctm_z = 0.f;
    static bool         s_ctm_active = false;

    if (const char* e = getenv("MD_FPS_CAP")) window_set_fps_cap(strcmp(e, "auto") == 0 ? -1 : atoi(e));   // auto (default), 60, 40, 30, 0 = unlimited (#191, #207)
    if (const char* e = getenv("MD_IDLE_FPS")) window_set_idle_fps(atoi(e));   // deep idle (#207): fps after 2 s without input, off by default
    window_note_activity();
    Vec3 cap_prev_pos = camera.pos, cap_prev_target = camera.target;

    const bool fps_log = getenv("MD_FPS_LOG") != nullptr;   // frames a second and the longest frame, printed every 5 s (fan / frame-cap checks in the normal game)
    double fps_t0 = 0, fps_last = 0, fps_max_ms = 0; int fps_n = 0;
    while (!input_should_quit())
    {
        // Software frame cap — prevents GPU/CPU spinning even if vsync is delayed.
        // iGPU (Intel HD 520): GPU + CPU share same die → GPU at 100% heats CPU.
        {
            static Uint64 s_prev_ns = 0;
            const SDL_WindowFlags wf = SDL_GetWindowFlags(_wnd::ptr());   // #207: a state that needs no smooth frames (owner 24.10: pause, unfocused / minimised window), not the lack of input
            window_set_state_fps((paused || !(wf & SDL_WINDOW_INPUT_FOCUS) || (wf & SDL_WINDOW_MINIMIZED)) ? 10 : 0);
            window_frame_cap_wait(s_prev_ns);
        }

        input_begin_frame();
        // Unified event pump: SDL_AddEventWatch (input) fires here + ImGui gets events.
        // Must be at START of frame so no mid-frame events are silently consumed.
        {
            SDL_Event _ev;
            while (SDL_PollEvent(&_ev)) {
                if (_ev.type == SDL_EVENT_KEY_DOWN || _ev.type == SDL_EVENT_MOUSE_WHEEL || (_ev.type == SDL_EVENT_MOUSE_MOTION && _ev.motion.state != 0)) g_dynres_input_event = true;
#ifdef MONKEY_DUST_EDITOR
                ImGui_ImplSDL3_ProcessEvent(&_ev);
#endif
                if (_ev.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED)
                    SDL_MaximizeWindow(_wnd::ptr());
                if (((_ev.type >= SDL_EVENT_KEY_DOWN && _ev.type <= SDL_EVENT_MOUSE_WHEEL) && !(_ev.type == SDL_EVENT_MOUSE_MOTION && _ev.motion.xrel == 0.f && _ev.motion.yrel == 0.f)) || _ev.type == SDL_EVENT_TEXT_INPUT) window_note_activity();   // keys, mouse motion / buttons / wheel
            }
        }
        if (camera.pos.x != cap_prev_pos.x || camera.pos.y != cap_prev_pos.y || camera.pos.z != cap_prev_pos.z || camera.target.x != cap_prev_target.x || camera.target.y != cap_prev_target.y || camera.target.z != cap_prev_target.z) {
            window_note_activity(); cap_prev_pos = camera.pos; cap_prev_target = camera.target;   // camera moved (input, script, follow)
        }

#if defined(MONKEY_DUST_EDITOR) || defined(MD_PERF_TEST_HOOKS)
        // Live command-file driving: polls tmp_/game_cmd/ for externally-
        // sent Lua commands (md.set_window_size/md.get_gpu_ms/md.ui.*/
        // md.screenshot/etc.). Every frame, unconditionally — same task #123
        // pattern as the editor's EditorCmdFile_Poll (tools/editor/main.cpp).
        // Split from UiDriver_Tick() (2026-08-12, MD_PERF_TEST_HOOKS option
        // doc comment) so a no-ImGui release build can still be live-driven
        // for perf measurement -- GameCmdFile_Poll itself has zero ImGui
        // dependency, only UiDriver_Tick (ImGuiTestEngine) needs it.
        GameCmdFile_Poll();
#endif
#ifdef MONKEY_DUST_EDITOR
        UiDriver_Tick();
#endif

        frame_tick();
        float dt = frame_dt();
        if (!paused) now_s += dt;
        if (save_msg_timer > 0.0f) save_msg_timer -= dt;

        // M = toggle world map
        if (input_key_pressed(KEY_M)) WorldMapUI::Get().Toggle();
        // P = pause / unpause
        if (input_key_pressed(SDL_SCANCODE_P)) paused = !paused;
#ifdef MONKEY_DUST_EDITOR
        // F3-overlay-only tab/camera-mode hotkeys — real keyboard shortcuts
        // so the Live UI-driver (game/src/ui_driver/) can drive the F3
        // overlay via md.ui.key_down/up alone, without fighting ImGui's
        // tab-click activation (see UiDriver_SelectTab's doc comment).
        // Bare letter when free; ALT+letter when the bare letter is already
        // WASD/gameplay-bound (A/S/D move the player; W is free but kept
        // reserved to avoid surprises).
        //   [O] Orbit  [F] Flythrough  [G] Game       — camera mode
        //   [C] Camera tab                             — bare, unused elsewhere
        //   [ALT+S] Scene  [ALT+A] AI  [ALT+N] Animation
        //   [ALT+L] FlowGraph  [ALT+D] Debug  [ALT+T] Settings
        if (EditorCore::Get().editor_open) {
            auto& ec = EditorCore::Get();
            if (input_key_pressed(SDL_SCANCODE_O)) { ec.cam_flying = false; ec.cam_game_mode = false; }
            if (input_key_pressed(SDL_SCANCODE_F)) { ec.cam_flying = true;  ec.cam_game_mode = false; }
            if (input_key_pressed(SDL_SCANCODE_G)) { ec.cam_flying = false; ec.cam_game_mode = true;  }
            extern void UiDriver_SelectTab(const char*);
            if (input_key_pressed(SDL_SCANCODE_C)) UiDriver_SelectTab("Camera");
            bool alt_down = input_key_down(SDL_SCANCODE_LALT) || input_key_down(SDL_SCANCODE_RALT);
            if (alt_down) {
                if (input_key_pressed(SDL_SCANCODE_S)) UiDriver_SelectTab("Scene");
                if (input_key_pressed(SDL_SCANCODE_A)) UiDriver_SelectTab("AI");
                if (input_key_pressed(SDL_SCANCODE_N)) UiDriver_SelectTab("Animation");
                if (input_key_pressed(SDL_SCANCODE_L)) UiDriver_SelectTab("FlowGraph");
                if (input_key_pressed(SDL_SCANCODE_D)) UiDriver_SelectTab("Debug");
                if (input_key_pressed(SDL_SCANCODE_T)) UiDriver_SelectTab("Settings");
            }
        }
#endif
        // Escape: close world map first, then quit
        if (input_key_pressed(KEY_ESCAPE)) {
            if (WorldMapUI::Get().IsOpen()) WorldMapUI::Get().Close();
            else break;
        }

        if (WorldMapUI::Get().IsOpen()) {
            s_ctm_active = false;
            // Scroll = zoom (must be outside orbit block — orbit block is skipped when editor open)
            {
                float scroll = input_get_scroll_y();
                if (scroll != 0.f) {
                    float mx = input_mouse_x(), my = input_mouse_y();
                    WorldMapUI::Get().Zoom(scroll, mx, my,
                                           window_get_width(), window_get_height());
                }
            }
            // WASD pan (0.3 viewport/s feels like 3D World navigation)
            const float MAP_PAN_SPD = 0.3f;
            float du = 0.f, dv = 0.f;
            if (input_key_down(KEY_A) || input_key_down(KEY_LEFT))  du -= MAP_PAN_SPD * dt;
            if (input_key_down(KEY_D) || input_key_down(KEY_RIGHT)) du += MAP_PAN_SPD * dt;
            if (input_key_down(KEY_W) || input_key_down(KEY_UP))    dv -= MAP_PAN_SPD * dt;
            if (input_key_down(KEY_S) || input_key_down(KEY_DOWN))  dv += MAP_PAN_SPD * dt;
            if (du != 0.f || dv != 0.f) WorldMapUI::Get().Pan(du, dv);
        }

        bool skip_world = false;
#ifdef MONKEY_DUST_EDITOR
        if (input_key_pressed(SDL_SCANCODE_F12))
            NpcRender::Get().anim_debug = !NpcRender::Get().anim_debug;
#endif
        HandleDebugCaptures(save_msg, sizeof(save_msg), save_msg_timer, camera);
#ifdef MONKEY_DUST_EDITOR
        if (input_key_pressed(KEY_F3)) {
            bool was_open = EditorCore::Get().editor_open;
            EditorCore::Get().editor_open ^= true;
            if (!was_open) {
                // Sync editor camera to current game camera on open
                auto& ec = EditorCore::Get();
                ec.cam_target = camera.target;
                Vec3 d = vec3_sub(camera.pos, camera.target);
                float horiz = sqrtf(d.x*d.x + d.z*d.z);
                ec.cam_dist  = sqrtf(d.x*d.x + d.y*d.y + d.z*d.z);
                ec.cam_pitch = atan2f(d.y, horiz) * (180.f / 3.14159265f);
                ec.cam_yaw   = atan2f(d.x, d.z)  * (180.f / 3.14159265f);
            }
        }
        if (EditorCore::Get().editor_open) {
            if (input_key_pressed(KEY_Z) && input_key_down(KEY_LEFT_CONTROL))
                EditorCore::Get().Undo();
            if (input_key_pressed(KEY_Y) && input_key_down(KEY_LEFT_CONTROL))
                EditorCore::Get().Redo();
            // Track game camera for smooth Game→Flythrough transition.
            if (EditorCore::Get().cam_game_mode)
                EditorCore::Get().last_game_cam_target = camera.target;
            // Game camera mode: allow normal player movement even while F3 is open.
            if (!EditorCore::Get().cam_game_mode)
                skip_world = true;
        }
#endif
        if (paused) {
            skip_world = true;
            // Zero player velocity so walk animation freezes on pause.
            auto& _pnav = MdRegistry::Get().Handle(player_entity).get_mut<NavAgent>();
            _pnav.desired_vel_x = 0.f; _pnav.desired_vel_z = 0.f;
        }
        if (!skip_world) {
            HandlePlayerMove(player_entity, cam_x, cam_z, g_cam_az, dt, s_ctm_x, s_ctm_z, s_ctm_active, NpcRender::Get().player_sprinting);
            // render-audit-2026: this call runs BEFORE FS_BEGIN("Logic")/
            // ("RenderTotal") below, in a per-frame span FrameStats never
            // covers at all (RenderTotal's own Begin() zeroes last_ms for
            // the frame-exact [FRAMESPIKE] dump, which would wipe out a
            // value set here moments earlier in the same frame) -- measured
            // directly instead, since TerrainStreamQueue::enqueue() failing
            // (queue full, CAPACITY=14) makes this fall back to a fully
            // synchronous TerrainGen_Build+Upload+AddTerrainMesh+ClutterGen
            // right here, unrelated to the already-diagnosed TerrainStreamPoll
            // cost inside RenderTotal (docs/RENDER_AUDIT_2026.md §11.3).
            {
                double t0 = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                HandleTerrainStreaming(cam_x, cam_z, gs);
                double ms = (std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count() - t0) * 1000.0;
                if (ms > 3.0)
                    fprintf(stderr, "[STREAMSYNC] HandleTerrainStreaming=%.1fms\n", ms);
            }

        }

#ifdef MONKEY_DUST_EDITOR
        HandleFlythroughStreaming();
#endif  // MONKEY_DUST_EDITOR

        HandleCamButtonClick();
#ifdef MONKEY_DUST_EDITOR
        if (!EditorCore::Get().editor_open || EditorCore::Get().cam_game_mode)
#endif
        {
            UpdateOrbitCamera(camera, cam_x, cam_z,
                              g_cam_az, g_cam_el, g_cam_dist,
                              s_ctm_x, s_ctm_z, s_ctm_active,
                              dt, save_msg, save_msg_timer);
        }

        FS_BEGIN("Logic");
        // no logic tick: the world clock runs (LightSystem::Tick(gs.game_time_hours) in RenderFrame drives day / night)
        gs.game_time_hours = LightSystem::Get().time_override_enabled ? LightSystem::Get().time_override_hours : fmodf(now_s / 60.f, 24.f);
        gs.now_s = now_s;
        gs.delta_time = dt;
        FS_END("Logic");

        FS_BEGIN("RenderTotal");
        RenderDocHook_BeginFrame();
        RenderFrame(camera, gs, player_entity, cam_x, cam_z, g_cam_az, logic_accum, now_s, dt, paused);
        RenderDocHook_EndFrame();
        FS_END("RenderTotal");
        if (fps_log) {
            const double t = (double)SDL_GetTicksNS() * 1e-9;
            if (fps_t0 == 0) { fps_t0 = fps_last = t; }
            ++fps_n; fps_max_ms = std::max(fps_max_ms, (t - fps_last) * 1000.0); fps_last = t;
            if (t - fps_t0 >= 5.0) { fprintf(stderr, "[FPS] %.1f fps, longest frame %.1f ms\n", fps_n / (t - fps_t0), fps_max_ms); fps_t0 = t; fps_n = 0; fps_max_ms = 0; }
        }
    }  // while (!input_should_quit())

    ShutdownGame(player_entity);
    return 0;
}
