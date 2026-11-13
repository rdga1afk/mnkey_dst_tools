#include "autonomy/scenario_driver.h"
#include "scripting/lua_scenario_api.h"
#include <monkey_dust/scripting/lua_system.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/platform/audio.h>  // frame_fps()
#include <monkey_dust/platform/job_system.h>  // Audit Phase 2b: --jobgraph-serial wiring
#include <monkey_dust/render/md_camera.h>
bool g_scenario_render_frame_pending = false;
extern bool g_dynres_input_event;   // main.cpp
bool DynresCameraInput(const SDL_Event& e);   // main.cpp
float g_scenario_last_frame_ms = 0.f;
static constexpr float LOGIC_TICK_S = 0.1f;   // one scenario tick = 0.1 s of world time (10 TPS)
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <SDL3/SDL.h>
#include <monkey_dust/render/light_system.h>
extern bool g_terrain_lod_error;  // npc_render_frame_prep.cpp (#154)
#include <monkey_dust/platform/frame_stats.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/transform_soa.h>
#include <algorithm>
#include <monkey_dust/world/terrain_stream_queue.h>
#include <monkey_dust/world/terrain_query.h>
#include <monkey_dust/physics/jolt_world.h>
#include <monkey_dust/physics/physics_terrain_region.h>
#include <monkey_dust/world/world_registry.h>
#include "render/scene_render.h"
#include <monkey_dust/render/renderdoc_hook.h> // md.renderdoc_capture (2026-07-18 diag)

// Defined (external linkage) in main.cpp — the interactive loop's per-frame
// terrain-window streaming. RunScenarioMode's --exec tick loop never called
// either, so the loaded 9x9 chunk window could sit many km from wherever a
// script pointed the camera, no matter how many ticks it waited (task
// #158f/g).
extern float g_dynres_cur;
void HandleTerrainStreaming(float cam_x, float cam_z, GameState& gs);
#ifdef MONKEY_DUST_EDITOR
void HandleFlythroughStreaming();
void ScenarioPacedFrameWait(uint64_t& prev_ns);   // main.cpp: the frame cap wait of the real loop
void ScenarioNoteActivity();
#endif

#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"
#include "editor_screenshot.h"
#include "render/npc_render.h"
#endif

// ── CLI parsing ────────────────────────────────────────────────────────────
static bool StrEq(const char* a, const char* b) { return strcmp(a, b) == 0; }

bool ParseScenarioArgs(int argc, char** argv, ScenarioConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", flag); return nullptr; }
            return argv[++i];
        };
        if (StrEq(a, "--exec")) {
            const char* v = next("--exec"); if (!v) return false;
            cfg.active = true;
            snprintf(cfg.script_path, sizeof(cfg.script_path), "%s", v);
        } else if (StrEq(a, "--seed")) {
            const char* v = next("--seed"); if (!v) return false;
            cfg.seed = (unsigned)strtoul(v, nullptr, 10);
        } else if (StrEq(a, "--max-frames")) {
            const char* v = next("--max-frames"); if (!v) return false;
            cfg.max_frames = strtol(v, nullptr, 10);
        } else if (StrEq(a, "--max-seconds")) {
            const char* v = next("--max-seconds"); if (!v) return false;
            cfg.max_seconds = strtod(v, nullptr);
        } else if (StrEq(a, "--fast")) {
            cfg.fast = true;
        } else if (StrEq(a, "--jobgraph-serial")) {
            cfg.jobgraph_serial = true;
        } else if (StrEq(a, "--headless")) {
            cfg.headless = true;
        } else if (StrEq(a, "--capture-dir")) {
            const char* v = next("--capture-dir"); if (!v) return false;
            snprintf(cfg.capture_dir, sizeof(cfg.capture_dir), "%s", v);
        } else if (StrEq(a, "--data-dir")) {
            const char* v = next("--data-dir"); if (!v) return false;
            snprintf(cfg.data_dir, sizeof(cfg.data_dir), "%s", v);
        }
        // Unrecognized args are ignored on purpose — no strict allowlist,
        // keeps room for engine/SDL passthrough flags later.
    }
    return true;
}

