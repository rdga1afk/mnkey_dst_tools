#include "game_common.h"
#include "render/sky_moons.h"
#include "render/sky_clouds.h"
#include "render/sky_panorama.h"

// ─── RenderFrame ──────────────────────────────────────────────────────────────
// Per-frame render pass: particle update, ImGui overlay, 3D scene,
// 2D HUD, and ring-buffer advance.
void RenderFrame(const MdCamera& camera,
                 const GameState& gs,
                 MdEntity player_entity,
                 float cam_x, float cam_z,
                 float g_cam_az,
                 float logic_accum,
                 float now_s,
                 float dt,
                 bool paused)
{
// render-audit-2026 (§11.2 follow-up): this whole function -- NOT
// NpcRender::RenderFrame -- is what main.cpp's FS_BEGIN/END("RenderTotal")
// actually wraps. Every code path here was previously uninstrumented;
// [FRAMESPIKE] dumps kept showing 20-40ms "unattributed" gaps because
// sum_others only ever summed NpcRender::RenderFrame's own internal
// sections, a strict subset of this function's real span.
FS_BEGIN("PreWindow");
ParticleSoA::Get().Update(dt);
FS_END("PreWindow");

FS_BEGIN("WindowBeginFrame");
window_begin_frame();
    md::GpuProfiler::Get().BeginFrame();
FS_END("WindowBeginFrame");
    MdCamera active_cam = camera;
#ifdef MONKEY_DUST_EDITOR
    // Always begin/end ImGui frame so GetDrawData() clears when nothing is drawn.
    FS_BEGIN("EditorImGui");
    {
        if (EditorCore::Get().editor_open &&
            !EditorCore::Get().cam_game_mode) {
            EditorCore::Get().UpdateEditorCamera(dt, true);
            active_cam = EditorCore::Get().editor_cam;
        }
        imgui_new_frame();


        if (paused) {
            const char* ps = "PAUSED";
            ImDrawList* fdl = ImGui::GetForegroundDrawList();
            ImVec2 tsz = ImGui::CalcTextSize(ps);
            float cx = window_get_width() * 0.5f - tsz.x * 0.5f;
            float cy = 14.f;
            fdl->AddRectFilled({cx - 10, cy - 4}, {cx + tsz.x + 10, cy + tsz.y + 4},
                               IM_COL32(0, 0, 0, 180), 3.f);
            fdl->AddText({cx, cy}, IM_COL32(255, 220, 60, 255), ps);
        }

        if (NpcRender::Get().anim_debug)
            NpcRender::Get().DrawAnimDebugWindow();

        if (EditorCore::Get().editor_open) {
            // Camera-panel abs-world-coords readout: this shift is what lets
            // tools/-side code (which cannot #include game/'s SceneRender)
            // convert its own session-local Position/Target into absolute
            // Kenshi metres -- see EditorCore::scene_world_dx's doc comment
            // and SceneRender::GraniteAbsCam (scene_render.cpp) for the
            // formula this mirrors.
            auto& sr_ec = SceneRender::Get();
            EditorCore::Get().scene_world_dx = (float)sr_ec.zone_ox * CHUNK_SIZE - sr_ec.tnoff_x;
            EditorCore::Get().scene_world_dz = (float)sr_ec.zone_oz * CHUNK_SIZE - sr_ec.tnoff_z;
            EditorCore::Get().Update(dt);
        }

        // World map zone labels — always drawn when map is open (editor not required)
        if (WorldMapUI::Get().IsOpen()) {
            int sw_m = window_get_width(), sh_m = window_get_height();
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImU32 col_label  = IM_COL32(255, 255, 210, 220);
            const ImU32 col_shadow = IM_COL32(0, 0, 0, 200);
            const ImU32 col_cur    = IM_COL32(255, 100, 80, 255);
            const char* cur_zone   = WorldRegistry::Get().CurrentZone();
            ImFont* fnt  = ImGui::GetFont();
            float   fsz  = ImGui::GetFontSize() * 0.95f;
            float ml, mt, mw, mh;
            WorldMapUI::Get().GetMapRect(sw_m, sh_m, ml, mt, mw, mh);
            for (int i = 0; i < WorldMapUI::Get().PinCount(); ++i) {
                ZonePin p = WorldMapUI::Get().GetPin(i);
                int px, py;
                WorldMapUI::Get().GetPinScreenPos(i, sw_m, sh_m, px, py);
                // skip labels outside the visible map square (not the full screen —
                // the map viewport is a centred 90%-height square, so pins panned/
                // zoomed off-window must not render past its edges)
                if ((float)px < ml || (float)px > ml + mw || (float)py < mt || (float)py > mt + mh) continue;
                bool is_cur = (cur_zone && !strcmp(p.slug, cur_zone));
                ImU32 tc = is_cur ? col_cur : col_label;
                ImVec2 pos((float)px + 6, (float)py - 7);
                dl->AddText(fnt, fsz, ImVec2(pos.x+1, pos.y+1), col_shadow, p.label);
                dl->AddText(fnt, fsz, pos, tc, p.label);
            }
        }
        imgui_render();
    }
    FS_END("EditorImGui");
#endif
    FS_BEGIN("WindowBegin3D");
    window_begin_3d(active_cam);
    FS_END("WindowBegin3D");

// ── Item 5: poll async terrain build queue — upload + physics + props ────────
// render-audit-2026: candidate for occasional large stalls -- fires once
// per finished background chunk load, doing a synchronous Jolt body swap
// (AddTerrainMesh builds a HeightFieldShape) + PropGen_Build on the render
// thread. Usually a no-op (early-return when no chunk finished this frame).
    FS_BEGIN("TerrainStreamPoll");
    TerrainStreamQueue::Get().poll([](TerrainBuildSlot& s) {
        if (!s.chunk || !s.chunk->loaded) return;
        // render-audit-2026: TerrainStreamPoll measured 30-56ms on 67/94
        // sampled spike frames (worst zone, docs/RENDER_AUDIT_2026.md
        // §11.2) -- PropGen_Build is a confirmed no-op (disabled
        // 2026-07-27), so splitting the remaining candidates.
        FS_BEGIN("JoltAddTerrainMesh");
        JoltWorld::Get().RemoveBody(*s.jolt_id);
        *s.jolt_id = JoltWorld::Get().AddTerrainMesh(
            s.chunk->heightmap.h, TERRAIN_GRID, TERRAIN_STEP,
            s.chunk->center_x - CHUNK_SIZE * 0.5f,
            s.chunk->center_z - CHUNK_SIZE * 0.5f);
        FS_END("JoltAddTerrainMesh");
        // Re-init TerrainQuery to pick up new chunk data
        FS_BEGIN("TerrainQueryInit");
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
        FS_END("TerrainQueryInit");
    });
    FS_END("TerrainStreamPoll");

// ── W-1: update sun direction + sky/light colors from game time ─────────────
    FS_BEGIN("LightTick");
    LightSystem::Get().Tick(gs.game_time_hours);
    SkyClouds::SetClock(LightSystem::Get().time_override_enabled ? (double)gs.game_time_hours * 60.0 : gs.now_s);   // one game minute = one second of the world clock
    SkyPanorama::SetGameSpeed(paused ? 0.f : 1.f);   // our engine has one speed (the world clock runs at 1 game minute per second); the exe's steps are 1 / 2 / 5
    SkyMoons::SetDay(LightSystem::Get().time_override_enabled ? 0 : (int)(gs.now_s / 1440.0));   // the world clock is now_s / 60 hours
    FS_END("LightTick");

// ── SDL_GPU NPC render pass (wired to NpcRender singleton) ──────────────────
    NpcRender::Get().RenderFrame(active_cam, now_s, dt,
                                 player_entity, cam_x, cam_z,
                                 g_cam_az,
                                 logic_accum);
    FS_BEGIN("WindowEnd3D");
    window_end_3d();
    FS_END("WindowEnd3D");
// Advance ring buffers after all GPU work for this frame is submitted.
FS_BEGIN("AdvanceRing");
TransformSoA::Get().AdvanceFrame();
AnimationSoA::Get().AdvanceFrame();
FS_END("AdvanceRing");
// Per-pass GpuProfiler CPU-submission timings to stderr (2026-08-24) --
// previously nothing read GpuProfiler::GetResult() outside the editor's
// own ImGui panel (tools/editor/editor_gpu_profiler_panel.cpp), so the
// game binary had no way to report per-pass cost at all. Note: these are
// CPU submission times, not GPU execution times -- SDL3 GPU exposes no
// timestamp-query API (see terrain_page_fill.comp's own doc comment).
// Throttled to once per ~3s like FrameStats' own report cadence.
{
    FS_BEGIN("GpuPassDump");
    static float s_dump_accum = 0.f;
    s_dump_accum += dt;
    if (s_dump_accum >= 3.0f) {
        s_dump_accum = 0.f;
        char buf[768]; char* p = buf;
        p += snprintf(p, 64, "[GPUPASS] total=%.2fms | ", md::GpuProfiler::Get().TotalMs());
        for (int i = 0; i < md::GpuProfiler::Get().ResultCount(); ++i) {
            const auto& r = md::GpuProfiler::Get().GetResult(i);
            p += snprintf(p, (size_t)(buf + sizeof(buf) - p), "%s=%.2fms ", r.name, r.cpu_ms);
        }
        fprintf(stderr, "%s\n", buf);
    }
    FS_END("GpuPassDump");
}
FS_BEGIN("WindowEndFrame");
window_end_frame();
FS_END("WindowEndFrame");
}  // RenderFrame
