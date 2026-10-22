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
#include <cstdlib>
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
    char mod_name[64] = "md_test", mod_author[64] = "md", mod_desc[256] = "made with the md editor", mod_deps[256] = "gamedata.base", mod_refs[256] = "";
    std::vector<std::string> hist; int hpos = -1;   // navigation history (Back / Forward)
    char repl_find[128] = "", repl_with[128] = "", bulk_field[64] = "", bulk_value[128] = "", columns[128] = "";
    std::vector<md::fcsdef::ReplaceHit> repl_hits;
    char single_path[512] = "";
    int autosave_min = 5; double last_autosave = 0;   // FCS settings: autosave interval in minutes (0 = off)
    std::vector<md::fcsdef::AssetRow> assets; bool assets_ready = false;
    std::vector<md::fcs::LoadEntry> mods;           // the installed mods in load order (core files excluded): the «Load mod» list
    bool w_world = true, w_item = true, w_info = false, w_changes = false, w_find = false, w_settings = false, w_assets = false, w_errors = false, show_refs = true;   // child windows of the main shell
    bool col_sid = false, col_type = false, col_state = false;   // Game World list columns (Columns submenu)
} g;

void Refresh() { if (!g.selected.empty()) { g.card = g.browser->Card(g.selected); g.insts = g.browser->Instances(g.selected); } }
void Select(const md::fcsdef::LayoutNode& n);
// (Re)builds browser / edit layer / dialogues over g.data; the selection starts over.
void Rebuild() {
    g.browser = std::make_unique<md::fcsdef::DataBrowser>(g.schema, g.data);
    g.edits = std::make_unique<md::fcsdef::EditSession>(g.schema, g.data, std::string(g.mod_name) + ".mod");
    g.browser->SetEdits(g.edits.get());
    g.dialogs = std::make_unique<md::fcsdef::DialogModel>(*g.edits, g.schema, g.data);
    g.problems.clear(); g.hits.clear(); g.repl_hits.clear(); g.line.clear(); g.line_parent.clear(); g.hist.clear(); g.hpos = -1; g.selected.clear(); g.card.clear(); g.insts.clear(); g.rows.clear(); g.node = nullptr;
    if (!g.schema.layout.empty()) { Select(g.schema.layout[0]); if (!g.rows.empty()) { g.selected = g.rows[0].string_id; g.card = g.browser->Card(g.selected); g.insts = g.browser->Instances(g.selected); } }   // the first category and its first item are open on start
}

// `files` empty = the install's load order (core files + mods.cfg); otherwise only those files (single-file mode, «Open any»).
bool LoadFiles(const std::vector<md::fcs::LoadEntry>& files, std::string& err) {
    g.data = md::fcs::GameDataContainer();
    md::fcs::LoadOrder order; md::fcs::Error e;
    if (files.empty()) { md::fcs::Install inst(md::KenshiDir()); if (!inst.LoadOrderFor(order, e)) { err = e.Message(); return false; } g.mods.clear(); for (const md::fcs::LoadEntry& m : order.files) if (!m.core) g.mods.push_back(m); }
    else order.files = files;
    if (!g.data.Load(order, e)) { err = e.Message(); return false; }
    return true;
}

