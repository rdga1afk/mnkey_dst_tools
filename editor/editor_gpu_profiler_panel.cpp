#ifdef MONKEY_DUST_EDITOR
#include "editor_gpu_profiler_panel.h"
#include "editor_core.h"
#include "imgui.h"
#include "imgui_widget_flamegraph.h"
#include <monkey_dust/render/gpu_profiler.h>
#include <monkey_dust/render/frame_counters.h>

#ifndef MONKEY_DUST_STANDALONE_EDITOR
extern void md_frame_counters(md::FrameCounters* out);   // game/src/render/frame_counters.cpp (G3); the standalone editor has no game systems
#endif

struct FlameEntry {
    float       start_ms;
    float       end_ms;
    const char* name;
};

static void FlameGetter(float* s, float* e, ImU8* lvl,
                        const char** cap, const void* data, int idx) {
    const FlameEntry* arr = static_cast<const FlameEntry*>(data);
    if (s)   *s   = arr[idx].start_ms;
    if (e)   *e   = arr[idx].end_ms;
    if (lvl) *lvl = 0;
    if (cap) *cap = arr[idx].name;
}

void EditorGpuProfilerPanel::Draw() {
    if (!EditorCore::Get().panels_visible[11]) return;

    ImGui::SetNextWindowSize(ImVec2(420, 380), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(980, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("GPU Profiler##gp", &EditorCore::Get().panels_visible[11])) {
        ImGui::End(); return;
    }
    DrawContent();
    ImGui::End();
}

void EditorGpuProfilerPanel::DrawContent() {

    auto& prof = md::GpuProfiler::Get();
    int   n    = prof.ResultCount();
    if (n > 64) n = 64;
    float tot  = prof.TotalMs();

    ImGui::Text("Total frame: %.2f ms  (%.0f FPS est.)",
                tot, tot > 0.001f ? 1000.0f / tot : 0.0f);
    ImGui::Separator();

#ifndef MONKEY_DUST_STANDALONE_EDITOR
    if (ImGui::CollapsingHeader("Counters by category", ImGuiTreeNodeFlags_DefaultOpen)) {
        md::FrameCounters fc;
        md_frame_counters(&fc);
        if (ImGui::BeginTable("##fcnt", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("category"); ImGui::TableSetupColumn("draws"); ImGui::TableSetupColumn("instances");
            ImGui::TableSetupColumn("triangles"); ImGui::TableSetupColumn("tris/inst");
            ImGui::TableHeadersRow();
            auto row = [](const md::FrameCounterRow& r) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name);
                ImGui::TableNextColumn(); ImGui::Text("%d", r.draws);
                ImGui::TableNextColumn(); ImGui::Text("%d", r.instances);
                ImGui::TableNextColumn(); ImGui::Text("%lld", (long long)r.triangles);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", md::FrameCounters::TrisPerInstance(r));
            };
            for (int i = 0; i < fc.count; ++i) row(fc.rows[i]);
            row(fc.Total());
            ImGui::EndTable();
        }
        ImGui::TextDisabled("NPC not counted yet (no draw / triangle counter in the NPC renderer).");
        ImGui::Separator();
    }
#endif

    if (n == 0) {
        ImGui::TextDisabled("No passes recorded.");
        ImGui::TextDisabled("Call GpuProfiler::Get().BeginPass/EndPass");
        ImGui::TextDisabled("from render systems to populate.");
        return;
    }

    // Budget reference: 16.67 ms = 60 FPS
    constexpr float BUDGET_MS = 16.67f;

    for (int i = 0; i < n; ++i) {
        const auto& r = prof.GetResult(i);
        float frac        = (tot > 0.001f) ? (r.cpu_ms / tot) : 0.0f;
        float budget_frac = r.cpu_ms / BUDGET_MS;

        ImVec4 col = budget_frac < 0.33f ? ImVec4(0.2f, 0.9f, 0.3f, 1.f) :
                     budget_frac < 0.66f ? ImVec4(1.0f, 0.8f, 0.1f, 1.f) :
                                           ImVec4(1.0f, 0.2f, 0.1f, 1.f);

        char label[64];
        snprintf(label, sizeof(label), "%.2f ms  %s", r.cpu_ms, r.name);

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(frac, ImVec2(-1, 16), label);
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::Text("Budget (60 FPS): 16.67 ms");

    // ── Flame graph timeline ───────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Text("Timeline:");

    FlameEntry entries[64];
    float      cursor = 0.f;
    for (int i = 0; i < n; ++i) {
        const auto& r  = prof.GetResult(i);
        entries[i].start_ms = cursor;
        entries[i].end_ms   = cursor + r.cpu_ms;
        entries[i].name     = r.name;
        cursor += r.cpu_ms;
    }

    float avail_w = ImGui::GetContentRegionAvail().x;
    ImGuiWidgetFlameGraph::PlotFlame(
        "##flame",
        FlameGetter,
        entries,
        n,
        0,
        nullptr,
        0.f,
        tot > 0.001f ? tot : 1.f,
        ImVec2(avail_w, 60.f));

}
#endif
