#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"
#include <monkey_dust/ecs/md_registry.h>
#include "editor_toolbar.h"
#include "editor_hierarchy.h"
#ifndef MONKEY_DUST_STANDALONE_EDITOR
#include "editor_inspector.h"
#endif
#include "editor_camera_panel.h"
#include "editor_console.h"
#include "editor_animation_panel.h"
#include "editor_flare_browser.h"
#include "editor_viewcone_panel.h"
#include "editor_flowgraph_panel.h"
#include "editor_sequencer_panel.h"
#include "editor_director_panel.h"
#include "editor_gpu_profiler_panel.h"
#include "editor_node_graph.h"
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/platform/input.h>
#include <monkey_dust/platform/window.h>  // _wnd::ptr() for RelativeMouseMode
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/render/light_system.h>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static constexpr float DEG2R = 3.14159265f / 180.f;

static constexpr const char* F3_LAYOUT_PATH = "data/editor_f3_layout.json";

// Version tag written by f3_save; file without it is from a different/stale session — ignored.
static constexpr const char* F3_VERSION_TAG = "\"v\":1";

struct F3SettingsState { bool det; ImVec2 pos, siz; };

#ifndef MONKEY_DUST_STANDALONE_EDITOR
// Settings tab -> renderer (defined in game/src/render/npc_render_deferred.cpp; tools/ does not include game headers)
extern void  md_settings_set_frames_in_flight(int n);   // #152: SDL_SetGPUAllowedFramesInFlight 1 / 2 / 3 (engine default 1)
extern void  md_settings_set_fsr1(int mode, float sharp);   // #156: 0 off, 1 Quality (1.5x), 2 Balanced (1.7x), 3 Performance (2x); RCAS 0..1
extern float md_settings_get_resolution_scale();
extern void  md_settings_set_resolution_scale(float s);
extern void  md_settings_render_size(int* in_w, int* in_h, int* out_w, int* out_h);
extern bool  md_settings_get_ground_reuse();
extern void  md_settings_set_ground_reuse(bool on);
extern int   md_settings_get_lighting();   // #140: 0 ours, 2 Kenshi (L2), 3 + fog (L3), 4 + auto exposure (L4)
extern void  md_settings_set_lighting(int mode);
extern void  md_settings_set_fxaa(bool on);   // #180: FXAA 3.11 (Kenshi's parameters), default off (owner 16.10)
extern void  md_settings_set_terrain_lod(float detail, float distant, float threshold);   // #175
extern void  md_settings_set_vsync(bool on);   // #175
extern void  md_settings_set_view_distance(float v);   // #175
extern void  md_settings_set_ground_aniso(int level);   // #180
extern void  md_settings_set_window_size(int w, int h);   // #175
extern void  md_settings_set_dyn_res(float scale);   // #182: resolution while the camera moves (Native upscaling only); 0 = off
extern int   md_settings_get_terrain_chunk();
extern void  md_settings_set_terrain_chunk(int cells);
#endif
// Upscaling / Terrain settings saved with the F3 layout ("rm": 0 Native, 1 Bilinear auto, 2 Bilinear fixed; "rs": fixed scale; "tcs": terrain chunk 16 or 64).
static int   s_set_fif = 1;            // "fif": GPU frames in flight (default 1: route -1.5 ms p50 / p99, #152, docs/DECISIONS.md)
static float s_set_fsr1_sharp = 0.5f;   // "fs": the RCAS sharpness of FSR 1
static int   s_set_res_mode  = 0;
static float s_set_res_scale = 0.75f;
static int   s_set_chunk     = 16;
static bool  s_set_fxaa      = false;  // "fxaa": FXAA (#180), off by default (owner 16.10)
// Kenshi's graphics sliders (#175, docs/research/KENSHI_GRAPHICS_OPTIONS.md): names, ranges and defaults of settings.cfg
static float s_set_tdetail   = 1.f;    // "td": terrain detail 0..1, Kenshi 1.0
static float s_set_tdistant  = 0.f;    // "tdi": terrain distant 0..1, Kenshi 0.0
static float s_set_tthresh   = 750.f;  // "tth": terrain threshold, Kenshi 750
static bool  s_set_vsync     = false;  // "vsy": VSync, Kenshi (kenshi.cfg) No
static const int kAnisoLv[3] = { 1, 4, 16 };
static int   s_set_ga        = 0;      // "ga": index in kAnisoLv (ground texture filtering): 0 = footprint mip (default), 1 = aniso 4, 2 = aniso 16 (Kenshi)
static float s_set_vdist     = 5000.f; // "vd": View Distance, Kenshi 5000 (1500...12000)
static int   s_set_wres      = -1;     // "wres": index in kWinRes, -1 = the window as it is
static const int kWinRes[][2] = { {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080} };
static int   s_set_dyn       = 1;      // "dyn": resolution while moving (#182, owner 17.10): 0 off, 1 = 0.75 (default), 2 = 0.5
static float dyn_scale_of(int m) { return m == 1 ? 0.75f : (m == 2 ? 0.5f : 0.f); }
static int   s_set_light     = 2;      // "lgt": lighting (#140), Kenshi L2 by default
static bool  s_set_reuse     = true;   // "gru": ground shading reuse (#163), on by default
static void  apply_res_mode() {
#ifndef MONKEY_DUST_STANDALONE_EDITOR
    md_settings_set_fsr1(s_set_res_mode >= 3 ? s_set_res_mode - 2 : 0, s_set_fsr1_sharp);   // 3..5 = FSR 1 Quality / Balanced / Performance
    md_settings_set_resolution_scale(s_set_res_mode == 0 || s_set_res_mode >= 3 ? 1.f : s_set_res_mode == 1 ? 0.f : s_set_res_scale);
#endif
}

static float f3_parse_f(const char* buf, const char* key) {
    const char* p = strstr(buf, key); if (!p) return 0.f;
    p += strlen(key);
    while (*p == ':' || *p == ' ') ++p;
    return (float)atof(p);
}

