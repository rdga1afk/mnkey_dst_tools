#ifdef MONKEY_DUST_EDITOR
#include "editor_toolbar.h"
#include <SDL3/SDL_events.h>
#include "editor_command_palette.h"
#include <monkey_dust/editor/cmd_registry.h>
#include "editor_std_commands.h"
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/renderable.h>
#include "editor_game_context.h"
#ifndef MONKEY_DUST_STANDALONE_EDITOR
#endif
#include "icon_definitions.h"
#include <monkey_dust/platform/md_log.h>
#include "editor_console.h"
#include <monkey_dust/world/transform_soa.h>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
void EditorToolbar::Draw(float dt) {
    // Update shared frame stats before any panel reads them.
    auto& ec = EditorCore::Get();
    float fps = (dt > 0.001f) ? 1.0f / dt : 0.f;
    ec.frame_fps   = fps;
    ec.frame_dt_ms = dt * 1000.f;
    EditorConsole::Get().SetFrameStats(fps, dt * 1000.f);

    DrawMenuBar();
    DrawButtonBar();
    EditorCommandPalette::Get().Draw();

    // Hotkeys — only when ImGui doesn't want keyboard
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) ec.gizmo_op    = EditorGizmoOp::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) ec.gizmo_op    = EditorGizmoOp::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) ec.gizmo_op    = EditorGizmoOp::SCALE;
        if (ImGui::IsKeyPressed(ImGuiKey_G, false)) ec.gizmo_space = (ec.gizmo_space == EditorGizmoSpace::WORLD)
                                                   ? EditorGizmoSpace::LOCAL
                                                   : EditorGizmoSpace::WORLD;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) ec.FocusOnSelected();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorToolbar::DrawMenuBar() {
    // BeginMainMenuBar always renders at Y=0 and cannot be offset.
    // When MD_OVERLAY_TOP_OFFSET is set (RenderDoc mode), use an explicit window
    // positioned below the overlay so it is not covered by the RenderDoc text.
    extern float s_overlay_top;   // defined in main.cpp / editor_core.cpp
    float menu_h = ImGui::GetFrameHeight();
    if (s_overlay_top > 0.f) {
        ImGui::SetNextWindowPos({0.f, s_overlay_top});
        ImGui::SetNextWindowSize({ImGui::GetIO().DisplaySize.x, menu_h});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
        bool open = ImGui::Begin("##MainMenuBarOffset", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_MenuBar);
        ImGui::PopStyleVar(3);
        if (!open || !ImGui::BeginMenuBar()) { ImGui::End(); return; }
    } else {
        if (!ImGui::BeginMainMenuBar()) return;
    }

    // ── File ─────────────────────────────────────────────────
    if (ImGui::BeginMenu("File")) {
        ImGui::Separator();
        // Phase 1.3 (EDITOR_AUTOMATION_PLAN_v1.md): New Scene/Save/Load
        // dispatch through EditorCmdRegistry — this menu used to carry its
        // own copy of the same logic as the command palette's "Scene: ..."
        // entries (now also migrated), violating the plan's "one execution
        // path" invariant. Import/Export take this file's own scene_path_
        // as their "path" CmdArgs string.
        if (ImGui::MenuItem("Exit Editor")) {
            EditorCore::Get().editor_open = false;
            SDL_Event e = {}; e.type = SDL_EVENT_QUIT; SDL_PushEvent(&e);
        }
        ImGui::EndMenu();
    }

    // ── Edit ─────────────────────────────────────────────────
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem(ICON_UNDO " Undo", "Ctrl+Z"))
            EditorCore::Get().Undo();
        if (ImGui::MenuItem(ICON_REDO " Redo", "Ctrl+Y"))
            EditorCore::Get().Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            CmdArgs args;
            DispatchEditorCmd("Duplicate Selected", args);
        }
        if (ImGui::MenuItem("Delete", "Del")) {
            CmdArgs args;
            DispatchEditorCmd("Delete Selected", args);
        }
        if (ImGui::MenuItem("Select All", "Ctrl+A")) {
            CmdArgs args;
            DispatchEditorCmd("Select All", args);
        }
        ImGui::EndMenu();
    }

    // ── View ─────────────────────────────────────────────────
    if (ImGui::BeginMenu("View")) {
        auto& pv = EditorCore::Get().panels_visible;
#ifndef MONKEY_DUST_STANDALONE_EDITOR
        ImGui::MenuItem("Hierarchy",          nullptr, &pv[0]);
        ImGui::MenuItem("Inspector",          nullptr, &pv[1]);
        ImGui::MenuItem("Console",            nullptr, &pv[3]);
        ImGui::MenuItem("Camera",             nullptr, &pv[5]);
        ImGui::MenuItem("Animation",          nullptr, &pv[6]);
        ImGui::Separator();
        ImGui::MenuItem("FlowGraph",          nullptr, &pv[9]);
        ImGui::MenuItem("Sequencer",          nullptr, &pv[13]);
        ImGui::MenuItem("GPU Profiler",       nullptr, &pv[11]);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            for (int i = 0; i < 15; ++i) pv[i] = false;
            pv[0] = pv[1] = pv[3] = pv[5] = pv[6] = true;
        }
