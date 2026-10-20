#include "character_mygui.h"

#ifdef MD_CHARACTER_MYGUI
#include "character/appearance_io.h"
#include "character/race_stats.h"
#include "character/session.h"
#include "character_editor.h"
#include "editor_char_preview.h"
#include "imgui.h"
#include <MyGUI.h>
#include <monkey_dust/ui/kenshi_widgets.h>
#include <monkey_dust/ui/mygui_sdlgpu.h>
#include <monkey_dust/platform/window.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using md::character::Gender;
using md::character::Session;

namespace CharacterMyGui {
namespace {

struct State {
    bool ok = false, tried = false, visible = false;
    Session* s = nullptr;
    int tab = 0;                            // 0 BODY, 1 FACE, 2 HAIR (editor numbering; the exe's categories are BODY 2, FACE 1, HAIR 3)
    uint64_t seen_revision = ~0ull;
    int built_tab = -1;
    size_t built_race = ~size_t(0), built_sub = ~size_t(0);
    Gender built_gender = Gender::Single;
    float x = 0, y = 0, w = 0, h = 0;       // region on the window, pixels
    MyGUI::Widget* root = nullptr;
    MyGUI::ScrollView* content = nullptr;
    bool updating = false;                  // widgets are being set from the session: their events must not write back
} g;

MyGUI::Widget* W(const char* name) {
    return MyGUI::Gui::getInstance().findWidgetT(name, false);
}
template <class T> T* WT(const char* name) { MyGUI::Widget* w = W(name); return w ? w->castType<T>(false) : nullptr; }

void SetText(const char* name, const std::string& t) {
    if (MyGUI::TextBox* b = WT<MyGUI::TextBox>(name)) b->setCaption(MyGUI::UString(t));
    else if (MyGUI::EditBox* e = WT<MyGUI::EditBox>(name)) e->setCaption(MyGUI::UString(t));
}

const char* kCategory[3] = {"Body", "Face", "Hair"};

std::string Colour(const char* palette_name) {
    float c[3];
    char b[16] = "#ffffff";
    if (mdui::MyGuiColour(palette_name, c)) std::snprintf(b, sizeof b, "#%02X%02X%02X", int(c[0] * 255.f + .5f), int(c[1] * 255.f + .5f), int(c[2] * 255.f + .5f));
    return b;
}

void ShowTabButtons() {
    const char* names[3] = {"BodyTabButton", "FaceTabButton", "HairTabButton"};
    for (int i = 0; i < 3; ++i) if (MyGUI::Button* b = WT<MyGUI::Button>(names[i])) b->setStateSelected(i == g.tab);
}

// ── tab content: rows built like the exe (FUN_1405ebd50): Slider / ListScrollBar / MultiSlider templates stacked in the ScrollView ──
const int kRowGap = 5;

MyGUI::Widget* AddRow(const char* type, const char* skin, int height, int& y) {
    const int w = std::max(100, g.content->getWidth() - 24);   // the view's own width less the scroll bar (its client area is not known before the canvas has a size)
    MyGUI::Widget* r = g.content->createWidgetT(type, skin, MyGUI::IntCoord(0, y, w, height), MyGUI::Align::Left | MyGUI::Align::Top);
    y += height + kRowGap;
    return r;
}

void OnSlider(mdui::Slider* sl, int pos) {
    if (g.updating) return;
    g.s->SetValue(sl->getUserString("slider"), float(pos));
}

mdui::Slider* AddSlider(const std::string& caption, const std::string& key, int lo, int hi, int value, int& y) {
    auto* sl = static_cast<mdui::Slider*>(AddRow("Slider", "Kenshi_Slider", 55, y));
    sl->setCaption(MyGUI::UString(caption));
    sl->setScrollRange(lo, hi);
    sl->setScrollPosition(value);
    sl->setUserString("slider", key);
    sl->eventScrollChangePosition += MyGUI::newDelegate(OnSlider);
    return sl;
}

void BuildContent() {
    const Session& s = *g.s;
    while (g.content->getChildCount()) MyGUI::Gui::getInstance().destroyWidget(g.content->getChildAt(0));
    int y = 0;
    g.content->setCanvasSize(std::max(100, g.content->getWidth() - 24), 4000);   // a canvas of the final width before the rows are placed in it
    using namespace md::character;
    const Config* cfg = s.Limits();
    const Category* cat = cfg ? cfg->Find(kCategory[g.tab]) : nullptr;
    g.updating = true;
    if (g.tab == 0 && s.StanceCount()) {
        auto* sl = AddSlider("Idle stance", "", 1, int(s.StanceCount()), int(s.stance()) + 1, y);
        sl->eventScrollChangePosition.clear();
        sl->eventScrollChangePosition += MyGUI::newDelegate(+[](mdui::Slider*, int pos) { if (!g.updating) g.s->SelectStance(pos - 1 - int(g.s->stance())); });
    }
    if (g.tab == 1 && s.HeadCount()) {
        auto* ls = static_cast<mdui::ListScrollBar*>(AddRow("ListScrollBar", "Kenshi_ListSlider", 55, y));
        std::vector<std::string> names;
        for (size_t i = 0; i < s.HeadCount(); ++i) { const md::fcs::GameData* hd = s.Data().Get(s.HeadId(i)); names.push_back(hd ? hd->name : s.HeadId(i)); }
        ls->setCaption("Face");
        ls->setValues(names);
        ls->setValueSelected(names[s.head()]);
        ls->eventValueChanged += MyGUI::newDelegate(+[](mdui::ListScrollBar*, const std::string&, int i) { if (!g.updating) g.s->SelectHead(i - int(g.s->head())); });
    }
    if (g.tab == 2 && s.HairCount()) {
        auto* ls = static_cast<mdui::ListScrollBar*>(AddRow("ListScrollBar", "Kenshi_ListSlider", 55, y));
        std::vector<std::string> names;
        for (size_t i = 0; i < s.HairCount(); ++i) names.push_back(s.Hair(i).name);
        ls->setCaption("Hair style");
        ls->setValues(names);
        ls->setValueSelected(names[s.hair_index()]);
        ls->eventValueChanged += MyGUI::newDelegate(+[](mdui::ListScrollBar*, const std::string&, int i) { if (!g.updating) g.s->SelectHair(size_t(i)); });
        if (s.BeardCount() && s.gender() != Gender::Female) {
            auto* lb = static_cast<mdui::ListScrollBar*>(AddRow("ListScrollBar", "Kenshi_ListSlider", 55, y));
            std::vector<std::string> bn;
            for (size_t i = 0; i < s.BeardCount(); ++i) bn.push_back(s.Beard(i).name);
            lb->setCaption("Beard");
            lb->setValues(bn);
            lb->setValueSelected(bn[s.beard_index()]);
            lb->eventValueChanged += MyGUI::newDelegate(+[](mdui::ListScrollBar*, const std::string&, int i) { if (!g.updating) g.s->SelectBeard(size_t(i)); });
        }
        auto* ms = static_cast<mdui::MultiSlider*>(AddRow("MultiSlider", "Kenshi_MultiSlider", 220, y));
        ms->setCaption("Hair colour");
        static const char* kKey[3] = {"Hair Colour", "Hair Saturation", "Hair Brightness"};
        static const char* kCap[3] = {"Colour", "Saturation", "Brightness"};
        for (size_t i = 0; i < 3 && i < ms->getSlidersCount(); ++i) {
            mdui::Slider* sl = ms->getSlider(i);
            sl->setCaption(kCap[i]);
            sl->setScrollRange(0, 100);
            Range r; r.name = kKey[i]; r.min = 0; r.max = 100;
            sl->setScrollPosition(int(s.Value(r)));
            sl->setUserString("slider", kKey[i]);
        }
        ms->eventScrollChangePosition += MyGUI::newDelegate(+[](mdui::MultiSlider*, mdui::Slider* sl, int pos, int) { if (!g.updating) g.s->SetValue(sl->getUserString("slider"), float(pos)); });
    }
    if (cat) {
        for (const ColourRange& c : cat->colour_ranges) {
            const int hi = c.colours.empty() ? 0 : int((c.colours.size() - 1) * 10);
            const auto& vals = s.Values().values;
            AddSlider(c.name, c.name, 0, hi, vals.count(c.name) ? int(vals.at(c.name)) : (c.has_mid ? c.mid : 0), y);
        }
        for (const Range& r : cat->ranges) AddSlider(r.name, r.name, r.min, r.max, int(s.Value(r)), y);
    }
    g.updating = false;
    g.content->setCanvasSize(std::max(100, g.content->getWidth() - 24), std::max(y, 1));
    g.content->setViewOffset(MyGUI::IntPoint(0, 0));
    g.built_tab = g.tab; g.built_race = s.race_index(); g.built_sub = s.subrace_index(); g.built_gender = s.gender();
}

void RefreshSliders() {   // values changed (RANDOMISE / RESET / IMPORT): put them on the existing sliders
    const Session& s = *g.s;
    g.updating = true;
    for (size_t i = 0; i < g.content->getChildCount(); ++i) {
        MyGUI::Widget* c = g.content->getChildAt(i);
        if (auto* sl = c->castType<mdui::Slider>(false)) {
            const std::string& key = sl->getUserString("slider");
            if (key.empty()) continue;
            md::character::Range r; r.name = key; r.min = 0; r.max = 100000;
            sl->setScrollPosition(int(s.Value(r)));
        }
        if (auto* ms = c->castType<mdui::MultiSlider>(false)) {
            for (size_t k = 0; k < ms->getSlidersCount(); ++k) {
                mdui::Slider* sl = ms->getSlider(k);
                md::character::Range r; r.name = sl->getUserString("slider"); r.min = 0; r.max = 100;
                sl->setScrollPosition(int(s.Value(r)));
            }
        }
    }
    g.updating = false;
}

void Refresh() {
    const Session& s = *g.s;
    g.updating = true;
    SetText("RaceText", s.Race(s.race_index()).name);
    SetText("SubRaceText", s.Current().name);
    SetText("GenderText", s.SingleGender() ? "-" : (s.gender() == Gender::Female ? "Female" : "Male"));
    SetText("CharIndexText", "1/1");
    SetText("DescriptionPanelText", s.Current().description);
    std::string stats;
    for (const md::character::StatRow& row : md::character::RaceStatRows(s.Current())) {
        stats += row.label;
        if (!row.value.empty()) stats += ": " + (row.bad ? Colour("BadBright") : std::string()) + row.value + (row.bad ? Colour("Main") : std::string());
        stats += "\n";
    }
    if (!CharacterEditor::ConfirmMessage().empty()) stats = CharacterEditor::ConfirmMessage() + "\n" + stats;
    SetText("StatsPanelText", stats);
    if (MyGUI::Button* c = WT<MyGUI::Button>("ClothesButton")) c->setStateSelected(CharacterEditor::Clothes());
    g.updating = false;
    ShowTabButtons();
    const bool structure = g.built_tab != g.tab || g.built_race != s.race_index() || g.built_sub != s.subrace_index() || g.built_gender != s.gender();
    if (structure) BuildContent(); else RefreshSliders();
    g.seen_revision = s.revision();
}

// ── button handlers (the exe's handlers of docs/spec/ui/character_editor.md §2) ──
void Click(MyGUI::Widget* w) {
    Session& s = *g.s;
    const std::string& n = w->getName();
    auto step = [](size_t cur, size_t count, int d) { return (cur + count + size_t(d + int(count))) % count; };
    (void)step;
    if (n == "RaceLeftButton" || n == "RaceRightButton") { const int d = n == "RaceLeftButton" ? -1 : 1; if (s.RaceCount() > 1) s.SelectRace((s.race_index() + s.RaceCount() + size_t(d + 0)) % s.RaceCount()); }
    else if (n == "SubRaceLeftButton" || n == "SubRaceRightButton") { const int d = n == "SubRaceLeftButton" ? -1 : 1; if (s.SubraceCount() > 1) s.SelectSubrace((s.subrace_index() + s.SubraceCount() + size_t(d + 0)) % s.SubraceCount()); }
    else if (n == "GenderLeftButton" || n == "GenderRightButton") { if (!s.SingleGender()) s.SetGender(s.gender() == Gender::Female ? Gender::Male : Gender::Female); }
    else if (n == "BodyTabButton") g.tab = 0;
    else if (n == "FaceTabButton") g.tab = 1;
    else if (n == "HairTabButton") g.tab = 2;
    else if (n == "RandomiseAllButton") s.RandomiseAll();
    else if (n == "RandomiseButton") { if (g.tab == 2) s.RandomiseHair(); else s.RandomiseCategory(kCategory[g.tab]); }
    else if (n == "ResetButton") s.Reset();
    else if (n == "ImportButton") CharacterEditor::OpenIoDialog(1);
    else if (n == "ExportButton") CharacterEditor::OpenIoDialog(2);
    else if (n == "ClothesButton") CharacterEditor::Clothes() = !CharacterEditor::Clothes();
    else if (n == "ConfirmButton") CharacterEditor::ConfirmForScenario();
    if (n == "BodyTabButton" || n == "FaceTabButton" || n == "HairTabButton") CharPreviewSDLGPU::SetCameraForTab(g.tab);
    g.seen_revision = ~0ull;    // refresh next frame (tab change does not bump the session revision)
}

bool Init() {
    if (g.tried) return g.ok;
    g.tried = true;
    if (!CharacterEditor::EnsureLoaded()) return false;
    g.s = &CharacterEditor::GetSession();
    if (!mdui::MyGuiInit(1920, 1080, "Kenshi_CharacterEditor.layout")) return false;
    const char* buttons[] = {"RaceLeftButton", "RaceRightButton", "SubRaceLeftButton", "SubRaceRightButton", "GenderLeftButton", "GenderRightButton", "BodyTabButton", "FaceTabButton", "HairTabButton",
                             "RandomiseAllButton", "RandomiseButton", "ResetButton", "ImportButton", "ExportButton", "ClothesButton", "ConfirmButton"};
    for (const char* b : buttons) if (MyGUI::Widget* w = W(b)) w->eventMouseButtonClick += MyGUI::newDelegate(Click);
    // TabsPanelContent is an empty panel in the layout; the exe puts a ScrollView ("Kenshi_ScrollViewEmptyLight") into it and the slider rows into that (spec §2)
    if (MyGUI::Widget* panel = W("TabsPanelContent"))
        g.content = panel->createWidget<MyGUI::ScrollView>("Kenshi_ScrollViewEmptyLight", MyGUI::IntCoord(0, 0, panel->getWidth(), panel->getHeight()), MyGUI::Align::Stretch);
    g.root = W("Root");
    if (g.content) g.content->setVisibleHScroll(false);   // rows are as wide as the view
    g.ok = g.content != nullptr;
    if (!g.ok) fprintf(stderr, "[CharacterMyGui] TabsPanelContent is not a ScrollView in the layout (type %s): the ImGui panel is used\n", W("TabsPanelContent") ? W("TabsPanelContent")->getTypeName().c_str() : "missing");
    if (g.ok) CharPreviewSDLGPU::SetCameraForTab(g.tab);
    return g.ok;
}

} // namespace

bool Available() { return true; }
bool Visible() { return g.visible; }

bool DrawTab(float w, float h) {
    g.visible = false;
    if (!Init()) return false;
    if (SDL_Window* win = SDL_GetKeyboardFocus()) if (!SDL_TextInputActive(win)) SDL_StartTextInput(win);   // the name field and every EditBox take text
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float W_ = std::floor(w), H_ = std::floor(h);
    // centre of the window layout (spec §3a: the left column is 0.161 of the width, the right one 0.30): the preview shows through between them
    const float left_w = std::floor(W_ * 0.161f), right_w = std::floor(W_ * 0.30f);
    ImGui::SetCursorScreenPos({origin.x + left_w, origin.y});
    ImGui::BeginGroup();
    CharPreviewSDLGPU::DrawInImGui(W_ - left_w - right_w, H_);
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({W_, H_});
    CharacterEditor::DrawIoPopup();
    g.x = origin.x; g.y = origin.y; g.w = W_; g.h = H_;
    if (int(W_) != int(MyGUI::RenderManager::getInstance().getViewSize().width) || int(H_) != int(MyGUI::RenderManager::getInstance().getViewSize().height)) mdui::MyGuiResize(int(W_), int(H_));
    if (g.s->revision() != g.seen_revision) Refresh();
    const ImGuiIO& io = ImGui::GetIO();
    mdui::MyGuiMouse(io.MousePos.x - g.x, io.MousePos.y - g.y, ImGui::IsMouseDown(0));
    g.visible = true;
    return true;
}

void OnEvent(const SDL_Event& e) { if (g.visible && g.ok) mdui::MyGuiEvent(e); }

void Prepare(md::GpuCommandBufferHandle cmd) { if (g.visible && g.ok) mdui::MyGuiPrepare(cmd); }

void Draw(SDL_GPUTexture* target, md::GpuCommandBufferHandle cmd) {
    if (!(g.visible && g.ok) || !target) return;
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = target; ct.load_op = SDL_GPU_LOADOP_LOAD; ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    if (!rp) return;
    SDL_GPUViewport vp = {g.x, g.y, g.w, g.h, 0.f, 1.f};
    SDL_SetGPUViewport(rp, &vp);
    SDL_Rect sc = {int(g.x), int(g.y), int(g.w), int(g.h)};
    SDL_SetGPUScissor(rp, &sc);
    mdui::MyGuiDraw(rp, cmd);
    SDL_EndGPURenderPass(rp);
}

const char* DebugCaption(const char* name) {
    static std::string s;
    if (MyGUI::TextBox* b = WT<MyGUI::TextBox>(name)) { s = b->getCaption().asUTF8(); return s.c_str(); }
    if (MyGUI::EditBox* e = WT<MyGUI::EditBox>(name)) { s = e->getCaption().asUTF8(); return s.c_str(); }
    return nullptr;
}

bool DebugClick(const char* name) {
    MyGUI::Widget* w = W(name);
    if (!w) return false;
    Click(w);
    return true;
}

bool DebugSetSlider(const char* caption, int value) {
    if (!g.content) return false;
    for (size_t i = 0; i < g.content->getChildCount(); ++i)
        if (auto* sl = g.content->getChildAt(i)->castType<mdui::Slider>(false))
            if (sl->getUserString("slider") == caption) { sl->setScrollPosition(value); g.s->SetValue(caption, float(value)); return true; }
    return false;
}

const char* DebugFirstSlider() {
    static std::string key;
    if (!g.content) return nullptr;
    for (size_t i = 0; i < g.content->getChildCount(); ++i)
        if (auto* sl = g.content->getChildAt(i)->castType<mdui::Slider>(false))
            if (!sl->getUserString("slider").empty()) { key = sl->getUserString("slider"); return key.c_str(); }
    return nullptr;
}

void DebugDump(const char* path) { mdui::MyGuiDumpWidgets(path); }

} // namespace CharacterMyGui

#else   // built without MyGUI
namespace CharacterMyGui {
bool Available() { return false; }
bool DrawTab(float, float) { return false; }
void OnEvent(const SDL_Event&) {}
void Prepare(md::GpuCommandBufferHandle) {}
void Draw(SDL_GPUTexture*, md::GpuCommandBufferHandle) {}
bool Visible() { return false; }
const char* DebugCaption(const char*) { return nullptr; }
bool DebugClick(const char*) { return false; }
bool DebugSetSlider(const char*, int) { return false; }
const char* DebugFirstSlider() { return nullptr; }
void DebugDump(const char*) {}
}
#endif