// ── Failure-artifact collection (principle 10) ──────────────────────────────
// Every fail path (assert_*, wait_until timeout, uncaught Lua error,
// watchdog) funnels through ONE place: RunScenarioMode's catch-all below.
// See docs/AUTONOMY_LOG.md — screenshot is NOT implemented in Etap 1, so
// the artifact omits it (documented, not silently missing).

static constexpr int LOG_TAIL_LINES = 200;
static constexpr int LOG_TAIL_LINE_LEN = 256;
static char s_log_tail[LOG_TAIL_LINES][LOG_TAIL_LINE_LEN];
static int  s_log_tail_head  = 0;
static int  s_log_tail_count = 0;

static void ScenarioLogHook(int level, const char* msg) {
    snprintf(s_log_tail[s_log_tail_head], LOG_TAIL_LINE_LEN, "%s", msg);
    s_log_tail_head = (s_log_tail_head + 1) % LOG_TAIL_LINES;
    if (s_log_tail_count < LOG_TAIL_LINES) ++s_log_tail_count;

    // MD_LOG(level,...) prints to stderr ONLY when no hook is installed
    // (md_log.h) — installing this hook to capture the tail for failure
    // artifacts would otherwise silently swallow every log line for the
    // rest of the run. Must still print, or scenario runs go dark.
    fprintf(stderr, "[%s] %s\n",
            level == MD_LOG_ERROR ? "ERROR" : level == MD_LOG_WARNING ? "WARN" : "INFO", msg);
}