void Load() {
    g.tried = true;
    const std::string dir = md::KenshiDir();
    if (!md::fcsdef::LoadDir(dir, g.schema, g.error)) return;
    md::fcsdef::AddCodeEnums(g.schema);   // the code enums (talker, conditions, actions, triggers) when the file is found (MD_FCS_ENUMS)
    md::fcsdef::AddDialogActionFields(g.schema);
    if (!LoadFiles({}, g.error)) return;
    Rebuild();
    g.ok = true;
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
    const bool open = ImGui::TreeNodeEx("##n", f, "[%s]%s %s%s", n.speaker.c_str(), (!n.conditions.empty() || n.has_template) ? " C" : "", title.substr(0, 90).c_str(), looped ? "  (loops back)" : "");
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
        if (n.has_template) ImGui::BulletText("conditions template: %s", n.template_name.c_str());
        for (const DialogAction& c : n.conditions) ImGui::BulletText("condition%s  %s  %s", c.from_template ? " (template)" : "", c.what.c_str(), c.detail.c_str());
        {   // templates: use / drop one for this line; make a new one; who uses it
            static int ti = 0; const auto tl = g.dialogs->Templates();
            if (!tl.empty()) { if (ti >= (int)tl.size()) ti = 0; ImGui::SetNextItemWidth(260); if (ImGui::BeginCombo("##tmpl", g.edits->NameOf(tl[ti]).c_str())) { for (int i = 0; i < (int)tl.size(); ++i) if (ImGui::Selectable(g.edits->NameOf(tl[i]).c_str(), i == ti)) ti = i; ImGui::EndCombo(); }
                ImGui::SameLine(); if (ImGui::SmallButton("use")) { g.dialogs->SetTemplate(g.line, tl[ti], true); Refresh(); }
                ImGui::SameLine(); if (ImGui::SmallButton("drop")) { g.dialogs->SetTemplate(g.line, tl[ti], false); Refresh(); }
                ImGui::SameLine(); if (ImGui::SmallButton("edit")) Go(tl[ti]);
                ImGui::SameLine(); ImGui::TextDisabled("used by %zu", g.dialogs->LinesUsing(tl[ti]).size()); }
            static char tn[64] = "New conditions template"; ImGui::SetNextItemWidth(220); ImGui::InputText("##tn", tn, sizeof tn); ImGui::SameLine();
            if (ImGui::SmallButton("new template")) { const std::string t = g.dialogs->NewTemplate(tn); g.dialogs->SetTemplate(g.line, t, true); Refresh(); }
        }
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


std::string s_dump;   // MD_FCS_LAYOUT_DUMP: this frame's layout (toolbar order, window sizes, splits), checked by tests/fcs_layout_check.py
void Dump(const std::string& line) { s_dump += line + "\n"; }

ImVec2 s_origin;   // top-left of the area below the toolbar: the child windows open relative to it

bool BeginWin(bool& open, const char* title, float w, float h, float dx = 40) {
    if (!open) return false;
    Dump(std::string("window|") + title + "|" + std::to_string((int)w) + "|" + std::to_string((int)h));
    ImGui::SetNextWindowPos(ImVec2(s_origin.x + dx, s_origin.y + dx), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &open)) { ImGui::End(); return false; }
    return true;
}

void SaveMod() {   // for now only into temp_/md_mods/ (never into the install); not with errors
    g.problems = md::fcsdef::CheckMod(*g.edits, g.schema, g.data);
    bool errors = false; for (const auto& pr : g.problems) if (pr.level == md::fcsdef::Problem::Critical) errors = true;
    if (errors) { g.status = "not saved: fix the errors listed in Errors"; g.w_errors = true; return; }
    std::filesystem::create_directories("temp_/md_mods");
    std::string err; md::fcsdef::ModInfo info; info.author = g.mod_author; info.description = g.mod_desc; info.dependencies = g.mod_deps; info.references = g.mod_refs;
    const std::string path = std::string("temp_/md_mods/") + g.mod_name + ".mod";
    if (md::fcsdef::WriteMod(path, *g.edits, g.schema, info, err, md::KenshiDir() + "/data")) g.status = "saved " + path; else g.status = "save failed: " + err;
}

void MergeMod() {
    md::fcs::File f; md::fcs::Error fe; size_t sk = 0;
    if (md::fcs::ReadFile(g.import_path, f, fe) && g.edits->ImportFile(f, &sk)) { g.status = "merged " + std::to_string(f.records.size()) + " records" + (sk ? " (" + std::to_string(sk) + " vector fields skipped)" : ""); g.rows = g.node ? g.browser->Items(*g.node) : g.rows; Refresh(); }
    else g.status = "merge failed: " + fe.Message();
}

void ExportPot() {   // strings to translate, into temp_/md_mods/ (never the install)
    std::filesystem::create_directories("temp_/md_mods");
    const auto ex = md::fcsdef::ExtractStrings(g.schema, g.data, *g.browser);
    FILE* o = std::fopen("temp_/md_mods/gamedata.pot", "wb");
    if (o) { const std::string t = md::fcsdef::MakePot(ex); std::fwrite(t.data(), 1, t.size(), o); std::fclose(o); g.status = "wrote temp_/md_mods/gamedata.pot (" + std::to_string(ex.size()) + " strings)"; } else g.status = "cannot write the .pot";
}

void ExportModsFile() {   // a copy in temp_/md_mods/, never over the install's data/mods.cfg
    std::filesystem::create_directories("temp_/md_mods");
    FILE* o = std::fopen("temp_/md_mods/mods.cfg", "wb");
    if (o) { for (const md::fcs::LoadEntry& m : g.mods) std::fprintf(o, "%s\n", m.name.c_str()); std::fclose(o); g.status = "wrote temp_/md_mods/mods.cfg (" + std::to_string(g.mods.size()) + " mods)"; } else g.status = "cannot write mods.cfg";
}

void CloseAll() { std::string err; if (LoadFiles({}, err)) { Rebuild(); g.status = "closed: install load order"; } else g.status = err; }

