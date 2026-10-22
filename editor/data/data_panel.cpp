#include "data_panel.h"
#include "data_browser.h"
#include <imgui.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace KenshiDataPanel {
namespace {

struct State {
    bool tried = false, ok = false;
    std::string error;
    md::fcsdef::Schema schema;
    md::fcs::GameDataContainer data;
    std::unique_ptr<md::fcsdef::DataBrowser> browser;
    std::unique_ptr<md::fcsdef::EditSession> edits;
    std::string status;                             // last edit error
    const md::fcsdef::LayoutNode* node = nullptr;   // selected category
    std::vector<md::fcsdef::ItemRow> rows;
    std::string selected;                           // string_id
    std::vector<md::fcsdef::CardField> card;
    std::vector<md::fcsdef::CardInstance> insts;
    char filter[64] = "";
    bool only_changed = false;
} g;

void Refresh() { if (!g.selected.empty()) { g.card = g.browser->Card(g.selected); g.insts = g.browser->Instances(g.selected); } }
void Select(const md::fcsdef::LayoutNode& n);
void Load() {
    g.tried = true;
    const std::string dir = md::KenshiDir();
    if (!md::fcsdef::LoadDir(dir, g.schema, g.error)) return;
    md::fcs::Install inst(dir);
    md::fcs::LoadOrder order; md::fcs::Error err;
    if (!inst.LoadOrderFor(order, err) || !g.data.Load(order, err)) { g.error = err.Message(); return; }
    g.browser = std::make_unique<md::fcsdef::DataBrowser>(g.schema, g.data);
    g.edits = std::make_unique<md::fcsdef::EditSession>(g.schema, g.data);
    g.browser->SetEdits(g.edits.get());
    g.ok = true;
    if (!g.schema.layout.empty()) { Select(g.schema.layout[0]); if (!g.rows.empty()) { g.selected = g.rows[0].string_id; g.card = g.browser->Card(g.selected); g.insts = g.browser->Instances(g.selected); } }   // the first category and its first item are open on start
}

void Select(const md::fcsdef::LayoutNode& n) {
    g.node = &n; g.rows = g.browser->Items(n); g.selected.clear(); g.card.clear(); g.insts.clear();
}

void DrawTree(const std::vector<md::fcsdef::LayoutNode>& nodes) {
    for (const md::fcsdef::LayoutNode& n : nodes) {
        ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | (n.children.empty() ? ImGuiTreeNodeFlags_Leaf : 0) | (g.node == &n ? ImGuiTreeNodeFlags_Selected : 0);
        const bool open = ImGui::TreeNodeEx((void*)&n, f, "%s", n.name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) Select(n);
        if (open) { DrawTree(n.children); ImGui::TreePop(); }
    }
}

}  // namespace

