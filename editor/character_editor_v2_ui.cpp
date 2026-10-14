#include "character/appearance.h"
#include "character/session.h"
#include "character_editor_v2_ui.h"
#include "editor_char_preview_sdlgpu.h"
#include "imgui.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace CharEditorV2 {
using namespace md::character;
namespace {
Session g_session;
bool g_active = false;
int g_tab = 0;                    // 0 BODY, 1 FACE, 2 HAIR
char g_name[48] = "";
bool g_clothes = true;

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
    ImGui::SetNextItemWidth(w - label_w - 2.f * aw - 40.f - 10.f);
    if (ImGui::SliderInt("##v", &iv, 0, hi, "")) changed = true;
    ImGui::SameLine(0.f, 2.f);
    if (ImGui::ArrowButton("r", ImGuiDir_Right)) { iv += 1; changed = true; }
    ImGui::SameLine(0.f, 4.f);
    auto col = ColourAt(c, iv);
    ImGui::ColorButton("##c", ImVec4(col[0] / 255.f, col[1] / 255.f, col[2] / 255.f, 1.f), ImGuiColorEditFlags_NoTooltip, {32.f, ImGui::GetFrameHeight()});
    ImGui::PopID();
    if (changed) g_session.SetValue(c.name, float(std::max(0, std::min(hi, iv))));
}

const char* kTabCategory[3] = {"Body", "Face", "Hair"};
}

bool Enable(bool on) {
    if (!on) { g_active = false; CharPreviewSDLGPU::V2Off(); return true; }
    if (!g_session.Loaded()) {
        std::string dir = md::KenshiDir(), err;
        if (dir.empty()) { fprintf(stderr, "[CharEditorV2] KENSHI_DIR not set\n"); return false; }
        if (!g_session.Load(dir, err)) { fprintf(stderr, "[CharEditorV2] %s\n", err.c_str()); return false; }
    }
    g_active = CharPreviewSDLGPU::V2Bind(&g_session);
    return g_active;
}
bool Active() { return g_active; }

void Draw() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float W = avail.x, H = avail.y, sp = ImGui::GetStyle().ItemSpacing.x;
    const float left_w = W * 0.161f, right_w = W * 0.30f, mid_w = W - left_w - right_w - 2.f * sp;
    const Session& s = g_session;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // ── left column: NamePanel, DescriptionPanel, StatsPanel, ConfirmPanel (fractions of §3a) ──
    ImGui::BeginGroup();
    ImGui::BeginChild("##v2_name", {left_w, H * 0.398f}, true, ImGuiWindowFlags_NoScrollbar);
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
    ImGui::Button("IMPORT", {hw, 0.f}); ImGui::SameLine(0.f, sp); ImGui::Button("EXPORT", {hw, 0.f});
    ImGui::Checkbox("CLOTHES", &g_clothes);
    ImGui::TextDisabled("CHANGE CHARACTER");
    NavRow("chr", "1/1", false);
    ImGui::EndChild();
    ImGui::BeginChild("##v2_desc", {left_w, H * 0.241f}, true);
    ImGui::TextDisabled("RACE DESCRIPTION");
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(s.Current().description.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::BeginChild("##v2_stats", {left_w, H * 0.241f}, true);
    ImGui::TextDisabled("RACE STATS");
    ImGui::EndChild();
    ImGui::BeginChild("##v2_confirm", {left_w, H * 0.096f}, true, ImGuiWindowFlags_NoScrollbar);
    ImGui::Button("CONFIRM", {-1.f, -1.f});
    ImGui::EndChild();
    ImGui::EndGroup();

    // ── centre: the 3D preview (it moves the cursor itself, so every column is placed by screen position) ──
    ImGui::SetCursorScreenPos({origin.x + left_w + sp, origin.y});
    ImGui::BeginGroup();
    const float skin[3] = {0.82f, 0.65f, 0.52f};
    CharPreviewSDLGPU::DrawInImGui(mid_w, H, 1.f, 1.f, skin, 0.f, 1.f, 0.f);
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

} // namespace CharEditorV2