void OpenSingle() {
    md::fcs::LoadEntry e; e.path = g.single_path; e.name = std::filesystem::path(e.path).filename().string(); e.core = e.name == "gamedata.base";
    std::string err; if (LoadFiles({e}, err)) { Rebuild(); g.status = "opened " + e.name + " alone (single-file mode)"; } else g.status = "open failed: " + err;
}

// The main toolbar of the FCS, left to right (FCS_UI.md); a button without an implementation is disabled.
void DrawToolbar() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 2)); ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
    bool first = true;
    auto B = [&](const char* label, const char* tip, bool en = true) {
        if (!first) ImGui::SameLine(0, 2);
        first = false; Dump(std::string("button|") + label);
        ImGui::BeginDisabled(!en); const bool c = ImGui::Button(label); ImGui::EndDisabled();
        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", en ? tip : "not implemented yet");
        return c;
    };
    auto S = [&]() { ImGui::SameLine(0, 6); ImGui::TextDisabled("|"); };
    const bool touched = !g.edits->TouchedRecords().empty();
    if (B("Load", "Pick an installed mod to merge in")) ImGui::OpenPopup("kd_load");
    if (B("Save", "Save the mod into temp_/md_mods/", touched)) SaveMod();
    if (B("FCS Settings", nullptr)) g.w_settings = true;
    S();
    if (B("Undo", nullptr) && g.edits->Undo()) Refresh();
    if (B("Redo", nullptr) && g.edits->Redo()) Refresh();
    S();
    if (B("Back", nullptr, g.hpos > 0)) Go(g.hist[--g.hpos], false);
    if (B("Forward", nullptr, g.hpos + 1 < (int)g.hist.size())) Go(g.hist[++g.hpos], false);
    S();
    if (B("Mod Info", nullptr)) g.w_info = true;
    B("Global settings", nullptr, false); B("Artifacts settings", nullptr, false); B("Regions", nullptr, false);
    S();
    if (B("Cleanup", "Drop edits that equal the original")) { const size_t n = g.edits->Cleanup(); g.status = "cleanup dropped " + std::to_string(n); Refresh(); }
    if (B("Changes", nullptr)) g.w_changes = true;
    if (B("Assets", nullptr)) g.w_assets = true;
    B("Notes", nullptr, false);
    if (B("Errors", "Check the mod")) { g.problems = md::fcsdef::CheckMod(*g.edits, g.schema, g.data); g.w_errors = true; }
    S();
    if (B("Merge Mod", "Merge the file picked with Load", g.import_path[0] != 0)) MergeMod();
    if (B("Export Mods file", nullptr)) ExportModsFile();
    S();
    if (B("Find and replace", nullptr)) g.w_find = true;
    B("Spell check", nullptr, false); B("Lexicon", nullptr, false);
    if (B("Translations", "Export the strings to temp_/md_mods/gamedata.pot")) ExportPot();
    B("Translaton Fix", nullptr, false);
    S();
    if (B("Open any", "Open a single .mod / .base alone")) ImGui::OpenPopup("kd_open_any");
    if (B("Close All", "Back to the install's load order")) CloseAll();
    if (ImGui::BeginPopup("kd_load")) {
        for (const md::fcs::LoadEntry& m : g.mods) if (ImGui::Selectable(m.name.c_str())) std::snprintf(g.import_path, sizeof g.import_path, "%s", m.path.c_str());
        ImGui::SetNextItemWidth(320); ImGui::InputTextWithHint("##imp", "mod file to merge in", g.import_path, sizeof g.import_path);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("kd_open_any")) {
        ImGui::SetNextItemWidth(420); ImGui::InputTextWithHint("##single", "path of a .mod / .base to open alone", g.single_path, sizeof g.single_path);
        if (ImGui::Button("Open")) { OpenSingle(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void RefreshRows() { if (g.node) g.rows = g.browser->Items(*g.node); Refresh(); }

// «Game World»: category tree 186 px | item list (column Name + the chosen ones) with the filter box at the bottom.
void DrawGameWorld() {
    constexpr float kTreeW = 186;
    if (!BeginWin(g.w_world, "Game World", 800, 564, 0)) return;
    Dump("split|Game World|" + std::to_string((int)kTreeW));
    const float h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("kd_tree", ImVec2(kTreeW, h), true); DrawTree(g.schema.layout); ImGui::EndChild();
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("kd_right", ImVec2(0, h), false);
    std::vector<std::string> cols; { std::string cur; for (const char* c = g.columns;; ++c) { if (*c == ',' || !*c) { size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' '); if (a != std::string::npos) cols.push_back(cur.substr(a, b - a + 1)); cur.clear(); if (!*c) break; } else cur += *c; } }
    const int ncol = 1 + (int)g.col_sid + (int)g.col_type + (int)g.col_state + (int)cols.size();
    if (ImGui::BeginTable("kd_list", ncol, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders, ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        if (g.col_sid) ImGui::TableSetupColumn("StringID");
        if (g.col_type) ImGui::TableSetupColumn("Type");
        if (g.col_state) ImGui::TableSetupColumn("State");
        for (const std::string& c : cols) ImGui::TableSetupColumn(c.c_str());
        ImGui::TableHeadersRow();
        for (const md::fcsdef::ItemRow& r : g.rows) {
            if (g.filter[0] && r.name.find(g.filter) == std::string::npos) continue;
            const md::fcsdef::RecordState st = g.edits->StateOfRecord(r.string_id);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            const bool tint = st != md::fcsdef::RecordState::Original;
            if (tint) ImGui::PushStyleColor(ImGuiCol_Text, st == md::fcsdef::RecordState::Removed ? ImVec4(0.65f, 0.65f, 0.65f, 1) : st == md::fcsdef::RecordState::New ? ImVec4(0.2f, 0.7f, 0.2f, 1) : ImVec4(0.3f, 0.45f, 1, 1));
            if (ImGui::Selectable((r.name + "##" + r.string_id).c_str(), g.selected == r.string_id, ImGuiSelectableFlags_SpanAllColumns)) Go(r.string_id);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) Go(r.string_id);
            if (ImGui::BeginPopupContextItem()) {
                int kind = -1;
                if (g.node && !g.node->type.empty()) for (int k = 0; k < md::fcs::ITEM_TYPE_COUNT; ++k) if (g.node->type == md::fcs::ItemTypeName(k)) { kind = k; break; }
                if (ImGui::MenuItem("Open Item")) g.w_item = true;
                if (ImGui::MenuItem("List References")) { g.w_item = true; g.show_refs = true; }
                ImGui::MenuItem("Replace references", nullptr, false, false); ImGui::MenuItem("Add Note Item", nullptr, false, false); ImGui::MenuItem("Add Todo Item", nullptr, false, false);
                if (ImGui::MenuItem("Copy ID")) ImGui::SetClipboardText(r.string_id.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("New Item", nullptr, false, kind >= 0)) { g.selected = g.edits->NewRecord(kind, "New item"); RefreshRows(); }
                if (ImGui::MenuItem("Duplicate Item")) { const std::string n = g.edits->Duplicate(r.string_id, g.edits->NameOf(r.string_id) + " copy"); if (!n.empty()) { g.selected = n; RefreshRows(); } }
                ImGui::Separator();
                ImGui::MenuItem("Clean Item", nullptr, false, false); ImGui::MenuItem("Spell check", nullptr, false, false);
                if (ImGui::MenuItem("Show Mod Info")) g.w_info = true;
                if (ImGui::MenuItem("Set Field")) g.w_find = true;
                if (ImGui::MenuItem("Find / Replace Values")) g.w_find = true;
                ImGui::MenuItem("Revert Changes", nullptr, false, false);
                if (st == md::fcsdef::RecordState::Removed) { if (ImGui::MenuItem("Restore Item")) { g.edits->Restore(r.string_id); RefreshRows(); } }
                else if (ImGui::MenuItem("Delete Item")) { g.edits->Remove(r.string_id); RefreshRows(); }
                ImGui::Separator();
                if (ImGui::BeginMenu("Columns")) {
                    ImGui::MenuItem("StringID", nullptr, &g.col_sid); ImGui::MenuItem("Ref", nullptr, false, false); ImGui::MenuItem("Type", nullptr, &g.col_type); ImGui::MenuItem("State", nullptr, &g.col_state);
                    ImGui::Separator();
                    ImGui::SetNextItemWidth(180); ImGui::InputTextWithHint("##addcol", "Add: field, field", g.columns, sizeof g.columns);
                    if (ImGui::MenuItem("Reset")) { g.columns[0] = 0; g.col_sid = g.col_type = g.col_state = false; }
                    ImGui::EndMenu();
                }
                ImGui::EndPopup();
            }
            if (tint) ImGui::PopStyleColor();
            if (g.col_sid) { ImGui::TableNextColumn(); ImGui::TextUnformatted(r.string_id.c_str()); }
            if (g.col_type) { ImGui::TableNextColumn(); ImGui::TextUnformatted(md::fcs::ItemTypeName(g.edits->KindOf(r.string_id))); }
            if (g.col_state) { ImGui::TableNextColumn(); ImGui::TextUnformatted(st == md::fcsdef::RecordState::Original ? "Original" : st == md::fcsdef::RecordState::Modified ? "Modified" : st == md::fcsdef::RecordState::New ? "Owned" : "Removed"); }
            if (!cols.empty()) { const md::fcs::GameData* rec = g.data.Get(r.string_id); for (const std::string& c : cols) { ImGui::TableNextColumn(); std::string v; if (rec && g.browser->FieldText(*rec, c, v)) ImGui::TextUnformatted(v.substr(0, 40).c_str()); } }
        }
        ImGui::EndTable();
    }
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##f", "filter", g.filter, sizeof g.filter);
    ImGui::EndChild();
    ImGui::End();
}

// «Item» window: the property list of the selected item.
void DrawItemWin() {
    if (!g.w_item) return;
    const std::string title = (g.selected.empty() ? std::string("Item") : g.edits->NameOf(g.selected)) + "###kd_item";
    Dump("window|Item|529|559");
    ImGui::SetNextWindowPos(ImVec2(s_origin.x + 800, s_origin.y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(529, 559), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), &g.w_item, ImGuiWindowFlags_MenuBar)) { ImGui::End(); return; }
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Item")) {
            ImGui::MenuItem("Show References", nullptr, &g.show_refs);
            ImGui::MenuItem("Copy data from Item", nullptr, false, false); ImGui::MenuItem("Add Note Item", nullptr, false, false); ImGui::MenuItem("Add Todo List Item", nullptr, false, false); ImGui::MenuItem("Import Scene", nullptr, false, false);
            if (ImGui::MenuItem("Duplicate Item", nullptr, false, !g.selected.empty())) { const std::string n = g.edits->Duplicate(g.selected, g.edits->NameOf(g.selected) + " copy"); if (!n.empty()) { g.selected = n; RefreshRows(); } }
            ImGui::MenuItem("Add Missing Fields", nullptr, false, false);
            if (ImGui::MenuItem("Remove Obsolete Fields")) { const size_t n = g.edits->CleanOutsideSchema(); g.status = "clean dropped " + std::to_string(n) + " (fields outside the schema)"; Refresh(); }
            if (ImGui::MenuItem("Delete This Item", nullptr, false, !g.selected.empty())) { g.edits->Remove(g.selected); RefreshRows(); }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    if (!g.selected.empty() && g.edits->StateOfRecord(g.selected) == md::fcsdef::RecordState::Removed) { if (ImGui::Button("Restore")) { g.edits->Restore(g.selected); RefreshRows(); } }
    ImGui::Checkbox("only fields set by the data", &g.only_changed); ImGui::SameLine(); ImGui::Checkbox("hidden by conditions", &g.show_hidden);
    if (!g.selected.empty()) { const std::string owner = g.edits->OwnerOf(g.selected); if (!owner.empty()) { ImGui::SameLine(); ImGui::TextDisabled("OWNED by %s", g.edits->NameOf(owner).c_str()); } }
    constexpr float kSplit1 = 498, kSplit2 = 297;   // itemproperties: horizontal split (top | description), vertical split (grid | reference lists)
    Dump("split|Item|split1|" + std::to_string((int)kSplit1)); Dump("split|Item|split2|" + std::to_string((int)kSplit2));
    static std::string s_field;   // the field shown in the bottom pane
    ImGui::BeginChild("kd_item_top", ImVec2(0, kSplit1 - ImGui::GetCursorPosY()), false);
    ImGui::BeginChild("kd_grid", ImVec2(kSplit2, 0), true);
    if (!g.selected.empty()) {
        const md::fcs::GameData* rec = g.data.Get(g.selected);
        if (rec && rec->source < g.data.files.size()) ImGui::TextDisabled("defined in %s%s", g.data.files[rec->source].c_str(), g.data.files[rec->source] == "gamedata.base" || rec->source + 1 < g.data.files.size() ? "  (LOCKED: edits go to your mod as changes)" : "");
        else if (g.edits->StateOfRecord(g.selected) == md::fcsdef::RecordState::New) ImGui::TextDisabled("new record of your mod");
    }
    std::string cat;
    for (const md::fcsdef::CardField& c : g.card) {
        if (g.only_changed && !c.changed && !c.edited) continue;
        if (!c.visible && !g.show_hidden) continue;
        if (c.category != cat) { cat = c.category; ImGui::SeparatorText(cat.c_str()); }
        const ImVec4 col = c.edited ? ImVec4(0.5f, 1, 0.5f, 1) : c.changed ? ImVec4(1, 0.85f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);   // green MODIFIED here, yellow set by the data, grey default
        ImGui::TextColored(col, "%s", c.name.c_str());
        if (ImGui::IsItemHovered() || ImGui::IsItemClicked()) s_field = c.name;
        ImGui::SameLine(110);
        const bool editable = !c.readonly && (c.type == "bool" || c.type == "float" || c.type == "int" || c.type == "string" || c.type == "colour" || c.type == "enum" || c.type == "file");
        ImGui::PushID(c.name.c_str());
        if (editable) {
            static char buf[256]; std::snprintf(buf, sizeof buf, "%s", c.value.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##v", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                std::string err;
                if (g.edits->Set(g.selected, c.name, buf, err)) g.status.clear(); else g.status = c.name + ": " + err;
                Refresh();
            }
            if (c.bitset && c.type == "enum") {   // checklist: enum value N <-> bit 1 << N; text form = names joined with |
                const md::fcsdef::EnumDef* en = g.schema.FindEnum(c.enum_name);
                if (en && !en->values.empty() && ImGui::TreeNode("##bits", "bits")) {
                    unsigned mask = 0; { std::string cur; for (size_t i = 0; i <= c.value.size(); ++i) if (i == c.value.size() || c.value[i] == '|') { for (const auto& ev : en->values) if (ev.name == cur || c.enum_name + "." + ev.name == cur) mask |= 1u << ev.value; cur.clear(); } else cur += c.value[i]; }
                    unsigned nm = mask; bool all = false;
                    if (ImGui::SmallButton("all")) { nm = 0; for (const auto& ev : en->values) nm |= 1u << ev.value; all = true; }
                    for (const auto& ev : en->values) { bool b = (nm >> ev.value) & 1u; if (ImGui::Checkbox(ev.name.c_str(), &b)) { if (b) nm |= 1u << ev.value; else nm &= ~(1u << ev.value); all = true; } }
                    if (all && nm != mask) { std::string t; for (const auto& ev : en->values) if ((nm >> ev.value) & 1u) { if (!t.empty()) t += "|"; t += ev.name; } std::string err; g.edits->Set(g.selected, c.name, t.empty() ? "0" : t, err); Refresh(); }
                    ImGui::TreePop();
                }
            }
            if (c.edited) { ImGui::SameLine(); if (ImGui::SmallButton("revert")) { g.edits->Revert(g.selected, c.name); Refresh(); } }
        } else {
            const std::string v = c.value.size() > 60 ? c.value.substr(0, 57) + "..." : c.value; ImGui::TextUnformatted(v.c_str());
            if (c.value.size() > 60 && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.value.c_str());
        }
        ImGui::PopID();
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
            ImGui::Text("%s  ->  %s%s", c.id.c_str(), c.target_name.c_str(), c.states.empty() ? "" : "  [states]");
            float pos[3] = { c.pos[0], c.pos[1], c.pos[2] };
            ImGui::SameLine(300); ImGui::SetNextItemWidth(260);
            if (ImGui::InputFloat3("##ip", pos, "%.1f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                for (md::fcs::Instance in : g.edits->Instances(g.selected)) if (in.id == c.id) { for (int k = 0; k < 3; ++k) in.position[k] = pos[k]; g.edits->SetInstance(g.selected, in); break; }
                Refresh();
            }
            ImGui::SameLine(); if (ImGui::SmallButton("x")) { g.edits->RemoveInstance(g.selected, c.id); Refresh(); }
            ImGui::PopID();
        }
    }
    if (!g.selected.empty()) { const int k = g.edits->KindOf(g.selected); if (k == md::fcsdef::kItemDialogue || k == md::fcsdef::kItemDialogueLine || k == md::fcsdef::kItemDialoguePackage) DrawDialogue(k); }
    ImGui::EndChild();   // kd_grid
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("kd_refs", ImVec2(0, 0), true);
    if (g.show_refs && !g.selected.empty()) {
        for (const md::fcsdef::RefHit& r : md::fcsdef::Referencing(*g.edits, g.data, g.selected)) if (ImGui::Selectable((r.name + "   (" + r.category + ")##rb" + r.string_id + r.category).c_str())) Go(r.string_id);
    }
    ImGui::EndChild();
    ImGui::EndChild();   // kd_item_top
    ImGui::BeginChild("kd_item_desc", ImVec2(0, 0), false);
    ImGui::TextUnformatted("Selected Item");
    for (const md::fcsdef::CardField& c : g.card) if (c.name == s_field) { ImGui::SameLine(); ImGui::TextDisabled("%s  (%s, default %s)", c.name.c_str(), c.type.c_str(), c.def.c_str()); ImGui::TextWrapped("%s", c.description.c_str()); break; }
    ImGui::EndChild();
    ImGui::End();
}

void DrawInfoWin() {
    if (!BeginWin(g.w_info, "Info", 681, 498)) return;
        ImGui::SetNextItemWidth(160); ImGui::InputText("name##mi", g.mod_name, sizeof g.mod_name); ImGui::SameLine();
        ImGui::SetNextItemWidth(120); ImGui::InputText("author##mi", g.mod_author, sizeof g.mod_author);
        ImGui::SetNextItemWidth(500); ImGui::InputText("description##mi", g.mod_desc, sizeof g.mod_desc);
        ImGui::SetNextItemWidth(500); ImGui::InputText("dependencies##mi", g.mod_deps, sizeof g.mod_deps);
        ImGui::SetNextItemWidth(500); ImGui::InputText("referenced (not required)##mi", g.mod_refs, sizeof g.mod_refs);
        if (ImGui::Button("New mod (discard edits)")) NewMod();
        ImGui::SameLine(); if (ImGui::Button("Close all (back to the install)")) { std::string err; if (LoadFiles({}, err)) { Rebuild(); g.status = "closed: install load order"; } else g.status = err; }
        ImGui::SetNextItemWidth(420); ImGui::InputTextWithHint("##single", "path of a .mod / .base to open alone", g.single_path, sizeof g.single_path); ImGui::SameLine();
        if (ImGui::Button("Open single file")) {
            md::fcs::LoadEntry e; e.path = g.single_path; e.name = std::filesystem::path(e.path).filename().string(); e.core = e.name == "gamedata.base";
            std::string err; if (LoadFiles({e}, err)) { Rebuild(); g.status = "opened " + e.name + " alone (single-file mode)"; } else g.status = "open failed: " + err;
        }
        if (ImGui::Button("Export mods.cfg of the loaded mods")) {   // a copy in temp_/md_mods/, never over the install's data/mods.cfg
            std::filesystem::create_directories("temp_/md_mods");
            FILE* o = std::fopen("temp_/md_mods/mods.cfg", "wb");
            if (o) { for (const md::fcs::LoadEntry& m : g.mods) std::fprintf(o, "%s\n", m.name.c_str()); std::fclose(o); g.status = "wrote temp_/md_mods/mods.cfg (" + std::to_string(g.mods.size()) + " mods)"; } else g.status = "cannot write mods.cfg";
        }
        ImGui::SameLine(); ImGui::TextDisabled("saves to temp_/md_mods/%s.mod", g.mod_name);
    ImGui::End();
}

void DrawChangesWin() {
    if (!BeginWin(g.w_changes, "Change List", 447, 575)) return;
        for (const md::fcsdef::ChangeRow& r : md::fcsdef::ChangeList(*g.edits)) {
            if (ImGui::Selectable((r.state + "  " + r.name + "   " + r.what + "##ch" + r.string_id).c_str(), g.selected == r.string_id)) Go(r.string_id);
        }
    ImGui::End();
}

void DrawSettingsWin() {
    if (!BeginWin(g.w_settings, "FCS Settings", 346, 248)) return;
        ImGui::TextDisabled("data folder: %s", md::KenshiDir().c_str());
        for (const auto& kv : g.schema.settings) ImGui::BulletText("%s = %s", kv.first.c_str(), kv.second.c_str());
        ImGui::SetNextItemWidth(120); ImGui::InputInt("autosave every (min, 0 = off)", &g.autosave_min, 1, 5); if (g.autosave_min < 0) g.autosave_min = 0;
    ImGui::End();
}

void DrawAssetsWin() {
    if (!BeginWin(g.w_assets, "Assets", 503, 534)) return;
        if (ImGui::Button("Scan")) { g.assets = md::fcsdef::AssetList(*g.edits, g.data, md::KenshiDir()); g.assets_ready = true; }
        if (g.assets_ready) {
            size_t missing = 0; for (const md::fcsdef::AssetRow& a : g.assets) missing += !a.found;
            ImGui::SameLine(); ImGui::Text("%zu files, %zu not found under the install", g.assets.size(), missing);
            for (size_t i = 0; i < g.assets.size() && i < 200; ++i) { const md::fcsdef::AssetRow& a = g.assets[i]; ImGui::TextColored(a.found ? ImVec4(0.7f, 0.7f, 0.7f, 1) : ImVec4(1, 0.5f, 0.4f, 1), "%s  (%s, %zu record%s, e.g. %s)", a.file.c_str(), a.kind_name.c_str(), a.records, a.records == 1 ? "" : "s", a.example_record.c_str()); }
        }
    ImGui::End();
}

void DrawErrorsWin() {
    if (!BeginWin(g.w_errors, "Errors", 984, 469)) return;
    for (const auto& pr : g.problems) {
        ImGui::PushID(&pr);
        const ImVec4 col = pr.level == md::fcsdef::Problem::Critical ? ImVec4(1, 0.3f, 0.3f, 1) : pr.level == md::fcsdef::Problem::Warning ? ImVec4(1, 0.8f, 0.3f, 1) : ImVec4(0.6f, 0.8f, 1, 1);
        ImGui::TextColored(col, "%s %s  %s.%s: %s", pr.level == md::fcsdef::Problem::Critical ? "CRITICAL" : pr.level == md::fcsdef::Problem::Warning ? "warning" : "info", pr.code.c_str(), g.edits->NameOf(pr.record).c_str(), pr.field.c_str(), pr.what.c_str());
        struct A { unsigned bit; const char* label; md::fcsdef::ErrorAction act; };
        bool applied = false;
        for (const A& a : {A{md::fcsdef::kRemoveInvalidReferences, "remove invalid links", md::fcsdef::kRemoveInvalidReferences}, A{md::fcsdef::kRevertItem, "revert item", md::fcsdef::kRevertItem}, A{md::fcsdef::kDeleteItem, "delete item", md::fcsdef::kDeleteItem}})
            if (pr.actions & a.bit) { ImGui::SameLine(); if (ImGui::SmallButton(a.label)) { md::fcsdef::ApplyFix(*g.edits, g.schema, pr, a.act); applied = true; } }
        ImGui::PopID();
        if (applied) { g.problems = md::fcsdef::CheckMod(*g.edits, g.schema, g.data); Refresh(); break; }
    }
    ImGui::End();
}

void DrawFindWin() {
    if (!BeginWin(g.w_find, "Find and replace", 634, 333)) return;
    ImGui::SetNextItemWidth(-60); if (ImGui::InputTextWithHint("##s", "search all data", g.search, sizeof g.search, ImGuiInputTextFlags_EnterReturnsTrue)) g.hits = md::fcsdef::SearchAll(*g.edits, g.data, g.search, g.node ? g.node->type : "");
    ImGui::SameLine(); if (ImGui::Button("all")) g.hits = md::fcsdef::SearchAll(*g.edits, g.data, g.search);
    if (!g.hits.empty()) {
        ImGui::Text("%zu hits", g.hits.size()); ImGui::SameLine(); if (ImGui::SmallButton("close")) g.hits.clear();
        for (const md::fcsdef::Hit& h : g.hits) if (ImGui::Selectable((h.name + "  [" + md::fcs::ItemTypeName(g.edits->KindOf(h.string_id)) + ", " + h.where + "]##" + h.string_id).c_str(), g.selected == h.string_id)) Go(h.string_id);
        ImGui::Separator();
    }
    ImGui::Separator();
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
    ImGui::End();
}

}  // namespace

