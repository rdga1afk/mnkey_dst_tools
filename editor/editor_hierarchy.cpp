#ifdef MONKEY_DUST_EDITOR
#include "editor_hierarchy.h"
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/player_controller.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/world/transform_soa.h>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
void EditorHierarchy::RefreshCache() {
    auto& reg = MdRegistry::Get();
    entity_cache_count_ = 0;
    // MdManagedTag is a genuinely empty/zero-size tag -- MdEach's gaia
    // dispatch (md_registry.h, 2026-09-08 rewrite for Sparse-storage
    // support) special-cases this: never asks gaia for the tag's "value"
    // at all (gaia can't hand one out for an empty type), matching
    // remains driven by this query's own .all<MdManagedTag&>() term.
    static auto q_all = reg.Raw().query().all<MdManagedTag&>();
    MdEach(q_all, [&](MdEntity fe, MdManagedTag) {
        if (entity_cache_count_ >= MAX_CACHE) return;
        entity_cache_[entity_cache_count_++] = fe;
    });
}

// ─────────────────────────────────────────────────────────────────────────────
const char* EditorHierarchy::EntityIcon(MdEntity e) const {
    auto& reg = MdRegistry::Get();
    if ((reg.Handle(e).has<PlayerController>()))   return "[PLY]";
    return "[---]";
}

void EditorHierarchy::EntityLabel(MdEntity e, char* buf, int len) const {
    uint32_t id = e.ToIntegral();
    snprintf(buf, len, "%s Entity_%u", EntityIcon(e), id);
}

ImVec4 EditorHierarchy::EntityColor(MdEntity e) const {
    auto& reg = MdRegistry::Get();
    if ((reg.Handle(e).has<PlayerController>()))
        return {0.4f, 0.8f, 1.0f, 1.0f}; // CYAN = player
    return {0.85f, 0.85f, 0.85f, 1.0f};  // WHITE = other
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorHierarchy::DrawContextMenu(MdEntity e) {
    auto& ec  = EditorCore::Get();
    auto& reg = MdRegistry::Get();

    if (ImGui::MenuItem("Select"))    ec.Select(e);
    if (ImGui::MenuItem("Duplicate")) {
        if (reg.Valid(e) && (reg.Handle(e).has<WorldTransform>())) {
            auto dst = reg.Create();
            reg.Handle(dst).emplace<WorldTransform>(reg.Handle(e).get_mut<WorldTransform>());
            auto& tr = reg.Handle(dst).get_mut<WorldTransform>();
            tr.x += 1.f; tr.slot = 0xFFFFFFFFu;
            tr.slot = TransformSoA::Get().Alloc(dst, tr.x, tr.z, 0);
            // Copy all components present on source.
            if ((reg.Handle(e).has<Health>()))         reg.Handle(dst).emplace<Health>(reg.Handle(e).get_mut<Health>());
            if ((reg.Handle(e).has<NavAgent>()))       reg.Handle(dst).emplace<NavAgent>(reg.Handle(e).get_mut<NavAgent>());
            if ((reg.Handle(e).has<Renderable>()))     reg.Handle(dst).emplace<Renderable>(reg.Handle(e).get_mut<Renderable>());
            ec.Select(dst);
            cache_refresh_counter_ = 0;
        }
    }
    if (ImGui::MenuItem("Delete")) {
        ec.Deselect(e);
        if (reg.Valid(e) && (reg.Handle(e).has<WorldTransform>()))
            TransformSoA::Get().Free(e);
        if (reg.Valid(e)) reg.Destroy(e);
        cache_refresh_counter_ = 0;
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Add Component")) {
        if (ImGui::MenuItem("Health"))  { if (reg.Valid(e) && !(reg.Handle(e).has<Health>()))  reg.Handle(e).emplace<Health>(Health{100.f, 100.f}); }
        ImGui::EndMenu();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorHierarchy::Draw() {
    if (!EditorCore::Get().panels_visible[0]) return;

    ImGui::SetNextWindowSize(ImVec2(280, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(0, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene##Hierarchy", &EditorCore::Get().panels_visible[0])) {
        ImGui::End();
        return;
    }
    DrawContent();
    ImGui::End();
}

void EditorHierarchy::DrawContent() {

    auto& ec  = EditorCore::Get();
    auto& reg = MdRegistry::Get();

    // Header
    ImGui::Text("Entities (%d)", entity_cache_count_);
    ImGui::SameLine();
    if (ImGui::SmallButton("[+]")) {
        auto e = reg.Create();
        reg.Handle(e).emplace<WorldTransform>();
        auto& tr = reg.Handle(e).get_mut<WorldTransform>();
        tr.x = ec.cam_target.x; tr.y = 0.f; tr.z = ec.cam_target.z; tr.rot_y = 0.f;
        tr.slot = TransformSoA::Get().Alloc(e, tr.x, tr.z, 0);
        ec.Select(e);
        cache_refresh_counter_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("[X]")) {
        for (int i = ec.selected_count - 1; i >= 0; --i) {
            MdEntity e = ec.selected[i];
            if (!reg.Valid(e)) continue;
            if ((reg.Handle(e).has<WorldTransform>())) TransformSoA::Get().Free(e);
            reg.Destroy(e);
        }
        ec.DeselectAll();
        cache_refresh_counter_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("[R]")) cache_refresh_counter_ = 0;

    // Search filter
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##filter", entity_filter_, sizeof(entity_filter_));

    ImGui::Separator();

    // Refresh cache every 10 frames or on demand
    if (++cache_refresh_counter_ >= 10) {
        cache_refresh_counter_ = 0;
        RefreshCache();
    }

    // Pre-filter into a display list so ListClipper never skips items mid-loop.
    // (ListClipper requires every index in [DisplayStart, DisplayEnd) to render
    // something that advances the cursor — a 'continue' inside would crash it.)
    static MdEntity display[MAX_CACHE];
    static char         display_labels[MAX_CACHE][128];
    int display_count = 0;
    for (int i = 0; i < entity_cache_count_; ++i) {
        MdEntity e = entity_cache_[i];
        if (!reg.Valid(e)) continue;
        EntityLabel(e, display_labels[display_count], 128);
        if (entity_filter_[0] != '\0' &&
            strstr(display_labels[display_count], entity_filter_) == nullptr)
            continue;
        display[display_count++] = e;
    }

    // Scrollable list with clipper
    ImGui::BeginChild("##HierarchyScroll", ImVec2(0, -26), false);

    ImGuiListClipper clipper;
    clipper.Begin(display_count);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            MdEntity e = display[i];
            bool selected = ec.IsSelected(e);
            ImVec4 col = EntityColor(e);
            ImGui::PushStyleColor(ImGuiCol_Text, col);

            if (ImGui::Selectable(display_labels[i], selected,
                    ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(0, 0)))
            {
                bool multi = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                ec.Select(e, multi);
                if (ImGui::IsMouseDoubleClicked(0)) ec.FocusOnSelected();
            }

            ImGui::PopStyleColor();

            if (ImGui::BeginPopupContextItem()) {
                DrawContextMenu(e);
                ImGui::EndPopup();
            }
        }
    }
    clipper.End();
    ImGui::EndChild();

    // Bottom status bar
    ImGui::Separator();
    if (entity_filter_[0] != '\0')
        ImGui::TextDisabled("sel: %d | shown: %d / %d", ec.selected_count, display_count, entity_cache_count_);
    else
        ImGui::TextDisabled("sel: %d | total: %d", ec.selected_count, entity_cache_count_);

}
#endif
