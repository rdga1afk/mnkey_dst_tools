#include "game_common.h"
#include <monkey_dust/render/ambient_probe.h>
#include "render/ground_live_driver.h"

// Shared by F9/F10 (below) — same `import`-based capture, factored out so F9
// can pair a screenshot with its bug report under one matching timestamp
// instead of requiring a separate F10 press (see F9's own comment for why
// this pairing was added).
static void CaptureScreenshotTo(const char* path)
{
    char cmd[512];
    SDL_PropertiesID props = SDL_GetWindowProperties(_wnd::ptr());
    unsigned long x11_id  = (unsigned long)SDL_GetNumberProperty(
        props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (x11_id)
        snprintf(cmd, sizeof(cmd),
            "import -window %lu '%s' >/dev/null 2>&1 &", x11_id, path);
    else
        snprintf(cmd, sizeof(cmd),
            "import -window root -crop %dx%d+0+0 '%s' >/dev/null 2>&1 &",
            window_get_width(), window_get_height(), path);
    system(cmd);
}

// ─── HandleDebugCaptures ────────────────────────────────────────────────────
// F10 = screenshot to tools/qa/captures/, F9 = bug report + screenshot to
// temp_/ (2026-09-19: was tmp_/, reserved for permanent RE data).
void HandleDebugCaptures(char* save_msg, size_t save_msg_size, float& save_msg_timer, const MdCamera& camera)
{
// Screenshot: F10 → tools/qa/captures/cap_NNN.png
if (input_key_pressed(SDL_SCANCODE_F10)) {
    static int sc_counter = 0;
    char sc_name[64], sc_path[256];
    snprintf(sc_name, sizeof(sc_name), "cap_%04d.png", ++sc_counter);
    snprintf(sc_path, sizeof(sc_path), "tools/qa/captures/%s", sc_name);
    CaptureScreenshotTo(sc_path);
    snprintf(save_msg, save_msg_size, "[F10] %s  saved", sc_name);
    save_msg_timer = 2.0f;
}
// Bug capture: F9 → temp_/bug_game_TIMESTAMP.txt + temp_/bug_game_TIMESTAMP.png
//
// task bug-repro-position-blind-spot (2026-07-31): the single biggest time
// sink this session was NOT knowing where in the world a reported visual
// artifact actually was -- screenshots alone carry no coordinates, and
// asking the user to separately report position after the fact repeatedly
// failed to converge. F9 already dumped px/pz + nearby NPCs but never a
// screenshot, camera orientation, or player height -- so a bug report and
// the image showing the bug were two disconnected artifacts that had to be
// correlated by hand, and yaw/pitch (needed to reproduce the exact view via
// md.set_camera_pose) wasn't captured at all. Now one F9 press writes BOTH
// a matching-timestamp .txt (px/py/pz/yaw/pitch/zone + NPCs) AND a .png of
// the exact same frame -- fully self-describing, no follow-up question
// needed to reproduce the view headlessly.
if (input_key_pressed(SDL_SCANCODE_F9)) {
    char bc_stem[256], bc_path[288], bc_png[288];
    time_t bc_now = time(nullptr);
    struct tm* bc_lt = localtime(&bc_now);
    snprintf(bc_stem, sizeof(bc_stem),
        "bug_game_%04d%02d%02d_%02d%02d%02d",
        bc_lt->tm_year+1900, bc_lt->tm_mon+1, bc_lt->tm_mday,
        bc_lt->tm_hour, bc_lt->tm_min, bc_lt->tm_sec);
    snprintf(bc_path, sizeof(bc_path), "temp_/%s.txt", bc_stem);
    snprintf(bc_png, sizeof(bc_png), "temp_/%s.png", bc_stem);
    FILE* bc_f = fopen(bc_path, "w");
    if (bc_f) {
        fprintf(bc_f, "# monkeydust BugCapture — game\n");
        fprintf(bc_f, "# %04d-%02d-%02d %02d:%02d:%02d\n",
            bc_lt->tm_year+1900, bc_lt->tm_mon+1, bc_lt->tm_mday,
            bc_lt->tm_hour, bc_lt->tm_min, bc_lt->tm_sec);
        fprintf(bc_f, "# screenshot: %s\n\n", bc_png);
        Vec3 d = vec3_sub(camera.target, camera.pos);
        float horiz = sqrtf(d.x * d.x + d.z * d.z);
        float cam_pitch = atan2f(d.y, horiz) * (180.f / 3.14159265f);
        float cam_yaw   = atan2f(d.x, d.z)   * (180.f / 3.14159265f);
        fprintf(bc_f, "[Camera]  (reproduce via md.set_camera_pose)\n");
        fprintf(bc_f, "  x=%.3f y=%.3f z=%.3f  yaw=%.2f pitch=%.2f\n\n",
            camera.pos.x, camera.pos.y, camera.pos.z, cam_yaw, cam_pitch);
        auto& wr = WorldRegistry::Get();
        fprintf(bc_f, "[WorldRegistry]\n");
        fprintf(bc_f, "  zone=%s  px=%.3f  pz=%.3f  zones=%d\n\n",
            wr.CurrentZone(), wr.PlayerX(), wr.PlayerZ(), wr.Count());
        fprintf(bc_f, "[ECS]\n");
        fprintf(bc_f, "  entities=%zu\n\n",
            (size_t)MdRegistry::Get().Raw().query().all<MdManagedTag>().count());
        // NPC positions within 80m of player
        float _px = wr.PlayerX(), _pz = wr.PlayerZ();
        fprintf(bc_f, "[NPCs within 80m]  (slot,x,z,vx,vz,moving)\n");
        int _nc = 0;
        static auto q_p3_main_3 = MdRegistry::Get().Raw().query().all<WorldTransform>().all<NavAgent>();
        MdEach(q_p3_main_3, [&](const WorldTransform& tr, const NavAgent& nav) {
                float dx = tr.x - _px, dz = tr.z - _pz;
                if (dx*dx + dz*dz > 6400.f) return;
                fprintf(bc_f, "  slot=%d  x=%.3f z=%.3f  vx=%.3f vz=%.3f  mv=%d\n",
                    (int)tr.slot, tr.x, tr.z,
                    nav.desired_vel_x, nav.desired_vel_z, nav.is_moving ? 1 : 0);
                ++_nc;
            });
        fprintf(bc_f, "  total=%d\n", _nc);
        fclose(bc_f);
        CaptureScreenshotTo(bc_png);
        snprintf(save_msg, save_msg_size, "[F9] bug+screenshot saved");
        save_msg_timer = 2.0f;
    }
}
}  // HandleDebugCaptures


// ─── ShutdownGame ─────────────────────────────────────────────────────────────
// Ordered teardown: save state, GPU resources, workers, systems.
void ShutdownGame(MdEntity player_entity, bool save_world)
{
SceneRender::Get().StopRecording();
SceneRender::Get().WaitRecordingChild();

// Save world state + shutdown world map.
// save_world=false in scenario mode (--exec) — principle 9 (data-sandbox):
// a scenario run must never write game/data/world_registry.txt.
if (save_world) {
    auto& ptr2 = MdRegistry::Get().Handle(player_entity).get_mut<WorldTransform>();
    WorldRegistry::Get().SetPlayerPos(ptr2.x, ptr2.z);
    WorldRegistry::Get().Save();
}
// Real crash found live (gdb backtrace: SIGSEGV inside libSDL3.so from
// GpuStaticBuffer::Shutdown -> SDL_ReleaseGPUBuffer, called on NpcRender's
// SkinMesh vbo/ibo) while releasing GPU resources below with these workers
// still running -- persisted even after adding SDL_WaitForGPUIdle(), so
// this is a genuine concurrent-thread race (terrain streaming/nav/job
// workers still touching render-adjacent state), not just an in-flight-
// frame timing issue. Moved ahead of the newly-wired Shutdown() calls
// (docs/SDLGPU_FENCE_OBJECT_LEAK_BUG.md §4-5) -- was previously positioned
// after the (much smaller) original GPU cleanup block, which apparently
// never touched anything these workers actively use.
TerrainStreamQueue::Get().stop();   // Item 5: join terrain worker thread
JobSystem::Get().Shutdown();

if (auto* dev = md::GpuDevice::Get().SDLDevice()) {
    SDL_WaitForGPUIdle(dev);
    WorldMapUI::Get().Shutdown(dev);
    NpcRender::Get().body_rdtex.Release();
    // dummy_tex/samp owned by RdDevice; released in RdDevice::Shutdown below.
    NpcRender::Get().dummy_tex = nullptr; NpcRender::Get().dummy_samp = nullptr;
    // NpcRender's own SSBOs/buffers/depth-texture/mesh + player cloth/hair
    // (docs/SDLGPU_FENCE_OBJECT_LEAK_BUG.md §4-5) -- body_rdtex/dummy_tex/
    // dummy_samp already handled above/below, deliberately excluded inside
    // NpcRender::Shutdown() to avoid a double-release.
    NpcRender::Get().Shutdown();
    md::RdDevice::Get().Shutdown();
    // 789-object Vulkan validation leak audit (docs/SDLGPU_FENCE_OBJECT_LEAK_
    // BUG.md §4-5, 2026-09-11): every Shutdown() below already existed and
    // was already correct (proven live by tools/editor's/tools/flare_demo's
    // own separate instances calling the same functions) -- none of it was
    // ever reachable from the real game's shutdown path. SceneRender's own
    // tree (terrain/props/vegetation/SSBO/textures/samplers) accounts for
    // most of the leak; TransformSoA/AnimationSoA/
    // AmbientProbeSystem are separate singletons outside that tree.
    SceneRender::Get().Shutdown();
    TransformSoA::Get().Shutdown();
    AnimationSoA::Get().Shutdown();
    AmbientProbeSystem::Get().Shutdown();
}

LuaSystem::Get().Shutdown();
#ifdef MONKEY_DUST_EDITOR
EditorCmdRegistry::Get().UnregisterModule(kGameEditorModuleId);
EditorCore::Get().Shutdown();
#endif
audio_shutdown();
// Real crash found live (2 coredumps, 2026-08-11): without this, the
// Vulkan device is never explicitly waited-idle/destroyed before
// window_shutdown() below tears down the window/X11 display -- an
// Intel ANV driver-internal present thread can still be mid-
// xcb_present_pixmap when the main thread's XCloseDisplay() runs,
// corrupting xcb's shared connection state (SIGABRT, "double free or
// corruption", both coredumps show the main thread inside XCloseDisplay
// racing a libvulkan_intel.so-internal thread inside xcb_present_pixmap).
// GpuDevice::Shutdown() (gpu_device.cpp) already calls
// SDL_WaitForGPUIdle() + SDL_DestroyGPUDevice() specifically to prevent
// this class of destroy-during-use race (see its own doc comment for a
// similar async-upload case) -- tools/editor/main.cpp already calls it
// in this exact position (after app-level GPU resource cleanup, before
// window_shutdown()); the game binary never did.
if (md::GroundLiveDriver::Wanted()) md::GroundLiveDriver::Get().Shutdown();   // its worker thread uploads to the device: stopped first
md::GpuDevice::Get().Shutdown();
// Same X11/xcb teardown-order constraint as GpuDevice above, confirmed
// live for Granite's WSI in M2 Крок 3: must be destroyed before
// window_shutdown() closes the X11 display. No-op when built without
// MD_USE_GRANITE.
if (md::GraniteBackend::Get().IsReady()) md::GraniteBackend::Get().Shutdown();
window_shutdown();
}  // ShutdownGame