void Draw() {
    if (!g.tried) Load();
    if (!g.ok) { ImGui::TextWrapped("Kenshi data: %s", g.error.empty() ? "not loaded" : g.error.c_str()); return; }
    const float h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("kd_tree", ImVec2(220, h), true); DrawTree(g.schema.layout); ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("kd_items", ImVec2(300, h), true);
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##f", "filter", g.filter, sizeof g.filter);
    ImGui::Text("%zu items", g.rows.size());
    for (const md::fcsdef::ItemRow& r : g.rows) {
        if (g.filter[0] && r.name.find(g.filter) == std::string::npos) continue;
        if (ImGui::Selectable((r.name + "##" + r.string_id).c_str(), g.selected == r.string_id)) { g.selected = r.string_id; g.card = g.browser->Card(r.string_id); g.insts = g.browser->Instances(r.string_id); }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("kd_card", ImVec2(0, h), true);
    ImGui::Checkbox("only fields set by the data", &g.only_changed);
    ImGui::SameLine(); if (ImGui::Button("Undo") && g.edits->Undo()) Refresh();
    ImGui::SameLine(); if (ImGui::Button("Redo") && g.edits->Redo()) Refresh();
    ImGui::SameLine(); ImGui::TextDisabled("%zu modified", g.edits->ModifiedCount());
    if (g.node && !g.node->type.empty()) {
        int kind = -1; for (int k = 0; k < md::fcs::ITEM_TYPE_COUNT; ++k) if (g.node->type == md::fcs::ItemTypeName(k)) { kind = k; break; }
        ImGui::SameLine(); if (kind >= 0 && ImGui::Button("New")) { g.selected = g.edits->NewRecord(kind, "New item"); g.rows = g.browser->Items(*g.node); Refresh(); }
    }
    if (!g.selected.empty()) {
        ImGui::SameLine(); if (ImGui::Button("Duplicate")) { const std::string n = g.edits->Duplicate(g.selected, g.edits->NameOf(g.selected) + " copy"); if (!n.empty()) { g.selected = n; g.rows = g.browser->Items(*g.node); Refresh(); } }
        ImGui::SameLine();
        if (g.edits->StateOfRecord(g.selected) == md::fcsdef::RecordState::Removed) { if (ImGui::Button("Restore")) { g.edits->Restore(g.selected); g.rows = g.browser->Items(*g.node); } }
        else if (ImGui::Button("Remove")) { g.edits->Remove(g.selected); g.rows = g.browser->Items(*g.node); }
        const std::string owner = g.edits->OwnerOf(g.selected);
        if (!owner.empty()) { ImGui::SameLine(); ImGui::TextDisabled("OWNED by %s", g.edits->NameOf(owner).c_str()); }
    }
    if (!g.status.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g.status.c_str());
    std::string cat;
    for (const md::fcsdef::CardField& c : g.card) {
        if (g.only_changed && !c.changed && !c.edited) continue;
        if (c.category != cat) { cat = c.category; ImGui::SeparatorText(cat.c_str()); }
        const ImVec4 col = c.edited ? ImVec4(0.5f, 1, 0.5f, 1) : c.changed ? ImVec4(1, 0.85f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);   // green MODIFIED here, yellow set by the data, grey default
        ImGui::TextColored(col, "%s", c.name.c_str());
        ImGui::SameLine(200);
        const bool editable = c.type == "bool" || c.type == "float" || c.type == "int" || c.type == "string" || c.type == "colour" || c.type == "enum" || c.type == "file";
        ImGui::PushID(c.name.c_str());
        if (editable) {
            static char buf[256]; std::snprintf(buf, sizeof buf, "%s", c.value.c_str());
            ImGui::SetNextItemWidth(340);
            if (ImGui::InputText("##v", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                std::string err;
                if (g.edits->Set(g.selected, c.name, buf, err)) g.status.clear(); else g.status = c.name + ": " + err;
                Refresh();
            }
            if (c.edited) { ImGui::SameLine(); if (ImGui::SmallButton("revert")) { g.edits->Revert(g.selected, c.name); Refresh(); } }
        } else {
            const std::string v = c.value.size() > 60 ? c.value.substr(0, 57) + "..." : c.value; ImGui::TextUnformatted(v.c_str());
            if (c.value.size() > 60 && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.value.c_str());
        }
        ImGui::PopID();
        ImGui::SameLine(640); ImGui::TextDisabled("%s  (default %s)", c.type.c_str(), c.def.c_str());
        if (ImGui::IsItemHovered() && !c.description.empty()) ImGui::SetTooltip("%s", c.description.c_str());
    }
    {   // links of the reference fields that have any: remove, change v0..v2, add by target id
        static char tgt[96] = ""; static int vals[3] = {0, 0, 0}; static int add_field = 0;
        bool header = false;
        for (const md::fcsdef::CardField& c : g.card) {
            if (c.type != "refs") continue;
            const auto refs = g.edits->Refs(g.selected, c.name);
            if (refs.empty()) continue;
            if (!header) { ImGui::SeparatorText("References"); header = true; }
            for (const auto& r : refs) {
                ImGui::PushID((c.name + r.first).c_str());
                ImGui::Text("%s  ->  %s", c.name.c_str(), g.edits->NameOf(r.first).empty() ? r.first.c_str() : g.edits->NameOf(r.first).c_str());
                int v[3] = { r.second[0], r.second[1], r.second[2] };
                ImGui::SameLine(420); ImGui::SetNextItemWidth(200);
                if (ImGui::InputInt3("##v", v, ImGuiInputTextFlags_EnterReturnsTrue)) { g.edits->SetRef(g.selected, c.name, r.first, { v[0], v[1], v[2] }); Refresh(); }
                ImGui::SameLine(); if (ImGui::SmallButton("x")) { g.edits->RemoveRef(g.selected, c.name, r.first); Refresh(); }
                ImGui::PopID();
            }
        }
        if (!g.card.empty()) {
            ImGui::SeparatorText("Add reference");
            std::vector<const char*> names; for (const md::fcsdef::CardField& c : g.card) if (c.type == "refs") names.push_back(c.name.c_str());
            if (!names.empty()) {
                if (add_field >= (int)names.size()) add_field = 0;
                ImGui::SetNextItemWidth(200); ImGui::Combo("field", &add_field, names.data(), (int)names.size());
                ImGui::SetNextItemWidth(260); ImGui::InputText("target string_id", tgt, sizeof tgt);
                ImGui::SetNextItemWidth(200); ImGui::InputInt3("v0 v1 v2", vals);
                if (ImGui::Button("Add") && g.edits->Exists(tgt) && g.edits->SetRef(g.selected, names[add_field], tgt, { vals[0], vals[1], vals[2] })) { g.status.clear(); Refresh(); }
                else if (ImGui::IsItemClicked() && !g.edits->Exists(tgt)) g.status = std::string("no record ") + tgt;
            }
        }
    }
    if (!g.insts.empty()) {
        ImGui::SeparatorText("Instances");
        for (const md::fcsdef::CardInstance& c : g.insts) {
            ImGui::PushID(c.id.c_str());
            ImGui::Text("%s  ->  %s   (%.1f, %.1f, %.1f)%s", c.id.c_str(), c.target_name.c_str(), c.pos[0], c.pos[1], c.pos[2], c.states.empty() ? "" : "  [states]");
            ImGui::SameLine(); if (ImGui::SmallButton("x")) { g.edits->RemoveInstance(g.selected, c.id); Refresh(); }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

}  // namespace KenshiDataPanel
