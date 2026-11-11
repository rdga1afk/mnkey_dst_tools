// npc_render_debug.cpp — Debug overlays separated from the main render TU.
// Split from npc_render.cpp to reduce incremental recompile scope:
// changing debug overlay code rebuilds only this ~170-line file,
// not the full 1600-line npc_render.cpp.
#include "npc_render.h"
#include <monkey_dust/render/animation_soa.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include "../components/nav_agent.h"
#include <monkey_dust/platform/window.h>
#include <cstdio>
#ifdef MONKEY_DUST_EDITOR
#include <imgui.h>
#endif

// ── Animation Debug Overlay ───────────────────────────────────────────────────
void NpcRender::DrawAnimDebugWindow() {
#ifdef MONKEY_DUST_EDITOR
    if (!anim_debug) return;
    const auto& d = player_anim_dbg;
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(280, 200), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##AnimDbg", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored({0.4f,1.f,0.4f,1.f}, "ANIM DEBUG  [F12 off]");
    ImGui::Separator();

    auto blend_col = [](float v) { return ImVec4(v, 1.f-v, 0.2f, 1.f); };
    ImGui::TextColored(blend_col(d.bt),
        "bt     %5.3f  tgt %5.3f", d.bt, d.tgt_bt);
    ImGui::Text("spd    %5.2f  %s %s",
        d.move_speed,
        d.is_moving  ? "MOVING"  : "STOP  ",
        d.use_jog    ? "JOG"     : "WALK  ");
    ImGui::Text("phase  %7.3f  aspd %.2f", d.walk_ph, d.anim_speed);
    ImGui::Text("vx %+.2f  vz %+.2f", d.desired_vx, d.desired_vz);

    ImGui::Separator();
    ImGui::Text("blend:");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, blend_col(d.bt));
    ImGui::ProgressBar(d.bt, ImVec2(-1, 8), "");
    ImGui::PopStyleColor();

    float ph_frac = fmodf(d.walk_ph, 10.f) / 10.f;
    ImGui::Text("phase:");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, {0.3f,0.6f,1.f,1.f});
    ImGui::ProgressBar(ph_frac, ImVec2(-1, 8), "");
    ImGui::PopStyleColor();

    ImGui::End();
#endif
}

