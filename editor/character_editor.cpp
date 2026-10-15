#include "character/appearance.h"
#include "character/appearance_io.h"
#include "character/race_stats.h"
#include "character/session.h"
#include "character_editor.h"
#include "editor_char_preview.h"
#include "imgui.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace CharacterEditor {
bool g_detached = false;
ImVec2 g_win_pos = {160.f, 60.f};
ImVec2 g_win_size = {900.f, 640.f};

using namespace md::character;
namespace {
Session g_session;
int g_tab = 0;                    // 0 BODY, 1 FACE, 2 HAIR
char g_name[48] = "";
bool g_clothes = true;
int g_io_mode = 0;                // 1 IMPORT, 2 EXPORT
bool g_io_open = false;
char g_io_path[512] = "";
std::string g_io_msg;
std::string g_confirm_msg;        // result of the last CONFIRM, shown in the RACE STATS panel header

// CONFIRM: the editor has no game character to give the appearance to; it writes the .bod2 to the last EXPORT / IMPORT path (character.bod2 before the first one).
bool Confirm() {
    std::string err;
    const std::string path = g_io_path[0] ? g_io_path : "character.bod2";
    const bool ok = md::character::ExportAppearance(g_session, path, err);
    g_confirm_msg = ok ? "Saved " + path : "Not saved: " + err;
    return ok;
}

// ◀ label ▶ row: returns -1 / +1 when an arrow is pressed
int NavRow(const char* id, const char* text, bool enabled) {
    int step = 0;
    const float aw = ImGui::GetFrameHeight();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(id);
    if (!enabled) ImGui::BeginDisabled();
    if (ImGui::ArrowButton("l", ImGuiDir_Left)) step = -1;
    ImGui::SameLine(0.f, 2.f);
    ImGui::Button(text, {w - 2.f * aw - 4.f, 0.f});
    ImGui::SameLine(0.f, 2.f);
    if (ImGui::ArrowButton("r", ImGuiDir_Right)) step = 1;
    if (!enabled) ImGui::EndDisabled();
    ImGui::PopID();
    return step;
}

void SliderRow(const Range& r) {
    float v = g_session.Value(r);
    ImGui::PushID(r.name.c_str());
    const float aw = ImGui::GetFrameHeight(), w = ImGui::GetContentRegionAvail().x;
    const float label_w = w * 0.34f, val_w = 34.f;
    ImGui::Button(r.name.c_str(), {label_w, 0.f});
    ImGui::SameLine(0.f, 2.f);
    bool changed = false;
    if (ImGui::ArrowButton("l", ImGuiDir_Left)) { v -= 1.f; changed = true; }
    ImGui::SameLine(0.f, 2.f);
    ImGui::SetNextItemWidth(w - label_w - 2.f * aw - val_w - 10.f);
    int iv = int(v);
    if (ImGui::SliderInt("##v", &iv, r.min, r.max, "")) { v = float(iv); changed = true; }
    ImGui::SameLine(0.f, 2.f);
    if (ImGui::ArrowButton("r", ImGuiDir_Right)) { v += 1.f; changed = true; }
    ImGui::SameLine(0.f, 4.f);
    ImGui::Text("%d", int(v));
    ImGui::PopID();
    if (changed) g_session.SetValue(r.name, float(std::max(r.min, std::min(r.max, int(v)))));
}

void ColourRow(const ColourRange& c) {
    float v = g_session.Values().values.count(c.name) ? g_session.Values().values.at(c.name) : float(c.has_mid ? c.mid : 0);
    const int hi = c.colours.empty() ? 0 : int((c.colours.size() - 1) * 10);
    ImGui::PushID(c.name.c_str());
    const float aw = ImGui::GetFrameHeight(), w = ImGui::GetContentRegionAvail().x;
    const float label_w = w * 0.34f;
    ImGui::Button(c.name.c_str(), {label_w, 0.f});
    ImGui::SameLine(0.f, 2.f);
    bool changed = false; int iv = int(v);
    if (ImGui::ArrowButton("l", ImGuiDir_Left)) { iv -= 1; changed = true; }
    ImGui::SameLine(0.f, 2.f);
    ImGui::SetNextItemWidth(w - label_w - 2.f * aw - 34.f - 10.f);
    if (ImGui::SliderInt("##v", &iv, 0, hi, "")) changed = true;
    ImGui::SameLine(0.f, 2.f);
    if (ImGui::ArrowButton("r", ImGuiDir_Right)) { iv += 1; changed = true; }
    ImGui::SameLine(0.f, 4.f);
    ImGui::Text("%d", iv);
    ImGui::PopID();
    if (changed) g_session.SetValue(c.name, float(std::max(0, std::min(hi, iv))));
}

const char* kTabCategory[3] = {"Body", "Face", "Hair"};
}

