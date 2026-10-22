#include "data_panel.h"
#include "data_browser.h"
#include <imgui.h>
#include <monkey_dust/io/kenshi_dir.h>
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
    const md::fcsdef::LayoutNode* node = nullptr;   // selected category
    std::vector<md::fcsdef::ItemRow> rows;
    std::string selected;                           // string_id
    std::vector<md::fcsdef::CardField> card;
    std::vector<md::fcsdef::CardInstance> insts;
    char filter[64] = "";
    bool only_changed = false;
} g;

void Select(const md::fcsdef::LayoutNode& n);
void Load() {
    g.tried = true;
    const std::string dir = md::KenshiDir();
    if (!md::fcsdef::LoadDir(dir, g.schema, g.error)) return;
    md::fcs::Install inst(dir);
    md::fcs::LoadOrder order; md::fcs::Error err;
    if (!inst.LoadOrderFor(order, err) || !g.data.Load(order, err)) { g.error = err.Message(); return; }
    g.browser = std::make_unique<md::fcsdef::DataBrowser>(g.schema, g.data);
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
    std::string cat;
    for (const md::fcsdef::CardField& c : g.card) {
        if (g.only_changed && !c.changed) continue;
        if (c.category != cat) { cat = c.category; ImGui::SeparatorText(cat.c_str()); }
        ImGui::TextColored(c.changed ? ImVec4(1, 0.85f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), "%s", c.name.c_str());
        ImGui::SameLine(200);
        { const std::string v = c.value.size() > 60 ? c.value.substr(0, 57) + "..." : c.value; ImGui::TextUnformatted(v.c_str()); if (c.value.size() > 60 && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.value.c_str()); }
        ImGui::SameLine(560); ImGui::TextDisabled("%s  (default %s)", c.type.c_str(), c.def.c_str());
        if (ImGui::IsItemHovered() && !c.description.empty()) ImGui::SetTooltip("%s", c.description.c_str());
    }
    if (!g.insts.empty()) {
        ImGui::SeparatorText("Instances");
        for (const md::fcsdef::CardInstance& c : g.insts) ImGui::Text("%s  ->  %s   (%.1f, %.1f, %.1f)%s", c.id.c_str(), c.target_name.c_str(), c.pos[0], c.pos[1], c.pos[2], c.states.empty() ? "" : "  [states]");
    }
    ImGui::EndChild();
}

}  // namespace KenshiDataPanel
