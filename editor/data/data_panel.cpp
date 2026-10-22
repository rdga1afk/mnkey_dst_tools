#include "data_panel.h"
#include "data_browser.h"
#include "dialog_model.h"
#include "mod_check.h"
#include "mod_write.h"
#include "translation.h"
#include <algorithm>
#include <filesystem>
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
    std::unique_ptr<md::fcsdef::DialogModel> dialogs;
    std::string status;                             // last edit error
    const md::fcsdef::LayoutNode* node = nullptr;   // selected category
    std::vector<md::fcsdef::ItemRow> rows;
    std::string selected;                           // string_id
    std::vector<md::fcsdef::CardField> card;
    std::vector<md::fcsdef::CardInstance> insts;
    char filter[64] = "";
    bool only_changed = false, show_hidden = false;
    std::string line, line_parent;                  // selected dialogue line (the one being edited) and the dialogue it was opened from
    char new_text[256] = "";
    char search[64] = "";
    char import_path[512] = "";
    std::vector<md::fcsdef::Hit> hits;
    std::vector<md::fcsdef::Problem> problems;
    char mod_name[64] = "md_test", mod_author[64] = "md", mod_desc[256] = "made with the md editor", mod_deps[256] = "gamedata.base";
    std::vector<std::string> hist; int hpos = -1;   // navigation history (Back / Forward)
    char repl_find[128] = "", repl_with[128] = "", bulk_field[64] = "", bulk_value[128] = "", columns[128] = "";
    std::vector<md::fcsdef::ReplaceHit> repl_hits;
} g;