static void f3_load(bool det[6], ImVec2 pos[6], ImVec2 siz[6],
                    float* out_fspd = nullptr, F3SettingsState* out_set = nullptr) {
    FILE* f = fopen(F3_LAYOUT_PATH, "rb");
    if (!f) return;
    char buf[1024]; size_t n = fread(buf, 1, 1023, f); buf[n] = '\0'; fclose(f);
    // Reject files not written by this code (no version tag = stale/foreign file).
    if (!strstr(buf, F3_VERSION_TAG)) return;
    const char* dk[6] = {"\"scene\"","\"ai\"","\"anim\"","\"flow\"","\"debug\"","\"cam\""};
    for (int i = 0; i < 6; ++i) {
        const char* p = strstr(buf, dk[i]); if (!p) continue;
        p += strlen(dk[i]); while (*p == ':' || *p == ' ') ++p;
        det[i] = (*p == '1');
    }
    // pos/size keys per panel; only applied when width > 0 (defaults kept on first run)
    static const char* pk[6][4] = {
        {"\"scx\"","\"scy\"","\"scw\"","\"sch\""},
        {"\"aix\"","\"aiy\"","\"aiw\"","\"aih\""},
        {"\"anx\"","\"any\"","\"anw\"","\"anh\""},
        {"\"flx\"","\"fly\"","\"flw\"","\"flh\""},
        {"\"dbx\"","\"dby\"","\"dbw\"","\"dbh\""},
        {"\"cmx\"","\"cmy\"","\"cmw\"","\"cmh\""},
    };
    for (int i = 0; i < 6; ++i) {
        float w = f3_parse_f(buf, pk[i][2]);
        if (w > 0.f) {
            pos[i] = { f3_parse_f(buf, pk[i][0]), f3_parse_f(buf, pk[i][1]) };
            siz[i] = { w, f3_parse_f(buf, pk[i][3]) };
        }
    }
    if (out_fspd) {
        float v = f3_parse_f(buf, "\"fspd\"");
        if (v > 0.f) *out_fspd = v;
    }
    if (strstr(buf, "\"rm\"")) {
        s_set_res_mode = (int)f3_parse_f(buf, "\"rm\"");
        if (s_set_res_mode < 0 || s_set_res_mode > 5) s_set_res_mode = 0;
        if (strstr(buf, "\"fs\"")) { const float fs = f3_parse_f(buf, "\"fs\""); if (fs >= 0.f && fs <= 1.f) s_set_fsr1_sharp = fs; }
        const float rs = f3_parse_f(buf, "\"rs\"");
        if (rs >= 0.5f && rs <= 1.f) s_set_res_scale = rs;
        s_set_chunk = ((int)f3_parse_f(buf, "\"tcs\"") == 64) ? 64 : 16;
        if (strstr(buf, "\"gru\"")) s_set_reuse = f3_parse_f(buf, "\"gru\"") != 0.f;
        if (strstr(buf, "\"fif\"")) { s_set_fif = (int)f3_parse_f(buf, "\"fif\""); if (s_set_fif < 1 || s_set_fif > 3) s_set_fif = 1; }
        if (strstr(buf, "\"fxaa\"")) s_set_fxaa = f3_parse_f(buf, "\"fxaa\"") != 0.f;
        if (strstr(buf, "\"td\"")) { s_set_tdetail = f3_parse_f(buf, "\"td\""); if (s_set_tdetail < 0.f || s_set_tdetail > 1.f) s_set_tdetail = 1.f; }
        if (strstr(buf, "\"tdi\"")) { s_set_tdistant = f3_parse_f(buf, "\"tdi\""); if (s_set_tdistant < 0.f || s_set_tdistant > 1.f) s_set_tdistant = 0.f; }
        if (strstr(buf, "\"tth\"")) { s_set_tthresh = f3_parse_f(buf, "\"tth\""); if (s_set_tthresh < 100.f || s_set_tthresh > 3000.f) s_set_tthresh = 750.f; }
        if (strstr(buf, "\"vsy\"")) s_set_vsync = f3_parse_f(buf, "\"vsy\"") != 0.f;
        if (strstr(buf, "\"ga\"")) { s_set_ga = (int)f3_parse_f(buf, "\"ga\""); if (s_set_ga < 0 || s_set_ga > 2) s_set_ga = 0; }
        if (strstr(buf, "\"vd\"")) { s_set_vdist = f3_parse_f(buf, "\"vd\""); if (s_set_vdist < 1500.f || s_set_vdist > 12000.f) s_set_vdist = 5000.f; }
        if (strstr(buf, "\"wres\"")) { s_set_wres = (int)f3_parse_f(buf, "\"wres\""); if (s_set_wres < 0 || s_set_wres > 3) s_set_wres = -1; }
        if (strstr(buf, "\"dyn\"")) { s_set_dyn = (int)f3_parse_f(buf, "\"dyn\""); if (s_set_dyn < 0 || s_set_dyn > 2) s_set_dyn = 1; }
        if (strstr(buf, "\"lgt\"")) { s_set_light = (int)f3_parse_f(buf, "\"lgt\""); if (s_set_light != 0 && (s_set_light < 2 || s_set_light > 4)) s_set_light = 2; }
    }
    if (out_set) {
        const char* p = strstr(buf, "\"sdet\"");
        if (p) {
            p += 6; while (*p == ':' || *p == ' ') ++p;
            out_set->det = (*p == '1');
        }
        float w = f3_parse_f(buf, "\"stw\"");
        if (w > 0.f) {
            out_set->pos = { f3_parse_f(buf, "\"stx\""), f3_parse_f(buf, "\"sty\"") };
            out_set->siz = { w, f3_parse_f(buf, "\"sth\"") };
        }
    }
}