#else
        ImGui::TextDisabled("Panels are embedded in tabs");
#endif
        ImGui::EndMenu();
    }

    // ── Scene ─────────────────────────────────────────────────
    if (ImGui::BeginMenu("Scene")) {
        if (ImGui::MenuItem("Bake Lights (stub)")) {
            MD_LOG(MD_LOG_INFO, "[Editor] Bake not available: Phase 33 CSM is runtime-only");
        }
        ImGui::EndMenu();
    }

#ifndef MONKEY_DUST_STANDALONE_EDITOR
    // ── Debug ─────────────────────────────────────────────────
    if (ImGui::BeginMenu("Debug")) {
        ImGui::MenuItem("Physics Paused", nullptr, &EditorCore::Get().physics_paused);
        ImGui::EndMenu();
    }
#endif

    // ── Help ──────────────────────────────────────────────────
    if (ImGui::BeginMenu("Help")) {
        ImGui::Text("monkey_dust Editor — Phase 34");
        ImGui::Text("F3 = toggle editor | W/E/R = gizmo");
        ImGui::Text("F = focus | G = local/world | Ctrl+Z/Y = undo/redo");
        ImGui::EndMenu();
    }

    if (s_overlay_top > 0.f) { ImGui::EndMenuBar(); ImGui::End(); }
    else                     { ImGui::EndMainMenuBar(); }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorToolbar::DrawButtonBar() {
    extern float s_overlay_top;
    float menu_h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(0, s_overlay_top + menu_h));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 30));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##EditorToolbar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    auto& ec = EditorCore::Get();

    // [+] new entity popup
    if (ImGui::Button(ICON_NEW_SCENE " New Entity")) ImGui::OpenPopup("NewEntityPopup");
    if (ImGui::BeginPopup("NewEntityPopup")) {
        if (ImGui::MenuItem("Transform (empty)")) SpawnEntity("Transform");
        if (ImGui::MenuItem("Light (Phase 36)"))  MD_LOG(MD_LOG_INFO, "[Editor] Light entity: Phase 36");
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    // Gizmo mode buttons
    bool t_active = (ec.gizmo_op == EditorGizmoOp::TRANSLATE);
    bool r_active = (ec.gizmo_op == EditorGizmoOp::ROTATE);
    bool s_active = (ec.gizmo_op == EditorGizmoOp::SCALE);

    if (t_active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_TRANSLATE)) ec.gizmo_op = EditorGizmoOp::TRANSLATE;
    if (t_active) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (r_active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_ROTATE)) ec.gizmo_op = EditorGizmoOp::ROTATE;
    if (r_active) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (s_active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_SCALE)) ec.gizmo_op = EditorGizmoOp::SCALE;
    if (s_active) ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    // Space toggle
    bool is_global = (ec.gizmo_space == EditorGizmoSpace::WORLD);
    if (is_global) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_GLOBAL)) {
        ec.gizmo_space = is_global ? EditorGizmoSpace::LOCAL : EditorGizmoSpace::WORLD;
    }
    if (is_global) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s space", is_global ? "World" : "Local");

    ImGui::SameLine();

    // Physics pause
    if (ec.physics_paused) ImGui::PushStyleColor(ImGuiCol_Button, {0.7f, 0.3f, 0.1f, 1.f});
    if (ImGui::Button(ICON_PHYSICS)) ec.physics_paused = !ec.physics_paused;
    if (ec.physics_paused) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Physics: %s", ec.physics_paused ? "PAUSED" : "RUNNING");

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    // FPS
    char fps_buf[32];
    snprintf(fps_buf, sizeof(fps_buf), "%.0f FPS", EditorCore::Get().frame_fps);
    ImGui::TextDisabled("%s", fps_buf);

    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorToolbar::SpawnEntity(const char* type) {
    auto& reg = MdRegistry::Get();
    auto& ec  = EditorCore::Get();
    Vec3 pos = ec.cam_target;

    if (strcmp(type, "Transform") == 0) {
        auto e = reg.Create();
        reg.Handle(e).emplace<WorldTransform>();
        auto& tr = reg.Handle(e).get_mut<WorldTransform>();
        tr.x = pos.x; tr.y = 0.f; tr.z = pos.z; tr.rot_y = 0.f;
        tr.slot = TransformSoA::Get().Alloc(e, pos.x, pos.z, 0);
        ec.Select(e);
        MD_LOG(MD_LOG_INFO, "[Editor] Spawned Transform entity at (%.1f,%.1f)", pos.x, pos.z);
        return;
    }

}
#endif