// Loads the install (KENSHI_DIR) on the first call. False when it is missing: the tab then says so.
std::string g_load_error = "Kenshi not found: set KENSHI_DIR";
bool EnsureLoaded() {
    if (g_session.Loaded()) return true;
    static bool tried = false;
    if (tried) return false;
    tried = true;
    std::string dir = md::KenshiDir(), err;
    if (dir.empty()) { g_load_error = "Kenshi not found: set KENSHI_DIR"; fprintf(stderr, "[CharacterEditor] %s\n", g_load_error.c_str()); return false; }
    if (!g_session.Load(dir, err)) { g_load_error = "Kenshi data in " + dir + " not usable: " + err; fprintf(stderr, "[CharacterEditor] %s\n", g_load_error.c_str()); return false; }
    CharPreviewSDLGPU::Bind(&g_session);
    return true;
}

bool ConfirmForScenario() { return Confirm(); }
void SetTab(int tab) { g_tab = tab < 0 ? 0 : tab > 2 ? 2 : tab; }

md::character::Session& GetSession() { return g_session; }

void Draw(bool) {
    if (!EnsureLoaded()) { ImGui::TextWrapped("%s", g_load_error.c_str()); return; }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float W = floorf(avail.x), H = floorf(avail.y), sp = ImGui::GetStyle().ItemSpacing.x;
    // whole pixels everywhere: fractional child sizes move the label glyphs by a fraction of a pixel
    const float left_w = floorf(W * 0.161f), right_w = floorf(W * 0.30f), mid_w = floorf(W - left_w - right_w - 2.f * sp);
    const Session& s = g_session;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // ── left column: NamePanel, DescriptionPanel, StatsPanel, ConfirmPanel (fractions of §3a) ──
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f, 2.f});   // the panel of §3a is 0.398 of the height: tighter rows so that CHANGE CHARACTER fits at 706 px
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 1.f});
    ImGui::BeginChild("##v2_name", {left_w, floorf(H * 0.398f)}, true, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##name", "NAME", g_name, sizeof g_name);
    ImGui::TextDisabled("RACE");
    int d = NavRow("race", s.Race(s.race_index()).name.c_str(), s.RaceCount() > 1);
    if (d) g_session.SelectRace((s.race_index() + s.RaceCount() + d) % s.RaceCount());
    ImGui::TextDisabled("SUBRACE");
    d = NavRow("sub", s.Current().name.c_str(), s.SubraceCount() > 1);
    if (d) g_session.SelectSubrace((s.subrace_index() + s.SubraceCount() + d) % s.SubraceCount());
    ImGui::TextDisabled("GENDER");
    d = NavRow("gen", s.SingleGender() ? "-" : (s.gender() == Gender::Female ? "Female" : "Male"), !s.SingleGender());
    if (d) g_session.SetGender(s.gender() == Gender::Female ? Gender::Male : Gender::Female);
    const float hw = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
    if (ImGui::Button("IMPORT", {hw, 0.f})) { g_io_mode = 1; g_io_open = true; }
    ImGui::SameLine(0.f, sp);
    if (ImGui::Button("EXPORT", {hw, 0.f})) { g_io_mode = 2; g_io_open = true; }
    ImGui::Checkbox("CLOTHES", &g_clothes);
    ImGui::TextDisabled("CHANGE CHARACTER");
    NavRow("chr", "1/1", false);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::BeginChild("##v2_desc", {left_w, floorf(H * 0.241f)}, true);
    ImGui::TextDisabled("RACE DESCRIPTION");
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(s.Current().description.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::BeginChild("##v2_stats", {left_w, floorf(H * 0.241f)}, true);
    ImGui::TextDisabled("RACE STATS");
    if (!g_confirm_msg.empty()) ImGui::TextWrapped("%s", g_confirm_msg.c_str());
    for (const md::character::StatRow& row : md::character::RaceStatRows(s.Current())) {
        ImGui::TextUnformatted(row.label.c_str());
        if (row.value.empty()) continue;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(row.value.c_str()).x);
        if (row.bad) ImGui::TextColored({0.85f, 0.42f, 0.36f, 1.f}, "%s", row.value.c_str()); else ImGui::TextUnformatted(row.value.c_str());
    }
    ImGui::EndChild();
    ImGui::BeginChild("##v2_confirm", {left_w, floorf(H * 0.096f)}, true, ImGuiWindowFlags_NoScrollbar);
    if (ImGui::Button("CONFIRM", {-1.f, -1.f})) Confirm();
    ImGui::EndChild();
    ImGui::EndGroup();

    if (g_io_open) {
        if (!g_io_path[0] && g_io_mode == 2) snprintf(g_io_path, sizeof g_io_path, "character.bod2");
        ImGui::OpenPopup("##io");
        g_io_open = false; g_io_msg.clear();
    }
    if (ImGui::BeginPopupModal("##io", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(g_io_mode == 1 ? "IMPORT: path of a .bod2" : "EXPORT: path of the .bod2 to write");
        ImGui::SetNextItemWidth(420.f);
        ImGui::InputText("##path", g_io_path, sizeof g_io_path);
        if (ImGui::Button("OK", {100.f, 0.f})) {
            std::string err;
            bool ok = g_io_mode == 1 ? md::character::ImportAppearance(g_session, g_io_path, err) : md::character::ExportAppearance(g_session, g_io_path, err);
            if (ok) ImGui::CloseCurrentPopup(); else g_io_msg = err;
        }
        ImGui::SameLine();
        if (ImGui::Button("CANCEL", {100.f, 0.f})) ImGui::CloseCurrentPopup();
        if (!g_io_msg.empty()) ImGui::TextColored({0.85f, 0.42f, 0.36f, 1.f}, "%s", g_io_msg.c_str());
        ImGui::EndPopup();
    }

    // ── centre: the 3D preview (it moves the cursor itself, so every column is placed by screen position) ──
    ImGui::SetCursorScreenPos({origin.x + left_w + sp, origin.y});
    ImGui::BeginGroup();
    CharPreviewSDLGPU::DrawInImGui(mid_w, H);
    ImGui::EndGroup();

    // ── right column: TabsPanel ──
    ImGui::SetCursorScreenPos({origin.x + left_w + mid_w + 2.f * sp, origin.y});
    ImGui::BeginChild("##v2_tabs", {right_w, H}, true, ImGuiWindowFlags_NoScrollbar);
    const float tw = (ImGui::GetContentRegionAvail().x - 2.f * sp) / 3.f;
    static const char* kTabs[3] = {"BODY", "FACE", "HAIR"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0.f, sp);
        if (i == g_tab) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kTabs[i], {tw, 0.f})) g_tab = i;
        if (i == g_tab) ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::BeginChild("##v2_content", {0.f, -ImGui::GetFrameHeightWithSpacing() - 4.f}, false);
    if (const Config* cfg = s.Limits()) {
        if (const Category* cat = cfg->Find(kTabCategory[g_tab])) {
            if (g_tab == 1 && s.HeadCount()) {
                char t[96];
                const md::fcs::GameData* hd = s.Data().Get(s.HeadId(s.head()));
                std::snprintf(t, sizeof t, "Face  %s  %d/%d", hd ? hd->name.c_str() : "", int(s.head()) + 1, int(s.HeadCount()));
                if (int d = NavRow("head", t, true)) g_session.SelectHead(int(s.head()) + d);
            }
            if (g_tab == 0 && s.StanceCount()) {
                char t[64];
                std::snprintf(t, sizeof t, "Idle stance  %d/%d", int(s.stance()) + 1, int(s.StanceCount()));
                if (int d = NavRow("stance", t, true)) g_session.SelectStance(int(s.stance()) + d);
            }
            for (const ColourRange& c : cat->colour_ranges) ColourRow(c);
            for (const Range& r : cat->ranges) SliderRow(r);
            if (cat->ranges.empty() && cat->colour_ranges.empty()) ImGui::TextDisabled("(nothing to adjust)");
        } else ImGui::TextDisabled("(no %s sliders)", kTabCategory[g_tab]);
    } else ImGui::TextDisabled("(this race has no editor limits)");
    ImGui::EndChild();
    const float bw = (ImGui::GetContentRegionAvail().x - 2.f * sp) / 3.f;
    if (ImGui::Button("RANDOMISE ALL", {bw, 0.f})) g_session.RandomiseAll();
    ImGui::SameLine(0.f, sp);
    if (ImGui::Button("RANDOMISE", {bw, 0.f})) g_session.RandomiseCategory(kTabCategory[g_tab]);
    ImGui::SameLine(0.f, sp);
    if (ImGui::Button("RESET", {bw, 0.f})) g_session.Reset();
    ImGui::EndChild();
}

} // namespace CharacterEditor