static void f3_save(const bool det[6], const ImVec2 pos[6], const ImVec2 siz[6],
                    float fspd, F3SettingsState set) {
    FILE* f = fopen(F3_LAYOUT_PATH, "w"); if (!f) return;
    fprintf(f,
        "{\"v\":1,\"scene\":%d,\"ai\":%d,\"anim\":%d,\"flow\":%d,\"debug\":%d,\"cam\":%d,"
        "\"fspd\":%.1f,\"sdet\":%d,"
        "\"scx\":%.0f,\"scy\":%.0f,\"scw\":%.0f,\"sch\":%.0f,"
        "\"aix\":%.0f,\"aiy\":%.0f,\"aiw\":%.0f,\"aih\":%.0f,"
        "\"anx\":%.0f,\"any\":%.0f,\"anw\":%.0f,\"anh\":%.0f,"
        "\"flx\":%.0f,\"fly\":%.0f,\"flw\":%.0f,\"flh\":%.0f,"
        "\"dbx\":%.0f,\"dby\":%.0f,\"dbw\":%.0f,\"dbh\":%.0f,"
        "\"cmx\":%.0f,\"cmy\":%.0f,\"cmw\":%.0f,\"cmh\":%.0f,"
        "\"stx\":%.0f,\"sty\":%.0f,\"stw\":%.0f,\"sth\":%.0f,"
        "\"rm\":%d,\"rs\":%.2f,\"tcs\":%d,\"gru\":%d,\"lgt\":%d,\"fs\":%.2f,\"fif\":%d,\"dyn\":%d,\"fxaa\":%d,\"td\":%.2f,\"tdi\":%.2f,\"tth\":%.0f,\"vsy\":%d,\"vd\":%.0f,\"ga\":%d,\"wres\":%d}\n",
        (int)det[0],(int)det[1],(int)det[2],(int)det[3],(int)det[4],(int)det[5],
        fspd,(int)set.det,
        pos[0].x,pos[0].y,siz[0].x,siz[0].y,
        pos[1].x,pos[1].y,siz[1].x,siz[1].y,
        pos[2].x,pos[2].y,siz[2].x,siz[2].y,
        pos[3].x,pos[3].y,siz[3].x,siz[3].y,
        pos[4].x,pos[4].y,siz[4].x,siz[4].y,
        pos[5].x,pos[5].y,siz[5].x,siz[5].y,
        set.pos.x,set.pos.y,set.siz.x,set.siz.y,
        s_set_res_mode, s_set_res_scale, s_set_chunk, s_set_reuse ? 1 : 0, s_set_light, s_set_fsr1_sharp, s_set_fif, s_set_dyn,
        s_set_fxaa ? 1 : 0, s_set_tdetail, s_set_tdistant, s_set_tthresh, s_set_vsync ? 1 : 0, s_set_vdist, s_set_ga, s_set_wres);
    fclose(f);
}

void EditorCore::Init() {
    EditorConsole::Get().Init();
    EditorNodeGraphPanel::Get().Init();
    for (int i = 0; i < MAX_SELECTED; ++i)
        selected[i] = MdEntity::Null();

    bool det6[6] = {};
    ImVec2 pos6[6] = { f3_pos_scene, f3_pos_ai, f3_pos_anim, f3_pos_flow, f3_pos_debug, f3_pos_cam };
    ImVec2 siz6[6] = { f3_size_scene, f3_size_ai, f3_size_anim, f3_size_flow, f3_size_debug, f3_size_cam };
    F3SettingsState set6{f3_det_settings, f3_pos_settings, f3_size_settings};
    f3_load(det6, pos6, siz6, &cam_speed, &set6);
#ifndef MONKEY_DUST_STANDALONE_EDITOR
    // saved Settings: read at start (a run without a saved file keeps the code defaults: native 1.0, chunk 16)
    {
        FILE* sf = fopen(F3_LAYOUT_PATH, "rb");
        char sb[1024] = {}; if (sf) { size_t n = fread(sb, 1, 1023, sf); sb[n] = '\0'; fclose(sf); }
        if (strstr(sb, F3_VERSION_TAG) && strstr(sb, "\"rm\"")) { apply_res_mode(); md_settings_set_terrain_chunk(s_set_chunk); md_settings_set_ground_reuse(s_set_reuse); md_settings_set_lighting(s_set_light); md_settings_set_frames_in_flight(s_set_fif); md_settings_set_dyn_res(dyn_scale_of(s_set_dyn));
            md_settings_set_fxaa(s_set_fxaa); md_settings_set_terrain_lod(s_set_tdetail, s_set_tdistant, s_set_tthresh); md_settings_set_vsync(s_set_vsync); md_settings_set_view_distance(s_set_vdist); md_settings_set_ground_aniso(kAnisoLv[s_set_ga]);
            if (s_set_wres >= 0) md_settings_set_window_size(kWinRes[s_set_wres][0], kWinRes[s_set_wres][1]); }
        else { s_set_chunk = md_settings_get_terrain_chunk(); s_set_reuse = md_settings_get_ground_reuse(); s_set_light = md_settings_get_lighting(); }
    }
#endif
    f3_det_scene = det6[0];  f3_det_ai    = det6[1]; f3_det_anim  = det6[2];
    f3_det_flow  = det6[3];  f3_det_debug = det6[4]; f3_det_cam   = det6[5];
    f3_det_settings = set6.det;
    f3_pos_scene = pos6[0];  f3_pos_ai    = pos6[1]; f3_pos_anim  = pos6[2];
    f3_pos_flow  = pos6[3];  f3_pos_debug = pos6[4]; f3_pos_cam   = pos6[5];
    f3_pos_settings  = set6.pos;  f3_size_settings = set6.siz;
    f3_size_scene = siz6[0]; f3_size_ai   = siz6[1]; f3_size_anim = siz6[2];
    f3_size_flow  = siz6[3]; f3_size_debug = siz6[4]; f3_size_cam = siz6[5];

#ifdef MONKEY_DUST_STANDALONE_EDITOR
    // Standalone editor has its own tab-based layout.
    // Wicked-style in-game panels (Hierarchy, Inspector, Assets, etc.) start hidden;
    // main.cpp enables only the panels relevant to the standalone tool.
    for (int i = 0; i < 15; ++i) panels_visible[i] = false;
#endif

    editor_cam.pos    = { 0.f, 35.f, 35.f };
    editor_cam.target = cam_target;
    editor_cam.up     = { 0.f, 1.f, 0.f };
    editor_cam.fovy   = 60.0f;
}