void Draw() {
    if (!g.tried) Load();
    if (!g.ok) { ImGui::TextWrapped("Kenshi data: %s", g.error.empty() ? "not loaded" : g.error.c_str()); return; }
    if (g.autosave_min > 0 && g.edits->TouchedRecords().size()) {   // FCS autosave: a copy next to the saved mod, never the install
        const double now = ImGui::GetTime();
        if (g.last_autosave == 0) g.last_autosave = now;
        if (now - g.last_autosave >= g.autosave_min * 60.0) {
            g.last_autosave = now; std::filesystem::create_directories("temp_/md_mods");
            std::string err; md::fcsdef::ModInfo info; info.author = g.mod_author; info.description = g.mod_desc; info.dependencies = g.mod_deps; info.references = g.mod_refs;
            if (md::fcsdef::WriteMod(std::string("temp_/md_mods/") + g.mod_name + ".autosave.mod", *g.edits, g.schema, info, err, md::KenshiDir() + "/data")) g.status = "autosaved"; else g.status = "autosave failed: " + err;
        }
    }
    static const char* const dump_path = std::getenv("MD_FCS_LAYOUT_DUMP"); static int dump_frames = 0;
    if (dump_path && dump_frames == 0) g.w_info = g.w_changes = g.w_find = g.w_settings = g.w_assets = g.w_errors = true;
    s_dump.clear();
    DrawToolbar();
    if (!g.status.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g.status.c_str());
    s_origin = ImGui::GetCursorScreenPos();
    DrawGameWorld(); DrawItemWin(); DrawInfoWin(); DrawChangesWin(); DrawFindWin(); DrawSettingsWin(); DrawAssetsWin(); DrawErrorsWin();
    if (dump_path && ++dump_frames == 5) { if (FILE* o = std::fopen(dump_path, "wb")) { std::fwrite(s_dump.data(), 1, s_dump.size(), o); std::fclose(o); } }
}


}  // namespace KenshiDataPanel
