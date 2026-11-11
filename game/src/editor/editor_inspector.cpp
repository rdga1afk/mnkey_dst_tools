#ifdef MONKEY_DUST_EDITOR
#include "editor_inspector.h"
#include "editor_core.h"
#include "imgui_reflect_visitor.h"
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/components/player_controller.h>
#include <monkey_dust/components/renderable.h>
#include "world/character_def.h"
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
bool EditorInspector::CollapsingSection(const char* label, ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(color.x*0.6f, color.y*0.6f, color.z*0.6f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(color.x*0.8f, color.y*0.8f, color.z*0.8f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  color);
    bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor(3);
    return open;
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawTransform(MdEntity e) {
    auto& reg = MdRegistry::Get();
    if (!(reg.Handle(e).has<WorldTransform>())) return;
    if (!CollapsingSection("Transform##insp", {0.2f, 0.8f, 0.3f, 1.f})) return;

    auto& tr = reg.Handle(e).get_mut<WorldTransform>();
    const auto* d = md::ComponentReflect::Get().Find("world_transform");
    if (d) {
        DrawReflectedFloatFields(&tr, *d);
    } else {
        // ComponentReflect not registered (shouldn't happen — see
        // md::RegisterCoreComponents() call in InitGame) — fallback so
        // the inspector never silently shows nothing.
        ImGui::DragFloat("X##tr", &tr.x, 0.1f);
        ImGui::DragFloat("Y##tr", &tr.y, 0.1f);
        ImGui::DragFloat("Z##tr", &tr.z, 0.1f);
        ImGui::DragFloat("RotY##tr", &tr.rot_y, 1.f, -180.f, 180.f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawHealth(MdEntity e) {
    auto& reg = MdRegistry::Get();
    if (!(reg.Handle(e).has<Health>())) return;
    if (!CollapsingSection("Health##insp", {0.9f, 0.2f, 0.2f, 1.f})) return;

    auto& hp = reg.Handle(e).get_mut<Health>();
    float frac = hp.HpFraction();
    float cur  = hp.TotalHp(), mx = hp.TotalMax();
    char overlay[32];
    snprintf(overlay, sizeof(overlay), "%.0f / %.0f", cur, mx);
    ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
    for (int i = 0; i < LIMB_COUNT; ++i) {
        char lbl[16]; snprintf(lbl, sizeof(lbl), "Limb %d##hp", i);
        ImGui::SliderFloat(lbl, &hp.hp[i], 0.f, hp.max[i]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawNavAgent(MdEntity e) {
    auto& reg = MdRegistry::Get();
    if (!(reg.Handle(e).has<NavAgent>())) return;
    if (!CollapsingSection("NavAgent##insp", {0.2f, 0.8f, 0.7f, 1.f})) return;

    const auto& nav = reg.Handle(e).get_mut<NavAgent>();
    ImGui::Text("Target: (%.1f, %.1f)", nav.target_x, nav.target_z);
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawPlayerController(MdEntity e) {
    auto& reg = MdRegistry::Get();
    if (!(reg.Handle(e).has<PlayerController>())) return;
    if (!CollapsingSection("PlayerController##insp", {0.9f, 0.9f, 0.9f, 1.f})) return;

    auto& pc = reg.Handle(e).get_mut<PlayerController>();
    ImGui::DragFloat("Move Speed##pc", &pc.move_speed, 0.1f, 0.5f, 20.f);
    ImGui::Text("Attack cooldown: %u ms", pc.attack_cooldown_ms);
    ImGui::Text("Last attack: %.0f ms", pc.last_attack_ms);
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawAddComponent(MdEntity e) {
    auto& reg = MdRegistry::Get();
    ImGui::Separator();
    if (ImGui::BeginMenu("Add Component##insp")) {
        if (ImGui::MenuItem("Health") && !(reg.Handle(e).has<Health>()))
            reg.Handle(e).emplace<Health>(Health::Make(100.f, 100.f));
        if (ImGui::MenuItem("NavAgent") && !(reg.Handle(e).has<NavAgent>()))
            reg.Handle(e).emplace<NavAgent>();
        if (ImGui::MenuItem("PlayerController") && !(reg.Handle(e).has<PlayerController>()))
            reg.Handle(e).emplace<PlayerController>(PlayerController{true, 5.f, 0.f, 800u, 1u});
        ImGui::EndMenu();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GH-5: Character editor — morph sliders for per-entity CharacterDef.
// CharacterDef is not an ECS component; we store one per-entity using the entity
// integral id as a map key (static fixed array up to MAX_CHAR_SLOTS).
static constexpr int MAX_CHAR_SLOTS = 256;
static CharacterDef s_char_defs[MAX_CHAR_SLOTS];
static uint32_t     s_char_slot_ids[MAX_CHAR_SLOTS] = {};
static int          s_char_slot_count = 0;

static CharacterDef* GetOrCreateCharDef(MdEntity e) {
    uint32_t eid = e.ToIntegral();
    for (int i = 0; i < s_char_slot_count; ++i)
        if (s_char_slot_ids[i] == eid) return &s_char_defs[i];
    if (s_char_slot_count >= MAX_CHAR_SLOTS) return nullptr;
    int idx = s_char_slot_count++;
    s_char_slot_ids[idx] = eid;
    s_char_defs[idx] = CharacterDef{};
    return &s_char_defs[idx];
}

void EditorInspector::DrawCharacterDef(MdEntity e) {
    if (!(MdRegistry::Get().Handle(e).has<Renderable>())) return;
    if (!CollapsingSection("Character##insp", {0.3f, 0.7f, 0.9f, 1.f})) return;

    CharacterDef* def = GetOrCreateCharDef(e);
    if (!def) { ImGui::TextDisabled("slot limit reached"); return; }

    ImGui::Text("Body");
    ImGui::DragFloat("Height##cd", &def->height,  0.01f, 0.80f, 1.20f);
    ImGui::DragFloat("Bulk##cd",   &def->bulk,    0.01f, 0.70f, 1.40f);

    ImGui::Separator();
    ImGui::Text("Skin");
    ImGui::ColorEdit3("Skin##cd",  &def->skin_r);
    ImGui::ColorEdit3("Hair##cd",  &def->hair_r);
    ImGui::DragFloat("Tint##cd",   &def->color_strength, 0.01f, 0.f, 1.f);
    ImGui::DragFloat("Saturation##cd",  &def->skintone_sat, 0.01f, 0.f, 2.f);
    ImGui::DragFloat("Brightness##cd",  &def->skintone_bri, 0.005f, -0.3f, 0.3f);

    ImGui::Separator();
    int mc = def->morph_count < CHARDEF_MAX_MORPHS ? def->morph_count : CHARDEF_MAX_MORPHS;
    ImGui::Text("Morphs (%d / %d)", mc, CHARDEF_MAX_MORPHS);
    if (ImGui::SmallButton("+##morph") && mc < CHARDEF_MAX_MORPHS)
        def->morph_count = (uint8_t)(mc + 1);
    ImGui::SameLine();
    if (ImGui::SmallButton("-##morph") && mc > 0)
        def->morph_count = (uint8_t)(mc - 1);

    for (int i = 0; i < mc; ++i) {
        char lbl[24]; snprintf(lbl, sizeof(lbl), "Morph %d##cm%d", i, i);
        ImGui::SliderFloat(lbl, &def->morph_weights[i], 0.f, 1.f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::Draw() {
    if (!EditorCore::Get().panels_visible[1]) return;

    ImGui::SetNextWindowSize(ImVec2(300, 550), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(1280 - 305, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector##insp", &EditorCore::Get().panels_visible[1])) {
        ImGui::End();
        return;
    }
    DrawContent();
    ImGui::End();
}

void EditorInspector::DrawContent() {

    auto& ec = EditorCore::Get();
    MdEntity e = ec.GetPrimary();

    if (e == MdEntity::Null() || !MdRegistry::Get().Valid(e)) {
        ImGui::TextDisabled("No entity selected");
        return;
    }

    uint32_t id = e.ToIntegral();
    ImGui::Text("Entity %u", id);
    ImGui::SameLine();
    if (ImGui::SmallButton("Focus")) ec.FocusOnSelected();
    ImGui::Separator();

    DrawTransform(e);
    DrawHealth(e);
    DrawNavAgent(e);
    DrawPlayerController(e);
    DrawCharacterDef(e);
    DrawAddComponent(e);

}

// hook called by tools/editor/editor_core.cpp (the tools repo does not include game headers)
void EditorInspector_DrawContent() { EditorInspector::Get().DrawContent(); }
#endif