void EditorCore::Update(float dt) {
    EditorToolbar::Get().Draw(dt);
#ifndef MONKEY_DUST_STANDALONE_EDITOR
    bool& g_det_scene = f3_det_scene, &g_det_ai    = f3_det_ai,    &g_det_anim  = f3_det_anim;
    bool& g_det_flow  = f3_det_flow,  &g_det_debug = f3_det_debug, &g_det_cam   = f3_det_cam;

    ImGuiIO& io = ImGui::GetIO();
    // s_overlay_top: shared with editor_toolbar.cpp (extern float).
    // Defined in main.cpp; editor_core.cpp just reads it.
    extern float s_overlay_top;
    float toolbar_h = s_overlay_top + ImGui::GetFrameHeight() + 30.f;

    // UiDriver's md.ui.select_tab(name) — selects a tab directly via
    // ImGuiTabItemFlags_SetSelected, no mouse simulation. Defined in
    // game/src/ui_driver/ui_driver.cpp (game-only, hence forward-declared
    // here rather than #included — this file also compiles into the
    // standalone editor, which has no ui_driver.h on its include path).
    extern const char* UiDriver_ConsumeForcedTab();
    const char* forced_tab = UiDriver_ConsumeForcedTab();
    auto tab_flags = [&](const char* name) -> ImGuiTabItemFlags {
        bool m = forced_tab && strcmp(forced_tab, name) == 0;
        return m ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };

    // If the active tab's panel is floating, make ##f3editor transparent so the
    // game is visible behind the detached panel (one-frame lag is imperceptible).
    static int  s_f3_active_tab = 0;
    active_tab = s_f3_active_tab;
    ImGuiWindowFlags f3_flags =
        ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
    if (s_f3_active_tab == 5 && g_det_cam) f3_flags |= ImGuiWindowFlags_NoBackground;
    if (f3_passthrough) f3_flags |= ImGuiWindowFlags_NoMouseInputs;

    ImGui::SetNextWindowPos({0.f, toolbar_h});
    ImGui::SetNextWindowSize({io.DisplaySize.x, io.DisplaySize.y - toolbar_h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
    ImGui::Begin("##f3editor", nullptr, f3_flags);
    ImGui::PopStyleVar();

    // min_y: floating title bars must not overlap the tab bar strip.
    const float min_y = toolbar_h + ImGui::GetFrameHeight() + 4.f;
    static constexpr ImGuiWindowFlags FLOAT_FLAGS =
        ImGuiWindowFlags_NoSavedSettings;

    // Helper: track pos/size after every frame and clamp y so the title bar
    // never sits under the tab bar.
    auto f3_end = [&](ImVec2& pos, ImVec2& sz) {
        pos = ImGui::GetWindowPos();
        if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
        sz = ImGui::GetWindowSize();
        ImGui::End();
    };

    // ALL tabs stay in the tab bar regardless of detach state.
    // A detached panel's floating window is created INSIDE its BeginTabItem scope —
    // so it only exists (and is visible) while that tab is active.  Switching to
    // another tab makes ImGui hide the floating window automatically. A floating
    // panel's native title bar (collapse box + close ×, once NoCollapse is off)
    // already docks it back — no custom "Dock" button needed.
    if (ImGui::BeginTabBar("##f3tabs")) {
        if (ImGui::BeginTabItem("Scene", nullptr, tab_flags("Scene"))) { s_f3_active_tab = 0;
            if (!g_det_scene) {
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3h", {300.f, av.y}, false);
                EditorHierarchy::Get().DrawContent();
                ImGui::EndChild();
                ImGui::SameLine(0, 4);
#ifndef MONKEY_DUST_STANDALONE_EDITOR
                ImGui::BeginChild("##f3i", {0.f, av.y}, false);
                EditorInspector::Get().DrawContent();
                ImGui::EndChild();
#endif
            } else {
                if (f3_pos_scene.y < min_y) f3_pos_scene.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_scene, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_scene, ImGuiCond_Appearing);
                if (ImGui::Begin("Scene##float", &g_det_scene, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    ImVec2 av = ImGui::GetContentRegionAvail();
                    ImGui::BeginChild("##fh", {av.x * 0.30f, av.y}, false);
                    EditorHierarchy::Get().DrawContent();
                    ImGui::EndChild();
#ifndef MONKEY_DUST_STANDALONE_EDITOR
                    ImGui::SameLine(0, 4);
                    ImGui::BeginChild("##fi", {0.f, av.y}, false);
                    EditorInspector::Get().DrawContent();
                    ImGui::EndChild();
#endif
                }
                f3_end(f3_pos_scene, f3_size_scene);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AI", nullptr, tab_flags("AI"))) { s_f3_active_tab = 1;
            if (!g_det_ai) {
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3dir", {av.x * 0.50f, av.y}, false);
                EditorDirectorPanel::Get().DrawContent();
                ImGui::EndChild();
                ImGui::SameLine(0, 4);
                ImGui::BeginChild("##f3vc", {0.f, av.y}, false);
                EditorViewConePanel::Get().DrawContent();
                ImGui::EndChild();
            } else {
                if (f3_pos_ai.y < min_y) f3_pos_ai.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_ai, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_ai, ImGuiCond_Appearing);
                if (ImGui::Begin("AI##float", &g_det_ai, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    ImVec2 av = ImGui::GetContentRegionAvail();
                    ImGui::BeginChild("##fdir", {av.x * 0.50f, av.y}, false);
                    EditorDirectorPanel::Get().DrawContent();
                    ImGui::EndChild();
                    ImGui::SameLine(0, 4);
                    ImGui::BeginChild("##fvc", {0.f, av.y}, false);
                    EditorViewConePanel::Get().DrawContent();
                    ImGui::EndChild();
                }
                f3_end(f3_pos_ai, f3_size_ai);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Animation", nullptr, tab_flags("Animation"))) { s_f3_active_tab = 2;
            if (!g_det_anim) {
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3an", {av.x, 180.f}, false);
                EditorAnimationPanel::Get().DrawContent();
                ImGui::EndChild();
                ImGui::BeginChild("##f3sq", {av.x, 0.f}, false);
                EditorSequencerPanel::Get().DrawContent();
                ImGui::EndChild();
            } else {
                if (f3_pos_anim.y < min_y) f3_pos_anim.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_anim, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_anim, ImGuiCond_Appearing);
                if (ImGui::Begin("Animation##float", &g_det_anim, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    ImVec2 av = ImGui::GetContentRegionAvail();
                    ImGui::BeginChild("##fan", {av.x, 180.f}, false);
                    EditorAnimationPanel::Get().DrawContent();
                    ImGui::EndChild();
                    ImGui::BeginChild("##fsq", {av.x, 0.f}, false);
                    EditorSequencerPanel::Get().DrawContent();
                    ImGui::EndChild();
                }
                f3_end(f3_pos_anim, f3_size_anim);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("FlowGraph", nullptr, tab_flags("FlowGraph"))) { s_f3_active_tab = 3;
            if (!g_det_flow) {
                ImGui::Separator();
                EditorFlowGraphPanel::Get().DrawContent();
            } else {
                if (f3_pos_flow.y < min_y) f3_pos_flow.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_flow, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_flow, ImGuiCond_Appearing);
                if (ImGui::Begin("FlowGraph##float", &g_det_flow, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    EditorFlowGraphPanel::Get().DrawContent();
                }
                f3_end(f3_pos_flow, f3_size_flow);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug", nullptr, tab_flags("Debug"))) { s_f3_active_tab = 4;
            if (!g_det_debug) {
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3con", {av.x * 0.60f, av.y}, false);
                EditorConsole::Get().DrawContent();
                ImGui::EndChild();
                ImGui::SameLine(0, 4);
                ImGui::BeginChild("##f3gpu", {0.f, av.y}, false);
                EditorGpuProfilerPanel::Get().DrawContent();
                ImGui::EndChild();
            } else {
                if (f3_pos_debug.y < min_y) f3_pos_debug.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_debug, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_debug, ImGuiCond_Appearing);
                if (ImGui::Begin("Debug##float", &g_det_debug, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    ImVec2 av = ImGui::GetContentRegionAvail();
                    ImGui::BeginChild("##fcon", {av.x * 0.60f, av.y}, false);
                    EditorConsole::Get().DrawContent();
                    ImGui::EndChild();
                    ImGui::SameLine(0, 4);
                    ImGui::BeginChild("##fgpu", {0.f, av.y}, false);
                    EditorGpuProfilerPanel::Get().DrawContent();
                    ImGui::EndChild();
                }
                f3_end(f3_pos_debug, f3_size_debug);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Camera", nullptr, tab_flags("Camera"))) { s_f3_active_tab = 5;
            if (!g_det_cam) {
                ImGui::Separator();
                EditorCameraPanel::Get().DrawContent();
            } else {
                if (f3_pos_cam.y < min_y) f3_pos_cam.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_cam, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_cam, ImGuiCond_Appearing);
                if (ImGui::Begin("Camera##float", &g_det_cam, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    EditorCameraPanel::Get().DrawContent();
                }
                f3_end(f3_pos_cam, f3_size_cam);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings", nullptr, tab_flags("Settings"))) { s_f3_active_tab = 6;
            bool&   g_det_set  = f3_det_settings;
            ImVec2& f3_pos_set = f3_pos_settings;
            ImVec2& f3_size_set = f3_size_settings;
            static char   s_st_msg[64] = {};
            static float  s_st_t = 0.f;
            if (s_st_t > 0.f) s_st_t -= io.DeltaTime;
            auto draw_settings = [&]() {
                ImGui::TextUnformatted("F3 Flythrough Camera");
                ImGui::Separator();
                ImGui::SetNextItemWidth(220.f);
                ImGui::SliderFloat("Fly speed (m/s)##fset", &cam_speed, 5.f, 5000.f, "%.0f",
                                   ImGuiSliderFlags_Logarithmic);
#ifndef MONKEY_DUST_STANDALONE_EDITOR
                // #114: internal 3D resolution. Native = 1.0; Bilinear auto = the old window-size rule (720p in a 1080p window);
                // Bilinear fixed = the slider; 3..5 = FSR 1 Quality / Balanced / Performance (#156).
                ImGui::Separator();
                ImGui::TextUnformatted("Upscaling");
                {
                    // radio buttons, not a combo: every choice is one click (and addressable by the UI driver)
                    if (ImGui::RadioButton("Native (1.0)##upsc0", s_set_res_mode == 0)) { s_set_res_mode = 0; apply_res_mode(); }
                    if (ImGui::RadioButton("Bilinear (auto: 720p in a 1080p window)##upsc1", s_set_res_mode == 1)) { s_set_res_mode = 1; apply_res_mode(); }
                    if (ImGui::RadioButton("Bilinear (fixed)##upsc2", s_set_res_mode == 2)) { s_set_res_mode = 2; apply_res_mode(); }
                    if (s_set_res_mode == 2) {
                        ImGui::SetNextItemWidth(220.f);
                        if (ImGui::SliderFloat("resolution_scale##upsc", &s_set_res_scale, 0.5f, 1.0f, "%.2f")) apply_res_mode();
                    }
                    // #156: FSR 1 (EASU + RCAS): the scene at 1 / ratio, no jitter and no vectors (FSR 2 costs 26 ms on this GPU, see docs/FAILED_APPROACHES.md)
                    if (ImGui::RadioButton("FSR 1 Quality (1.5x)##upsc3", s_set_res_mode == 3)) { s_set_res_mode = 3; apply_res_mode(); }
                    if (ImGui::RadioButton("FSR 1 Balanced (1.7x)##upsc4", s_set_res_mode == 4)) { s_set_res_mode = 4; apply_res_mode(); }
                    if (ImGui::RadioButton("FSR 1 Performance (2.0x)##upsc5", s_set_res_mode == 5)) { s_set_res_mode = 5; apply_res_mode(); }
                    if (s_set_res_mode >= 3) {
                        ImGui::SetNextItemWidth(220.f);
                        if (ImGui::SliderFloat("Sharpness (RCAS)##upscs", &s_set_fsr1_sharp, 0.0f, 1.0f, "%.2f")) apply_res_mode();
                    }
                    // #182: while the camera moves the scene renders at a lower scale, native when it stands (Native upscaling only)
                    ImGui::TextUnformatted("Resolution while moving");
                    if (ImGui::RadioButton("Off##dyn0", s_set_dyn == 0)) { s_set_dyn = 0; md_settings_set_dyn_res(dyn_scale_of(s_set_dyn)); }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("0.75##dyn1", s_set_dyn == 1)) { s_set_dyn = 1; md_settings_set_dyn_res(dyn_scale_of(s_set_dyn)); }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("0.5##dyn2", s_set_dyn == 2)) { s_set_dyn = 2; md_settings_set_dyn_res(dyn_scale_of(s_set_dyn)); }
                    int iw = 0, ih = 0, ow = 0, oh = 0;
                    md_settings_render_size(&iw, &ih, &ow, &oh);
                    ImGui::Text("internal %dx%d -> output %dx%d", iw, ih, ow, oh);
                }
                // #175: Kenshi's graphics options as separate sliders (no presets); names, ranges and defaults of settings.cfg
                ImGui::Separator();
                ImGui::TextUnformatted("Graphics");
                {
                    bool lod_ch = false;
                    ImGui::SetNextItemWidth(220.f);
                    lod_ch |= ImGui::SliderFloat("Terrain Detail##tdet", &s_set_tdetail, 0.f, 1.f, "%.2f");
                    ImGui::SetNextItemWidth(220.f);
                    lod_ch |= ImGui::SliderFloat("Terrain Distant##tdis", &s_set_tdistant, 0.f, 1.f, "%.2f");
                    ImGui::SetNextItemWidth(220.f);
                    lod_ch |= ImGui::SliderFloat("Terrain Threshold##tthr", &s_set_tthresh, 100.f, 3000.f, "%.0f");
                    if (lod_ch) md_settings_set_terrain_lod(s_set_tdetail, s_set_tdistant, s_set_tthresh);
                    if (ImGui::Checkbox("FXAA (+1.7 ms)##fxaa", &s_set_fxaa)) md_settings_set_fxaa(s_set_fxaa);
                    {
                        static const char* kAnisoNm[] = { "Footprint mip", "Aniso x4", "Aniso x16 (Kenshi)" };
                        int gi = s_set_ga;
                        ImGui::SetNextItemWidth(220.f);
                        if (ImGui::Combo("Ground filtering##gaf", &gi, kAnisoNm, 3)) { s_set_ga = gi; md_settings_set_ground_aniso(kAnisoLv[gi]); }
                    }
                    ImGui::SetNextItemWidth(220.f);
                    if (ImGui::SliderFloat("View Distance##vdist", &s_set_vdist, 1500.f, 12000.f, "%.0f")) md_settings_set_view_distance(s_set_vdist);
                    if (ImGui::Checkbox("VSync##vsy", &s_set_vsync)) md_settings_set_vsync(s_set_vsync);
                    {
                        static const char* kRes[] = { "1280x720", "1366x768", "1600x900", "1920x1080" };
                        int ri = s_set_wres;
                        ImGui::SetNextItemWidth(220.f);
                        if (ImGui::Combo("Window resolution##wres", &ri, kRes, 4)) { s_set_wres = ri; md_settings_set_window_size(kWinRes[ri][0], kWinRes[ri][1]); }
                    }
                    ImGui::TextDisabled("Shadows: off (Forward tier has no shadow pass). Texture Quality, Water Reflection, grass / foliage ranges: not implemented yet");
                }
                // #154: patch size of the terrain LOD (Kenshi's "Terrain Chunk Size"). Read at start.
                ImGui::Separator();
                ImGui::TextUnformatted("Terrain");
                {
                    if (ImGui::RadioButton("Small (16)##tcs16", s_set_chunk == 16)) s_set_chunk = 16;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Large (64)##tcs64", s_set_chunk == 64)) s_set_chunk = 64;
                    ImGui::SameLine();
                    ImGui::TextUnformatted("Terrain Chunk Size");
                    ImGui::TextDisabled("applies after a restart; keep it with Save config");
                    if (ImGui::Checkbox("Ground reuse (faster, 1 frame history)##gru", &s_set_reuse)) md_settings_set_ground_reuse(s_set_reuse);
                    {
                        static const char* kFif[] = { "1 (lowest latency)", "2 (default)", "3" };
                        int fi = s_set_fif - 1;
                        if (ImGui::Combo("GPU frames in flight##fif", &fi, kFif, 3)) { s_set_fif = fi + 1; md_settings_set_frames_in_flight(s_set_fif); }
                    }
                    {
                        static const char* kLight[] = { "Ours", "Kenshi (L2)", "+ fog (L3)", "+ auto exposure (L4)" };
                        static const int   kMode[]  = { 0, 2, 3, 4 };
                        int li = 1; for (int k = 0; k < 4; ++k) if (kMode[k] == s_set_light) li = k;
                        if (ImGui::Combo("Lighting##lgt", &li, kLight, 4)) { s_set_light = kMode[li]; md_settings_set_lighting(s_set_light); }
                    }
                }
#endif
                // 2026-09-20 (docs/RESOLVE_OPT.md session): time-of-day
                // override, requested after the ambient-probe/live-sun fix
                // made it hard to verify lighting at a chosen hour without
                // waiting real minutes (game_time_hours = now_s/60 -- see
                // logic_tick_orchestration.cpp). LightSystem::Get() (not a
                // new global) so both this UI and the consumer reach the
                // same flag through the singleton both already use.
                ImGui::Separator();
                ImGui::TextUnformatted("Time of Day");
                bool timeOv = LightSystem::Get().time_override_enabled;
                if (ImGui::Checkbox("Override##time_ov", &timeOv))
                    LightSystem::Get().time_override_enabled = timeOv;
                if (LightSystem::Get().time_override_enabled) {
                    ImGui::SetNextItemWidth(220.f);
                    float h = LightSystem::Get().time_override_hours;
                    // %02d:%02d display -- game_time_hours is fractional
                    // (game_time_hours=12.5 == 12:30), same convention
                    // LightSystem::Tick already uses for day_frac.
                    int hh = (int)h, mm = (int)((h - (float)hh) * 60.f);
                    if (ImGui::SliderFloat("Hour##time_ov_h", &h, 0.f, 24.f,
                                            (std::string(std::to_string(hh)) + ":" +
                                             (mm < 10 ? "0" : "") + std::to_string(mm)).c_str()))
                        LightSystem::Get().time_override_hours = fmodf(h, 24.f);
                }
                if (ImGui::Button("Save config##fset")) {
                    bool det[6]   = {f3_det_scene, f3_det_ai, f3_det_anim, f3_det_flow, f3_det_debug, f3_det_cam};
                    ImVec2 pos[6] = {f3_pos_scene, f3_pos_ai, f3_pos_anim, f3_pos_flow, f3_pos_debug, f3_pos_cam};
                    ImVec2 siz[6] = {f3_size_scene, f3_size_ai, f3_size_anim, f3_size_flow, f3_size_debug, f3_size_cam};
                    F3SettingsState ss{f3_det_settings, f3_pos_settings, f3_size_settings};
                    f3_save(det, pos, siz, cam_speed, ss);
                    snprintf(s_st_msg, sizeof(s_st_msg), "Saved");
                    s_st_t = 2.f;
                }
                if (s_st_t > 0.f) {
                    ImGui::SameLine();
                    ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.f}, "%s", s_st_msg);
                }
            };
            if (!g_det_set) {
                ImGui::Separator();
                draw_settings();
            } else {
                if (f3_pos_set.y < min_y) f3_pos_set.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_set,  ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_set, ImGuiCond_Appearing);
                if (ImGui::Begin("Settings##float", &g_det_set, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    draw_settings();
                }
                f3_end(f3_pos_set, f3_size_set);
            }
            ImGui::EndTabItem();
        }
        // Trailing "Detach" rendered directly in the tab bar's own row
        // (ImGuiTabItemFlags_Trailing sorts it to the end regardless of call
        // order) instead of a per-panel button inside the tab body — avoids
        // an extra blank content row above each panel. Only shown for the
        // currently active tab while it's docked.
        bool* active_det = nullptr;
        switch (s_f3_active_tab) {
            case 0: active_det = &g_det_scene;  break;
            case 1: active_det = &g_det_ai;     break;
            case 2: active_det = &g_det_anim;   break;
            case 3: active_det = &g_det_flow;   break;
            case 4: active_det = &g_det_debug;  break;
            case 5: active_det = &g_det_cam;    break;
            case 6: active_det = &f3_det_settings; break;
            default: break;
        }
        if (active_det && !*active_det) {
            if (ImGui::TabItemButton("Detach", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
                *active_det = true;
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
#endif
}

void EditorCore::Shutdown() {
    bool det6[6]  = { f3_det_scene, f3_det_ai, f3_det_anim, f3_det_flow, f3_det_debug, f3_det_cam };
    ImVec2 pos6[6] = { f3_pos_scene, f3_pos_ai, f3_pos_anim, f3_pos_flow, f3_pos_debug, f3_pos_cam };
    ImVec2 siz6[6] = { f3_size_scene, f3_size_ai, f3_size_anim, f3_size_flow, f3_size_debug, f3_size_cam };
    F3SettingsState set6{f3_det_settings, f3_pos_settings, f3_size_settings};
    f3_save(det6, pos6, siz6, cam_speed, set6);
    EditorNodeGraphPanel::Get().Shutdown();
    EditorFlowGraphPanel::Get().Shutdown();
    DeselectAll();
}

// ── Selection ─────────────────────────────────────────────────
MdEntity EditorCore::GetPrimary() const {
    return (selected_count > 0) ? selected[0] : MdEntity::Null();
}

void EditorCore::Select(MdEntity e, bool add) {
    if (!add) DeselectAll();
    if (IsSelected(e)) return;
    if (selected_count >= MAX_SELECTED) return;
    selected[selected_count++] = e;
}

void EditorCore::Deselect(MdEntity e) {
    for (int i = 0; i < selected_count; ++i) {
        if (selected[i] == e) {
            selected[i] = selected[--selected_count];
            selected[selected_count] = MdEntity::Null();
            return;
        }
    }
}

void EditorCore::DeselectAll() {
    for (int i = 0; i < selected_count; ++i) selected[i] = MdEntity::Null();
    selected_count = 0;
}

bool EditorCore::IsSelected(MdEntity e) const {
    for (int i = 0; i < selected_count; ++i)
        if (selected[i] == e) return true;
    return false;
}

// ── Editor Camera ─────────────────────────────────────────────
void EditorCore::UpdateEditorCamera(float dt, bool viewport_hovered) {
    (void)viewport_hovered;

    ImGuiIO& io = ImGui::GetIO();

    // Relative mouse mode for unlimited 360° rotation on RMB (no window-edge clamping).
    bool rmb_down = io.MouseDown[ImGuiMouseButton_Right];
    {
        static bool s_rel = false;
        if (rmb_down != s_rel) {
            SDL_SetWindowRelativeMouseMode(_wnd::ptr(), rmb_down);
            s_rel = rmb_down;
        }
    }
    // SDL raw delta — unlimited, not clamped to window edges.
    float rdx = 0.f, rdy = 0.f;
    SDL_GetRelativeMouseState(&rdx, &rdy);
    if (!rmb_down) { rdx = 0.f; rdy = 0.f; }

    if (cam_flying) {
        // ── Exact copy of editor_world_3d_sdlgpu handle_input() ──────────────
        // RMB = look; fly_yaw/fly_pitch in RADIANS (same as 3D World s_yaw/s_pitch)
        if (rmb_down) {
            fly_yaw   -= rdx * 0.003f;
            fly_pitch += rdy * 0.002f;
            if (fly_pitch < -0.3f) fly_pitch = -0.3f;
            if (fly_pitch >  1.3f) fly_pitch =  1.3f;
        }
        float sy = sinf(fly_yaw), cy2 = cosf(fly_yaw);
        const bool* kb = (const bool*)SDL_GetKeyboardState(nullptr);
        bool shift = kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT];
        float sp = cam_speed * dt;
        if (kb[SDL_SCANCODE_W]||kb[SDL_SCANCODE_UP])       { cam_target.x+=sp*sy; cam_target.z+=sp*cy2; }
        if (kb[SDL_SCANCODE_S]||kb[SDL_SCANCODE_DOWN])     { cam_target.x-=sp*sy; cam_target.z-=sp*cy2; }
        if (kb[SDL_SCANCODE_A])                            { cam_target.x+=sp*cy2; cam_target.z-=sp*sy; }
        if (kb[SDL_SCANCODE_D])                            { cam_target.x-=sp*cy2; cam_target.z+=sp*sy; }
        if (kb[SDL_SCANCODE_Q]||kb[SDL_SCANCODE_PAGEDOWN]) cam_target.y -= sp;
        if (kb[SDL_SCANCODE_E]||kb[SDL_SCANCODE_PAGEUP])   cam_target.y += sp;
        // Use input_get_scroll_y() — io.MouseWheel is 0 here because
        // UpdateEditorCamera() runs BEFORE imgui_new_frame() in the game loop.
        float wheel = input_get_scroll_y();
        if (wheel == 0.f) wheel = io.MouseWheel; // fallback for standalone editor
        if (wheel != 0.f) {
            if (shift) {
                cam_speed = (wheel > 0)
                    ? fminf(cam_speed * 1.25f, 80000.f)
                    : fmaxf(cam_speed * 0.80f, 10.f);
            } else {
                float step = cam_target.y * 0.03f * wheel;
                cam_target.x += step * sy; cam_target.z += step * cy2;
                cam_target.y = (wheel > 0)
                    ? fmaxf(cam_target.y * 0.94f, 1.f)
                    : fminf(cam_target.y * 1.06f, 150000.f);
            }
        }
        if (TerrainAtlas_Loaded()) {
            float th = TerrainAtlas_SampleWorld(cam_target.x, cam_target.z);
            if (cam_target.y < th + 1.5f) cam_target.y = th + 1.5f;
        }
        float cp = cosf(fly_pitch), sp2 = sinf(fly_pitch);
        editor_cam.pos    = cam_target;
        editor_cam.target = { cam_target.x + sy*cp,
                              cam_target.y - sp2,
                              cam_target.z + cy2*cp };
        editor_cam.up     = { 0.f, 1.f, 0.f };
        return;
    } else {
        // Orbit mode — unlimited yaw (360°), pitch clamped near-vertical.
        if (rmb_down) {
            cam_yaw   -= rdx * 0.4f;
            cam_pitch  += rdy * 0.4f;
            if (cam_pitch >  89.f) cam_pitch =  89.f;
            if (cam_pitch < -89.f) cam_pitch = -89.f;
        }
        // MMB pan
        if (io.MouseDown[ImGuiMouseButton_Middle]) {
            float yaw_r = cam_yaw * DEG2R;
            Vec3 right = { cosf(yaw_r), 0.f, -sinf(yaw_r) };
            Vec3 up    = { 0.f, 1.f, 0.f };
            float pan = cam_dist * 0.002f;
            cam_target = vec3_sub(cam_target, vec3_scale(right, io.MouseDelta.x * pan));
            cam_target = vec3_add(cam_target, vec3_scale(up,    io.MouseDelta.y * pan));
        }
        // Scroll zoom
        cam_dist -= io.MouseWheel * cam_dist * 0.1f;
        if (cam_dist < 1.f)   cam_dist = 1.f;
        if (cam_dist > 500.f) cam_dist = 500.f;
    }

    float yaw_r   = cam_yaw   * DEG2R;
    float pitch_r = cam_pitch * DEG2R;
    editor_cam.pos = {
        cam_target.x + cam_dist * cosf(pitch_r) * sinf(yaw_r),
        cam_target.y + cam_dist * sinf(pitch_r),
        cam_target.z + cam_dist * cosf(pitch_r) * cosf(yaw_r)
    };
    // Terrain floor clamp — prevent eye clipping through surface in both
    // Flythrough (cam_target moves via WASD) and Orbit (cam_dist shrinks) modes.
    if (TerrainAtlas_Loaded()) {
        static constexpr float MIN_ABOVE_TERRAIN = 1.5f;
        float th = TerrainAtlas_SampleWorld(editor_cam.pos.x, editor_cam.pos.z);
        if (editor_cam.pos.y < th + MIN_ABOVE_TERRAIN) {
            float dy = (th + MIN_ABOVE_TERRAIN) - editor_cam.pos.y;
            editor_cam.pos.y = th + MIN_ABOVE_TERRAIN;
            cam_target.y    += dy;  // shift target with eye so look direction is preserved
        }
    }
    editor_cam.target = cam_target;
    editor_cam.up     = { 0.f, 1.f, 0.f };
}

void EditorCore::FocusOnSelected() {
    if (selected_count == 0) return;
    auto& reg = MdRegistry::Get();
    MdEntity e = GetPrimary();
    if (!reg.Valid(e)) return;
    if (!(reg.Handle(e).has<WorldTransform>())) return;
    const auto& tr = reg.Handle(e).get_mut<WorldTransform>();
    cam_target = Vec3{ tr.x, 0.f, tr.z };
    cam_dist   = 15.f;
}

// ── History ───────────────────────────────────────────────────
void EditorCore::Undo() {
    history.Undo(MdRegistry::Get());
}

void EditorCore::Redo() {
    history.Redo(MdRegistry::Get());
}
#endif