void Refresh() { if (!g.selected.empty()) { g.card = g.browser->Card(g.selected); g.insts = g.browser->Instances(g.selected); } }
void Select(const md::fcsdef::LayoutNode& n);
void Load() {
    g.tried = true;
    const std::string dir = md::KenshiDir();
    if (!md::fcsdef::LoadDir(dir, g.schema, g.error)) return;
    md::fcsdef::AddCodeEnums(g.schema);   // the code enums (talker, conditions, actions, triggers) when the file is found (MD_FCS_CODE_ENUMS)
    md::fcsdef::AddDialogActionFields(g.schema);
    md::fcs::Install inst(dir);
    md::fcs::LoadOrder order; md::fcs::Error err;
    if (!inst.LoadOrderFor(order, err) || !g.data.Load(order, err)) { g.error = err.Message(); return; }
    g.browser = std::make_unique<md::fcsdef::DataBrowser>(g.schema, g.data);
    g.edits = std::make_unique<md::fcsdef::EditSession>(g.schema, g.data);
    g.browser->SetEdits(g.edits.get());
    g.dialogs = std::make_unique<md::fcsdef::DialogModel>(*g.edits, g.schema, g.data);
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

void Go(const std::string& sid, bool record = true) {
    g.selected = sid;
    if (record) { g.hist.resize(g.hpos + 1); if (g.hist.empty() || g.hist.back() != sid) g.hist.push_back(sid); g.hpos = (int)g.hist.size() - 1; }
    Refresh();
}

// A new empty mod: the edit layer starts over under the mod's name (string ids of new records end with it).
void NewMod() {
    g.edits = std::make_unique<md::fcsdef::EditSession>(g.schema, g.data, std::string(g.mod_name) + ".mod");
    g.browser->SetEdits(g.edits.get());
    g.dialogs = std::make_unique<md::fcsdef::DialogModel>(*g.edits, g.schema, g.data);
    g.problems.clear(); g.hits.clear(); g.repl_hits.clear(); g.line.clear(); g.line_parent.clear(); g.hist.clear(); g.hpos = -1;
    if (g.node) g.rows = g.browser->Items(*g.node);
    Refresh();
    g.status = std::string("new mod ") + g.mod_name + ".mod";
}

const md::fcsdef::EnumDef* EnumOf(const char* n) { return g.schema.FindEnum(n); }
bool EnumCombo(const char* label, const char* enum_name, int& idx, float width) {
    const md::fcsdef::EnumDef* e = EnumOf(enum_name);
    if (!e || e->values.empty()) { ImGui::SetNextItemWidth(width); return ImGui::InputInt(label, &idx); }
    if (idx < 0 || idx >= (int)e->values.size()) idx = 0;
    ImGui::SetNextItemWidth(width);
    bool changed = false;
    if (ImGui::BeginCombo(label, e->values[idx].name.c_str())) {
        for (int i = 0; i < (int)e->values.size(); ++i) if (ImGui::Selectable(e->values[i].name.c_str(), i == idx)) { idx = i; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}
std::string EnumPick(const char* enum_name, int idx) { const md::fcsdef::EnumDef* e = EnumOf(enum_name); return e && idx >= 0 && idx < (int)e->values.size() ? e->values[idx].name : std::to_string(idx); }

// One line of a dialogue and, when opened, the lines below it (read per frame only for open nodes). `path` guards against the loops the data has.
void DrawLine(const std::string& parent, const std::string& sid, std::vector<std::string>& path) {
    using namespace md::fcsdef;
    const DialogNode n = g.dialogs->Shallow(sid);
    const std::string title = (n.texts.empty() ? g.edits->NameOf(sid) : n.texts[0]);
    const bool looped = std::find(path.begin(), path.end(), sid) != path.end();
    const auto kids = looped ? std::vector<std::string>() : g.dialogs->ChildLines(sid);
    ImGui::PushID(sid.c_str());
    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_SpanAvailWidth | (kids.empty() ? ImGuiTreeNodeFlags_Leaf : 0);
    const bool open = ImGui::TreeNodeEx("##n", f, "[%s] %s%s", n.speaker.c_str(), title.substr(0, 90).c_str(), looped ? "  (loops back)" : "");
    if (ImGui::IsItemClicked()) { g.line = sid; g.line_parent = parent; }
    if (open) {
        path.push_back(sid);
        for (const std::string& k : kids) DrawLine(sid, k, path);
        path.pop_back();
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void DrawDialogue(int kind) {
    using namespace md::fcsdef;
    ImGui::SeparatorText("Dialogue");
    if (kind == kItemDialoguePackage) {
        for (const DialogRef& d : g.dialogs->DialoguesOf(g.selected)) ImGui::BulletText("%s  <-  %s", d.name.c_str(), d.trigger_name.c_str());
        static char nm[64] = "New dialogue"; static int trig = 1;
        ImGui::SetNextItemWidth(200); ImGui::InputText("name##nd", nm, sizeof nm);
        ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::InputInt("EventTriggerEnum##nd", &trig);
        ImGui::SameLine();
        if (ImGui::Button("New dialogue")) { const std::string s = g.dialogs->NewDialogue(nm, g.selected, std::to_string(trig)); if (s.empty()) g.status = "new dialogue failed"; else Refresh(); }
        return;
    }
    std::vector<std::string> path;
    if (kind == kItemDialogue) {
        for (const DialogAction& c : g.dialogs->Shallow(g.selected).conditions) ImGui::BulletText("condition  %s  %s", c.what.c_str(), c.detail.c_str());
        for (const std::string& k : g.dialogs->ChildLines(g.selected)) DrawLine(g.selected, k, path);
        if (g.dialogs->ChildLines(g.selected).empty()) ImGui::TextDisabled("no lines");
        ImGui::SetNextItemWidth(400); ImGui::InputText("##nl", g.new_text, sizeof g.new_text);
        ImGui::SameLine();
        if (ImGui::Button("Add line")) { if (g.dialogs->AddLine(g.selected, g.new_text, "T_ME").empty()) g.status = "add line failed"; else Refresh(); }
    }
    if (kind == kItemDialogueLine) g.line = g.selected;
    if (!g.line.empty() && g.edits->Exists(g.line)) {
        const DialogNode n = g.dialogs->Shallow(g.line);
        ImGui::Separator(); ImGui::Text("selected line: %s", n.speaker.c_str());
        for (size_t i = 0; i < n.texts.size(); ++i) {
            ImGui::PushID((int)i);
            static char buf[8192]; std::snprintf(buf, sizeof buf, "%s", n.texts[i].c_str());
            ImGui::SetNextItemWidth(500);
            if (ImGui::InputText("##t", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) { if (n.texts[i].size() >= sizeof buf - 1) g.status = "text too long to edit here (8191 bytes)"; else { g.dialogs->SetText(g.line, (int)i, buf); Refresh(); } }
            ImGui::PopID();
        }
        for (const DialogAction& c : n.conditions) ImGui::BulletText("condition  %s  %s", c.what.c_str(), c.detail.c_str());
        for (const DialogAction& c : n.effects) ImGui::BulletText("effect  %s  %s", c.what.c_str(), c.detail.c_str());
        {   // templates by menu: a condition (what, who, compare, value) and an effect (what, value)
            static int ci = 1, who = 0, cmp = 0, cval = 0, ai = 1, aval = 0;
            ImGui::SeparatorText("Add condition");
            EnumCombo("##cond", "DialogConditionEnum", ci, 220); ImGui::SameLine(); EnumCombo("##who", "TalkerEnum", who, 130); ImGui::SameLine(); EnumCombo("##cmp", "ComparisonEnum", cmp, 130);
            ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputInt("##cv", &cval, 0); ImGui::SameLine();
            if (ImGui::Button("Add condition")) { if (g.dialogs->AddCondition(g.line, EnumPick("DialogConditionEnum", ci), EnumPick("TalkerEnum", who), EnumPick("ComparisonEnum", cmp), cval).empty()) g.status = "add condition failed (code enums not loaded?)"; else Refresh(); }
            ImGui::SeparatorText("Add effect");
            EnumCombo("##act", "DialogActionEnum", ai, 260); ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputInt("##av", &aval, 0); ImGui::SameLine();
            if (ImGui::Button("Add effect")) { if (g.dialogs->AddEffect(g.line, EnumPick("DialogActionEnum", ai), aval).empty()) g.status = "add effect failed (code enums not loaded?)"; else Refresh(); }
            ImGui::Separator();
        }
        ImGui::SetNextItemWidth(400); ImGui::InputText("##rl", g.new_text, sizeof g.new_text);
        ImGui::SameLine();
        if (ImGui::Button("Add reply")) { if (g.dialogs->AddLine(g.line, g.new_text, "T_TARGET").empty()) g.status = "add reply failed"; else Refresh(); }
        if (!g.line_parent.empty() && g.line_parent != g.line) { ImGui::SameLine(); if (ImGui::Button("Remove line") && g.dialogs->RemoveLine(g.line_parent, g.line)) { g.line.clear(); Refresh(); } }
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
    ImGui::SetNextItemWidth(-60); if (ImGui::InputTextWithHint("##s", "search all data", g.search, sizeof g.search, ImGuiInputTextFlags_EnterReturnsTrue)) g.hits = md::fcsdef::SearchAll(*g.edits, g.data, g.search, g.node ? g.node->type : "");
    ImGui::SameLine(); if (ImGui::Button("all")) g.hits = md::fcsdef::SearchAll(*g.edits, g.data, g.search);
    if (!g.hits.empty()) {
        ImGui::Text("%zu hits", g.hits.size()); ImGui::SameLine(); if (ImGui::SmallButton("close")) g.hits.clear();
        for (const md::fcsdef::Hit& h : g.hits) if (ImGui::Selectable((h.name + "  [" + md::fcs::ItemTypeName(g.edits->KindOf(h.string_id)) + ", " + h.where + "]##" + h.string_id).c_str(), g.selected == h.string_id)) Go(h.string_id);
        ImGui::Separator();
    }
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##cols", "extra columns: field, field", g.columns, sizeof g.columns);
    ImGui::Text("%zu items", g.rows.size());
    std::vector<std::string> cols; { std::string cur; for (const char* c = g.columns;; ++c) { if (*c == ',' || !*c) { size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' '); if (a != std::string::npos) cols.push_back(cur.substr(a, b - a + 1)); cur.clear(); if (!*c) break; } else cur += *c; } }
    for (const md::fcsdef::ItemRow& r : g.rows) {
        if (g.filter[0] && r.name.find(g.filter) == std::string::npos) continue;
        std::string label = r.name;
        if (!cols.empty()) if (const md::fcs::GameData* rec = g.data.Get(r.string_id)) for (const std::string& c : cols) { std::string v; if (g.browser->FieldText(*rec, c, v)) label += "  |  " + v.substr(0, 24); }
        if (ImGui::Selectable((label + "##" + r.string_id).c_str(), g.selected == r.string_id)) Go(r.string_id);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("kd_card", ImVec2(0, h), true);
    ImGui::Checkbox("only fields set by the data", &g.only_changed); ImGui::SameLine(); ImGui::Checkbox("hidden by conditions", &g.show_hidden);
    ImGui::BeginDisabled(g.hpos <= 0); if (ImGui::Button("Back")) Go(g.hist[--g.hpos], false); ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(g.hpos + 1 >= (int)g.hist.size()); if (ImGui::Button("Forward")) Go(g.hist[++g.hpos], false); ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("Undo") && g.edits->Undo()) Refresh();
    ImGui::SameLine(); if (ImGui::Button("Redo") && g.edits->Redo()) Refresh();
    ImGui::SameLine(); ImGui::TextDisabled("%zu modified", g.edits->ModifiedCount());
    ImGui::SameLine();
    if (ImGui::Button("Cleanup")) { const size_t n = g.edits->Cleanup(); g.status = "cleanup dropped " + std::to_string(n); Refresh(); }
    ImGui::SameLine();
    if (ImGui::Button("Export .pot")) {   // strings to translate, into temp_/md_mods/ (never the install)
        std::filesystem::create_directories("temp_/md_mods");
        const auto ex = md::fcsdef::ExtractStrings(g.schema, g.data, *g.browser);
        FILE* o = std::fopen("temp_/md_mods/gamedata.pot", "wb");
        if (o) { const std::string t = md::fcsdef::MakePot(ex); std::fwrite(t.data(), 1, t.size(), o); std::fclose(o); g.status = "wrote temp_/md_mods/gamedata.pot (" + std::to_string(ex.size()) + " strings)"; } else g.status = "cannot write the .pot";
    }
    ImGui::SameLine(); if (ImGui::Button("Clean")) { const size_t n = g.edits->CleanOutsideSchema(); g.status = "clean dropped " + std::to_string(n) + " (fields outside the schema)"; Refresh(); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(220); ImGui::InputTextWithHint("##imp", "mod file to merge in", g.import_path, sizeof g.import_path);
    ImGui::SameLine();
    if (ImGui::Button("Merge")) {
        md::fcs::File f; md::fcs::Error fe; size_t sk = 0;
        if (md::fcs::ReadFile(g.import_path, f, fe) && g.edits->ImportFile(f, &sk)) { g.status = "merged " + std::to_string(f.records.size()) + " records" + (sk ? " (" + std::to_string(sk) + " vector fields skipped)" : ""); g.rows = g.node ? g.browser->Items(*g.node) : g.rows; Refresh(); }
        else g.status = "merge failed: " + fe.Message();
    }
    ImGui::SameLine();
    if (ImGui::Button("Check")) g.problems = md::fcsdef::CheckMod(*g.edits, g.schema, g.data);
    ImGui::SameLine();
    if (ImGui::Button("Save mod") && g.edits->TouchedRecords().size()) {   // for now only into temp_/md_mods/ (never into the install); not with errors
        g.problems = md::fcsdef::CheckMod(*g.edits, g.schema, g.data);
        bool errors = false; for (const auto& pr : g.problems) if (pr.level == md::fcsdef::Problem::Error) errors = true;
        if (errors) g.status = "not saved: fix the errors listed below";
        else {
            std::filesystem::create_directories("temp_/md_mods");
            std::string err; md::fcsdef::ModInfo info; info.author = g.mod_author; info.description = g.mod_desc; info.dependencies = g.mod_deps;
            const std::string path = std::string("temp_/md_mods/") + g.mod_name + ".mod";
            if (md::fcsdef::WriteMod(path, *g.edits, g.schema, info, err, md::KenshiDir() + "/data")) g.status = "saved " + path; else g.status = "save failed: " + err;
        }
    }
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
    if (ImGui::CollapsingHeader("Mod info / new mod")) {
        ImGui::SetNextItemWidth(160); ImGui::InputText("name##mi", g.mod_name, sizeof g.mod_name); ImGui::SameLine();
        ImGui::SetNextItemWidth(120); ImGui::InputText("author##mi", g.mod_author, sizeof g.mod_author);
        ImGui::SetNextItemWidth(500); ImGui::InputText("description##mi", g.mod_desc, sizeof g.mod_desc);
        ImGui::SetNextItemWidth(500); ImGui::InputText("dependencies##mi", g.mod_deps, sizeof g.mod_deps);
        if (ImGui::Button("New mod (discard edits)")) NewMod();
        ImGui::SameLine(); ImGui::TextDisabled("saves to temp_/md_mods/%s.mod", g.mod_name);
    }
    if (ImGui::CollapsingHeader("Changes of the mod")) {
        for (const md::fcsdef::ChangeRow& r : md::fcsdef::ChangeList(*g.edits)) {
            if (ImGui::Selectable((r.state + "  " + r.name + "   " + r.what + "##ch" + r.string_id).c_str(), g.selected == r.string_id)) Go(r.string_id);
        }
    }
    if (ImGui::CollapsingHeader("Find and replace / set in all listed items")) {
        ImGui::SetNextItemWidth(200); ImGui::InputText("find##fr", g.repl_find, sizeof g.repl_find); ImGui::SameLine();
        ImGui::SetNextItemWidth(200); ImGui::InputText("replace with##fr", g.repl_with, sizeof g.repl_with);
        const std::string type = g.node ? g.node->type : "";
        if (ImGui::Button("Preview")) g.repl_hits = md::fcsdef::FindReplace(*g.edits, g.data, g.repl_find, g.repl_with, type, false);
        ImGui::SameLine();
        if (ImGui::Button("Replace all")) {
            if (!g.repl_find[0]) g.status = "find text is empty";
            else { g.repl_hits = md::fcsdef::FindReplace(*g.edits, g.data, g.repl_find, g.repl_with, type, true); g.status = "replaced in " + std::to_string(g.repl_hits.size()) + " places"; if (g.node) g.rows = g.browser->Items(*g.node); Refresh(); }
        }
        ImGui::TextDisabled("limited to the type of the selected category (%s)", type.empty() ? "all" : type.c_str());
        for (size_t i = 0; i < g.repl_hits.size() && i < 30; ++i) ImGui::BulletText("%s %s: %s -> %s", g.edits->NameOf(g.repl_hits[i].string_id).c_str(), g.repl_hits[i].field.empty() ? "name" : g.repl_hits[i].field.c_str(), g.repl_hits[i].before.substr(0, 30).c_str(), g.repl_hits[i].after.substr(0, 30).c_str());
        ImGui::Separator();
        ImGui::SetNextItemWidth(160); ImGui::InputText("field##bk", g.bulk_field, sizeof g.bulk_field); ImGui::SameLine();
        ImGui::SetNextItemWidth(160); ImGui::InputText("value##bk", g.bulk_value, sizeof g.bulk_value); ImGui::SameLine();
        if (ImGui::Button("Set in all listed")) {
            std::vector<std::string> ids; for (const md::fcsdef::ItemRow& r : g.rows) if (!g.filter[0] || r.name.find(g.filter) != std::string::npos) ids.push_back(r.string_id);
            std::string err; const size_t n = md::fcsdef::SetMany(*g.edits, ids, g.bulk_field, g.bulk_value, err);
            g.status = "set in " + std::to_string(n) + " of " + std::to_string(ids.size()) + (err.empty() ? "" : "; " + err); Refresh();
        }
    }
    if (!g.selected.empty() && ImGui::CollapsingHeader("Referenced by")) {
        for (const md::fcsdef::RefHit& r : md::fcsdef::Referencing(*g.edits, g.data, g.selected)) if (ImGui::Selectable((r.name + "   (" + r.category + ")##rb" + r.string_id + r.category).c_str())) Go(r.string_id);
    }
    for (const auto& pr : g.problems) {
        ImGui::PushID(&pr);
        ImGui::TextColored(pr.level == md::fcsdef::Problem::Error ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(1, 0.8f, 0.3f, 1), "%s  %s.%s: %s", pr.level == md::fcsdef::Problem::Error ? "error" : "warning", g.edits->NameOf(pr.record).c_str(), pr.field.c_str(), pr.what.c_str());
        if (pr.fix != md::fcsdef::Problem::None) { ImGui::SameLine(); if (ImGui::SmallButton(pr.fix == md::fcsdef::Problem::RemoveLink ? "remove link" : "name it")) { md::fcsdef::ApplyFix(*g.edits, pr); g.problems = md::fcsdef::CheckMod(*g.edits, g.schema, g.data); ImGui::PopID(); Refresh(); break; } }
        ImGui::PopID();
    }
    if (!g.status.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g.status.c_str());
    std::string cat;
    for (const md::fcsdef::CardField& c : g.card) {
        if (g.only_changed && !c.changed && !c.edited) continue;
        if (!c.visible && !g.show_hidden) continue;
        if (c.category != cat) { cat = c.category; ImGui::SeparatorText(cat.c_str()); }
        const ImVec4 col = c.edited ? ImVec4(0.5f, 1, 0.5f, 1) : c.changed ? ImVec4(1, 0.85f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);   // green MODIFIED here, yellow set by the data, grey default
        ImGui::TextColored(col, "%s", c.name.c_str());
        ImGui::SameLine(200);
        const bool editable = !c.readonly && (c.type == "bool" || c.type == "float" || c.type == "int" || c.type == "string" || c.type == "colour" || c.type == "enum" || c.type == "file");
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
    if (!g.selected.empty()) {
        static char itgt[96] = ""; static float ipos[3] = {0, 0, 0};
        ImGui::SeparatorText("Instances");
        ImGui::SetNextItemWidth(220); ImGui::InputText("target id##in", itgt, sizeof itgt); ImGui::SameLine(); ImGui::SetNextItemWidth(220); ImGui::InputFloat3("pos##in", ipos); ImGui::SameLine();
        if (ImGui::Button("Add instance")) {
            if (!g.edits->Exists(itgt)) g.status = std::string("no record ") + itgt;
            else { md::fcs::Instance in; in.id = "md" + std::to_string(g.edits->Instances(g.selected).size() + 1) + "_" + std::to_string(g.edits->LastId()); in.target = itgt; for (int k = 0; k < 3; ++k) in.position[k] = ipos[k]; in.rotation[0] = 1; g.edits->SetInstance(g.selected, in); Refresh(); }
        }
    }
    if (!g.insts.empty()) {
        for (const md::fcsdef::CardInstance& c : g.insts) {
            ImGui::PushID(c.id.c_str());
            ImGui::Text("%s  ->  %s   (%.1f, %.1f, %.1f)%s", c.id.c_str(), c.target_name.c_str(), c.pos[0], c.pos[1], c.pos[2], c.states.empty() ? "" : "  [states]");
            ImGui::SameLine(); if (ImGui::SmallButton("x")) { g.edits->RemoveInstance(g.selected, c.id); Refresh(); }
            ImGui::PopID();
        }
    }
    if (!g.selected.empty()) { const int k = g.edits->KindOf(g.selected); if (k == md::fcsdef::kItemDialogue || k == md::fcsdef::kItemDialogueLine || k == md::fcsdef::kItemDialoguePackage) DrawDialogue(k); }
    ImGui::EndChild();
}

}  // namespace KenshiDataPanel