static void WriteFailureArtifact(const ScenarioConfig& cfg, const char* reason) {
    char run_id[64];
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(run_id, sizeof(run_id), "%04d%02d%02d_%02d%02d%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    char dir[512];
    snprintf(dir, sizeof(dir), "temp_/failures/%s", run_id);
    // mkdir -p (two levels: temp_/failures/ may not exist yet). temp_/, not
    // tmp_/ (2026-09-19) -- tmp_/ is reserved for permanent RE data.
    mkdir("temp_", 0755);
    mkdir("temp_/failures", 0755);
    mkdir(dir, 0755);

    char dump_path[560];
    snprintf(dump_path, sizeof(dump_path), "%s/dump_state.json", dir);
    LuaSystem::Get().Exec((std::string("md.dump_state('") + dump_path + "')").c_str());

    char log_path[560];
    snprintf(log_path, sizeof(log_path), "%s/log_tail.txt", dir);
    if (FILE* lf = fopen(log_path, "w")) {
        int start = (s_log_tail_count < LOG_TAIL_LINES) ? 0 : s_log_tail_head;
        for (int i = 0; i < s_log_tail_count; ++i)
            fprintf(lf, "%s\n", s_log_tail[(start + i) % LOG_TAIL_LINES]);
        fclose(lf);
    }

    char repro_path[560];
    snprintf(repro_path, sizeof(repro_path), "%s/repro.txt", dir);
    if (FILE* rf = fopen(repro_path, "w")) {
        fprintf(rf, "reason: %s\n", reason);
        fprintf(rf, "repro: ./build/game/monkey_dust --exec %s --seed %u%s%s\n",
                cfg.script_path, cfg.seed,
                cfg.fast ? " --fast" : "",
                cfg.headless ? " --headless" : "");
        fclose(rf);
    }

    MD_LOG(MD_LOG_WARNING, "[Scenario] FAILED (%s) — artifact: %s", reason, dir);
}

// ── Driver loop ──────────────────────────────────────────────────────────────
static void PollTerrainStreams() {
    TerrainStreamQueue::Get().poll([](TerrainBuildSlot& s) {
        if (!s.chunk || !s.chunk->loaded) return;
        JoltWorld::Get().RemoveBody(*s.jolt_id);
        *s.jolt_id = JoltWorld::Get().AddTerrainMesh(
            s.chunk->heightmap.h, TERRAIN_GRID, TERRAIN_STEP,
            s.chunk->center_x - CHUNK_SIZE * 0.5f,
            s.chunk->center_z - CHUNK_SIZE * 0.5f);
        TerrainQuery::Get().Init(&SceneRender::Get().terrain_chunks[0][0],
                                 SceneRender::TNKN, CHUNK_SIZE,
                                 SceneRender::Get().tnoff_x,
                                 SceneRender::Get().tnoff_z,
                                 SceneRender::Get().base_cx,
                                 SceneRender::Get().base_cz,
                                 SceneRender::Get().zone_ox,
                                 SceneRender::Get().zone_oz);
        PhysicsTerrainRegion::Get().SetOrigin(
            SceneRender::Get().tnoff_x, SceneRender::Get().tnoff_z,
            SceneRender::Get().zone_ox * CHUNK_SIZE, SceneRender::Get().zone_oz * CHUNK_SIZE);
    });
}

int RunScenarioMode(const ScenarioConfig& cfg,
                    MdEntity player_entity,
                    GameState& gs,
                    MdCamera& /*camera*/, float& cam_x, float& cam_z)
{
    MdLogSetHook(ScenarioLogHook);

    // Audit Phase 2b (TSan falsification pair, AUDIT_PLAN_v1.md): was parsed
    // but never wired anywhere — see JobSystem::SetForceSerial's doc comment
    // for the methodology this enables. Must be set before any code path
    // calls JobSystem::Get().Submit() this session.
    if (cfg.jobgraph_serial) JobSystem::Get().SetForceSerial(true);

    ScenarioRng_Seed(cfg.seed);
    RegisterLuaScenarioAPI(&cam_x, &cam_z);

    // main.cpp's interactive loop resyncs cam_x/cam_z from the player
    // entity's real WorldTransform every frame (player_input.h's
    // HandlePlayerMove, called right before HandleTerrainStreaming below) —
    // RunScenarioMode never calls it, so cam_x/cam_z stay at main()'s
    // (0.f, 5.f) placeholder (the old atlas-corner spawn, task #158) for the
    // whole run. Do the same resync once here so terrain streaming centres
    // on where the player actually spawned, not on that stale placeholder.
    {
        const auto& ptr = MdRegistry::Get().Handle(player_entity).get_mut<WorldTransform>();
        cam_x = ptr.x;
        cam_z = ptr.z;
    }

    FILE* sf = fopen(cfg.script_path, "rb");
    if (!sf) {
        fprintf(stderr, "[Scenario] cannot open script: %s\n", cfg.script_path);
        return 1;
    }
    fseek(sf, 0, SEEK_END);
    long sz = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    static char s_script_buf[1 << 20];  // 1MB cap, no heap
    if (sz <= 0 || sz >= (long)sizeof(s_script_buf)) {
        fprintf(stderr, "[Scenario] script too large or empty: %s\n", cfg.script_path);
        fclose(sf);
        return 1;
    }
    size_t nread = fread(s_script_buf, 1, (size_t)sz, sf);
    s_script_buf[nread] = '\0';
    fclose(sf);

    // n_ticks arg (`...` in the script) — --max-frames if set, else a
    // generous default; scripts loop on their own via wait_ticks/wait_until,
    // this value is informational (scripts don't have to consume it).
    lua_Integer n_ticks_arg = cfg.max_frames > 0 ? cfg.max_frames : 100000;

    char parse_err[256] = {};
    if (!LuaSystem::Get().StartScenario(s_script_buf, n_ticks_arg, parse_err, sizeof(parse_err))) {
        fprintf(stderr, "[Scenario] parse error in %s: %s\n", cfg.script_path, parse_err);
        return 1;
    }

    long   frame_count = 0;
    // Real wall-clock, not clock() (CPU time) — the non-fast path below
    // sleeps between ticks (nanosleep), during which clock() does not
    // advance, which would make --max-seconds massively undercount actual
    // elapsed time for any non-fast run.
    auto wall_now = []() -> double {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    };
    double wall_start_s = wall_now();
    float  now_s = 0.f;

    for (;;) {
        // Watchdog — principle 6: process cannot hang forever.
        if (cfg.max_frames > 0 && frame_count >= cfg.max_frames) {
            WriteFailureArtifact(cfg, "watchdog: max-frames exceeded");
            return 124;
        }
        double wall_now_s = wall_now();
        if (cfg.max_seconds > 0.0 && (wall_now_s - wall_start_s) >= cfg.max_seconds) {
            WriteFailureArtifact(cfg, "watchdog: max-seconds exceeded");
            return 124;
        }

        LuaSystem::ScenarioResult r = LuaSystem::Get().ResumeScenario();

        if (g_terrain_lod_error) {
            printf("[Scenario] FAILED: terrain LOD is on but unavailable (KENSHI_DIR / fullmap.tif, see the [TerrainLod] error); measuring on the flat tiling is refused\n");
            WriteFailureArtifact(cfg, "terrain LOD unavailable");
            return 3;
        }
        if (r.status == LuaSystem::ScenarioStatus::Failed) {
            printf("[Scenario] FAILED: %s\n", r.error_msg);
            WriteFailureArtifact(cfg, r.error_msg);
            return 1;
        }
        if (r.status == LuaSystem::ScenarioStatus::Quit) {
            if (r.quit_code != 0) WriteFailureArtifact(cfg, "md.quit(nonzero)");
            return r.quit_code;
        }
        if (r.status == LuaSystem::ScenarioStatus::Finished) {
            return 0;
        }

        // Yielded — advance the world by exactly one logic tick, fixed dt,
        // no real-time coupling (principle 5 / Etap 1 --fast requirement;
        // see docs/AUTONOMY_LOG.md §Recon.5 — RunTickAndPortal's `if`, not
        // `while`, is why the driver calls RunLogicTick directly here
        // instead of going through the interactive loop's accumulator).
        now_s += LOGIC_TICK_S;
        {
            // the one effect of the logic tick the world needs -- cam_x/cam_z follow the player (UpdateGameState,
            // logic_tick_orchestration.cpp:14). Without it the terrain window does not re-centre on md.teleport_player in the same
            // tick, the camera pose is later rebased by the window shift and its local coordinates differ in the last float digits
            // (~0.1 mm), which changes fine ground detail by 1-2 levels over most of the frame (south_hive).
            const auto& pwt = MdRegistry::Get().Handle(player_entity).get_mut<WorldTransform>();
            cam_x = pwt.x;
            cam_z = pwt.z;
            // and the world clock (UpdateGameState, :22-24): LightSystem::Tick(gs.game_time_hours) in RenderFrame drives day/night
            gs.game_time_hours = LightSystem::Get().time_override_enabled ? LightSystem::Get().time_override_hours : fmodf(now_s / 60.f, 24.f);
            gs.now_s = now_s;
        }

        // Mirrors the interactive loop's per-frame terrain-window streaming
        // (main.cpp), which RunScenarioMode never called — so the loaded 9x9
        // chunk window never followed either cam_x/cam_z (md.teleport_camera)
        // or the F3 flythrough camera (md.set_camera_pose), no matter how
        // long a script waited (task #158f/g). Gated on !headless: shifting
        // the window uploads new chunk VBOs/IBOs to the GPU (TerrainGen_
        // Upload), same reasoning as the screenshot gate below.
        //
        // Mutually exclusive with HandleFlythroughStreaming, exactly like
        // the interactive loop: there, HandleTerrainStreaming only runs
        // inside `if (!skip_world)`, and skip_world is true whenever the F3
        // flythrough camera is active — otherwise the two fight over the
        // same SceneRender::zone_ox/zone_oz state every tick (one pulling
        // the window toward cam_x/cam_z=player position, the other toward
        // EditorCore's cam_target=script camera), rebuilding chunks forever
        // without ever converging. Confirmed: without this guard, --exec
        // ran ~600x slower after md.set_camera_pose (135 ticks in 20s vs.
        // the usual <1s for the whole scenario) instead of hanging outright.
#ifdef MONKEY_DUST_EDITOR
        bool flythrough_active = EditorCore::Get().editor_open && !EditorCore::Get().cam_game_mode;
#else
        bool flythrough_active = false;
#endif
        if (!cfg.headless && !flythrough_active) HandleTerrainStreaming(cam_x, cam_z, gs);
#ifdef MONKEY_DUST_EDITOR
        if (!cfg.headless && flythrough_active) HandleFlythroughStreaming();
#endif

        // Task terrain-patches: TerrainStreamQueue::Get().poll() (the ONLY
        // thing that consumes an async-built chunk — uploads its GPU buffers
        // and flips loaded=true, main.cpp:1649) previously had exactly ONE
        // call site, deep in main.cpp's interactive loop, which this scenario
        // driver never runs. Confirmed via targeted diagnostics: the first
        // TerrainStreamQueue::CAPACITY (14) chunks that go through the async
        // path after any --exec-driven camera jump/teleport get PERMANENTLY
        // stuck built-but-never-uploaded (loaded stays false forever, no
        // matter how many ticks/renders follow) — every later shift-touched
        // chunk falls back to the synchronous build path once the queue
        // reports full, so only ever exactly CAPACITY chunks are affected,
        // but they are real, load-bearing near-camera chunks. Same callback
        // as main.cpp's call site (Jolt body swap + PropGen + TerrainQuery
        // re-init) so a --exec scenario's terrain streaming matches
        // interactive play instead of only half-completing.
        if (!cfg.headless) PollTerrainStreams();

        // RunScenarioMode never renders on its own (RunLogicTick only) — a
        // pending md.screenshot() needs an on-demand render+capture here.
        // Root cause of the hang this used to have (task #158f): the forward-
        // tier render path (NpcRender::DrawScene) acquired the swapchain
        // texture directly via GpuRenderPass::BeginColor; a second real
        // SDL_AcquireGPUSwapchainTexture call for this capture, later in the
        // same command buffer, deadlocked SDL_SubmitGPUCommandBufferAndAcquireFence
        // at the kernel level (confirmed via /proc/<pid>/task/*/wchan showing
        // drm_syncobj_array_wait_timeout on the hung process). Fixed by
        // routing every swapchain acquire on a frame through
        // NpcRender::AcquireSwapchainCached so there is only ever one.
        //
        // task screenshot-headless-fix (2026-07-29): was `!cfg.headless &&
        // EditorScreenshot_HasPending()` — silently discarded EVERY
        // md.screenshot() call made via tools/md_ctl run, which ALWAYS
        // passes --headless (both its `run` and default invocation paths).
        // md.screenshot() itself never blocks/reports failure (just sets a
        // pending path and returns), so this produced no error anywhere —
        // the scenario would finish normally, exit 0, with no file ever
        // written. Root-caused this session by noticing EditorScreenshot_
        // CaptureAndSubmit's own extensive stderr diagnostics (success AND
        // every failure path) never appeared in ANY of these runs' logs —
        // meaning the function was never even called, not merely failing
        // inside it. Confirmed safe to drop the headless check: main.cpp's
        // own comment confirms `--headless` only skips per-tick rendering,
        // NOT window/GPU device creation (InitGame() runs unconditionally),
        // so an on-demand render here has everything it needs regardless.
        // HasPending() alone already gates this to a no-op for the vast
        // majority of headless scenario runs that never call md.screenshot().
#ifdef MONKEY_DUST_EDITOR
        if (EditorScreenshot_HasPending() || g_scenario_render_frame_pending) {
            g_scenario_render_frame_pending = false;
            if (g_scenario_burst_paced) { static uint64_t paced_ns = 0; ScenarioPacedFrameWait(paced_ns); }   // md.set_burst_paced(true): the frame cap of the real loop also paces md.render_frame (owner 13.11: cap 60 for runs that do not measure frame cost)
            { static uint64_t prev_pc = 0; const uint64_t pc = SDL_GetPerformanceCounter(); if (prev_pc) g_scenario_last_frame_ms = (float)((double)(pc - prev_pc) * 1000.0 / (double)SDL_GetPerformanceFrequency()); prev_pc = pc; }   // md.get_frame_ms(): wall time between consecutive md.render_frame() renders (frame_dt() is not ticked on this path and returned 0)
            SDL_PumpEvents();
            {   // dynres v2 step 4 (default on, MD_DYNRES_INPUT=0 off): this loop has no event poll of its own, so the camera inputs a scenario pushes (md.push_key) are taken here, as main.cpp does
                static const bool input_on = !(getenv("MD_DYNRES_INPUT") && getenv("MD_DYNRES_INPUT")[0] == '0');
                SDL_Event ev[16]; int n = 0;
                while (input_on && (n = SDL_PeepEvents(ev, 16, SDL_GETEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_WHEEL)) > 0)
                    for (int i = 0; i < n; ++i) if (DynresCameraInput(ev[i])) g_dynres_input_event = true;
            }
            EditorCore::Get().UpdateEditorCamera(LOGIC_TICK_S, false);
            MdCamera active_cam = EditorCore::Get().editor_cam;
            RenderDocHook_BeginFrame();
            NpcRender::Get().RenderFrame(active_cam, now_s, LOGIC_TICK_S,
                                          player_entity, cam_x, cam_z,
                                          0.f, 0.f);
            RenderDocHook_EndFrame();
        }
        if (g_scenario_burst_n > 0) {
            // md.render_burst: n back-to-back frames, per-call wall time (pipelined by SubmitFrame when SyncTiming is off)
            const int n = g_scenario_burst_n;
            g_scenario_burst_n = 0;
            g_scenario_burst_ms.clear();
            g_scenario_burst_freq.clear();
            g_scenario_burst_cpu_ms.clear();
            SDL_PumpEvents();
            EditorCore::Get().UpdateEditorCamera(LOGIC_TICK_S, false);
            MdCamera burst_cam = EditorCore::Get().editor_cam;
            md::GpuDevice::Get().WaitForIdle();
            uint64_t prev = SDL_GetPerformanceCounter();
            const double freq = (double)SDL_GetPerformanceFrequency();
            auto thread_cpu_ms = []() -> double { // CPU time the render thread actually used (blocked fence/acquire waits do not count)
                struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
                return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
            };
            const bool path_mode = (int)g_scenario_path.size() >= n * 5 && n > 0;
            g_scenario_burst_dynres.clear(); g_scenario_burst_stream.clear(); g_scenario_burst_stream_ms.clear();
            uint64_t paced_prev_ns = 0;
            for (int i = 0; i < n; ++i) {
                if (g_scenario_burst_paced) { if (path_mode) ScenarioNoteActivity(); ScenarioPacedFrameWait(paced_prev_ns); }
                const double cpu0 = thread_cpu_ms();
                float stream_ms = 0.f; int shifted = 0;
                if (path_mode) {   // fixed camera timestep along the path; every tick_every frames the logic tick's streaming (player follows the camera, window shift, chunk uploads)
                    auto& sr = SceneRender::Get();
                    const float* P = &g_scenario_path[(size_t)i * 5];
                    const int ox0 = sr.zone_ox, oz0 = sr.zone_oz;
                    if (g_scenario_path_tick > 0 && i % g_scenario_path_tick == 0) {
                        const double s0 = thread_cpu_ms();
                        auto& pwt = MdRegistry::Get().Handle(player_entity).get_mut<WorldTransform>();
                        pwt.x = P[0] + sr.tnoff_x - (float)sr.zone_ox * CHUNK_SIZE;
                        pwt.z = P[1] + sr.tnoff_z - (float)sr.zone_oz * CHUNK_SIZE;
                        if (pwt.slot != TransformSoA::INVALID_SLOT) { auto& soa = TransformSoA::Get(); soa.px[pwt.slot] = pwt.x; soa.pz[pwt.slot] = pwt.z; soa.MarkTransformDirty(pwt.slot); }
                        cam_x = pwt.x; cam_z = pwt.z;
                        if (!cfg.headless) { HandleTerrainStreaming(cam_x, cam_z, gs); PollTerrainStreams(); }
                        stream_ms = (float)(thread_cpu_ms() - s0);
                    }
                    shifted = (sr.zone_ox != ox0 || sr.zone_oz != oz0) ? 1 : 0;
                    const float lx = P[0] + sr.tnoff_x - (float)sr.zone_ox * CHUNK_SIZE, lz = P[1] + sr.tnoff_z - (float)sr.zone_oz * CHUNK_SIZE;
                    float gh = 0.f; TerrainQuery::Get().GetHeight(lx, lz, gh);
                    static constexpr float DEG2R = 3.14159265f / 180.f;
                    auto& ec = EditorCore::Get();
                    ec.editor_open = true; ec.cam_game_mode = false; ec.cam_flying = true;
                    ec.cam_target = { lx, gh + P[2], lz }; ec.fly_yaw = P[3] * DEG2R; ec.fly_pitch = std::max(-1.48f, std::min(1.48f, P[4] * DEG2R));
                    EditorCore::Get().UpdateEditorCamera(g_scenario_path_dt, false);
                    burst_cam = EditorCore::Get().editor_cam;
                }
                NpcRender::Get().RenderFrame(burst_cam, now_s, path_mode ? g_scenario_path_dt : LOGIC_TICK_S, player_entity, cam_x, cam_z, 0.f, 0.f);
                g_scenario_burst_dynres.push_back(g_dynres_cur); g_scenario_burst_stream.push_back(shifted); g_scenario_burst_stream_ms.push_back(stream_ms);
                uint64_t t = SDL_GetPerformanceCounter();
                g_scenario_burst_cpu_ms.push_back((float)(thread_cpu_ms() - cpu0));
                g_scenario_burst_ms.push_back((float)((double)(t - prev) * 1000.0 / freq));
                prev = t;
                if (i % 10 == 0) {
                    int mhz = -1;
                    if (FILE* ff = fopen("/sys/class/drm/card1/gt_cur_freq_mhz", "r")) { if (fscanf(ff, "%d", &mhz) != 1) mhz = -1; fclose(ff); }
                    g_scenario_burst_freq.push_back(mhz);
                }
            }
            md::GpuDevice::Get().WaitForIdle();
            g_scenario_path.clear();
        }
#endif
        ScenarioTick_Advance();
        ++frame_count;

        // --fast: no sleep, no real-time pacing, no rendering — ticks run
        // back-to-back as fast as the CPU allows. Without --fast, pace to
        // real LOGIC_TICK_S wall-clock per tick (matches normal 10 TPS) —
        // otherwise --fast has no actual effect to verify (fast_check.lua
        // needs a genuinely slower baseline to diff against). --capture-dir
        // rendering is intentionally NOT wired in Etap 1 (main.cpp's render
        // path is deeply GPU/window-coupled — see §Recon.3 — and none of
        // the 5 VERIFY scripts need a visible frame).
        if (!cfg.fast) {
            struct timespec ts{0, (long)(LOGIC_TICK_S * 1e9)};
            nanosleep(&ts, nullptr);
        }
    }
}
