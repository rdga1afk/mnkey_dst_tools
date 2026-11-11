#pragma once
// editor_world_panel.h — World Data Editor (Zones / Factions / Towns + interactive map)
// Zones: read-only, from WorldRegistry (the install). Loads factions / towns: game/data/md_world.json (private, stays in main; the panel shows nothing there without it).
// Saves: game/data/factions_override.cfg, game/data/towns.cfg

// MD_KENSHI_TMP: override at cmake time with -DMD_KENSHI_TMP=<path> for dev asset import.
// Default points to repo-relative temp dir — does NOT break runtime if absent.
#ifndef MD_KENSHI_TMP
#  define MD_KENSHI_TMP "tmp_/kenshi"
#endif

#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/world/world_registry.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include "editor_ui.h"
#include <stb_image.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

// ─── Data structures ─────────────────────────────────────────────────────────

static constexpr int WP_MAX_ZONES    = 72;   // 59 kenshi zones + buffer
static constexpr int WP_MAX_FACTIONS = 160;  // 137 kenshi factions + buffer
static constexpr int WP_MAX_TOWNS    = 384;  // 348 kenshi towns + buffer

struct WPZone {   // read-only: one kind-95 weather group of the install and the cell of its areasmap region (WorldRegistry::BuildZonesFromInstall)
    char  id[48], display_name[48], biome[48];
    int   grid_x, grid_z;
};
struct WPFaction {
    char  id[32], name[64], ideology[32], attitude[48];
    float color[3];
    bool  slaving, patrols;
};
struct WPTown {
    char  id[32], name[64], faction[32], town_type[16], biome[24];
    float x, z;   // 0-63 grid coords (loaded from grid_x/grid_z in md_world.json)
    int   pop;
};

// ─── namespace WorldPanel ────────────────────────────────────────────────────

namespace WorldPanel {

static WPZone    g_zones[WP_MAX_ZONES];   static int g_zcount = 0;
static WPFaction g_facs[WP_MAX_FACTIONS]; static int g_fcount = 0;
static WPTown    g_towns[WP_MAX_TOWNS];   static int g_tcount = 0;

static int   g_sel_type = 0;   // 0=zone 1=faction 2=town
static int   g_sel_zone = -1, g_sel_fac = -1, g_sel_town = -1;
static int   g_list_tab = 0;   // 0=Zones 1=Factions 2=Towns
static char  g_search[64]   = {};
static char  g_status[80]   = {};
static float g_status_t     = 0.f;

static GpuTexture      g_maptex_gpu;
static md::GpuTextureHandle g_maptex = nullptr;
static int    g_mapw = 0, g_maph = 0;
static bool   g_map_tried   = false;

// ─── Small parsers ────────────────────────────────────────────────────────────

static bool wp_str(const char* src, const char* key, char* out, int outlen) {
    const char* p = strstr(src, key); if (!p) return false;
    p += strlen(key);
    const char* q = strchr(p, '"'); if (!q) return false; q++;
    int i = 0; while (*q && *q != '"' && i < outlen-1) out[i++] = *q++;
    out[i] = '\0'; return true;
}
static int wp_int(const char* src, const char* key, int def) {
    const char* p = strstr(src, key); if (!p) return def;
    p += strlen(key);
    while (*p && (*p==':'||*p==' ')) p++;
    return atoi(p);
}
static bool wp_bool(const char* src, const char* key) {
    const char* p = strstr(src, key); if (!p) return false;
    p += strlen(key);
    while (*p && (*p==':'||*p==' ')) p++;
    return strncmp(p, "true", 4) == 0;
}

// ─── Load / Save ─────────────────────────────────────────────────────────────

// The zones are the game's own list, built from the install (no file of ours): nothing to save, so the panel only shows them (owner 11.11: no hand-typed zone data).
inline void LoadZones() {
    g_zcount = 0; g_sel_zone = -1;
    int n = 0; ZoneRecord* z = WorldRegistry::Get().GetAll(n);
    for (int i = 0; i < n && g_zcount < WP_MAX_ZONES; ++i) {
        WPZone& d = g_zones[g_zcount++]; memset(&d, 0, sizeof(d));
        strncpy(d.id, z[i].name, sizeof(d.id) - 1); strncpy(d.display_name, z[i].display_name, sizeof(d.display_name) - 1);
        d.grid_x = z[i].grid_x; d.grid_z = z[i].grid_z;
        strncpy(d.biome, WorldRegistry::Get().GetBiomeAt(d.grid_x, d.grid_z), sizeof(d.biome) - 1);
    }
}
// Shared buffer for md_world.json (120KB+). Static: loaded once per session.
static char  g_world_json[256*1024];
static int   g_world_json_len = 0;

inline bool EnsureWorldJson(const char* path = "game/data/md_world.json") {
    if (g_world_json_len > 0) return true;
    FILE* f = fopen(path, "rb"); if (!f) return false;
    g_world_json_len = (int)fread(g_world_json, 1, sizeof(g_world_json)-1, f);
    fclose(f); g_world_json[g_world_json_len] = '\0';
    return g_world_json_len > 0;
}

inline void LoadFactions(const char* path = "game/data/md_world.json") {
    if (!EnsureWorldJson(path)) return;
    g_fcount = 0; g_sel_fac = -1;
    const char* arr = strstr(g_world_json, "\"factions\""); if (!arr) return;
    arr = strchr(arr, '['); if (!arr) return;
    const char* cur = arr + 1;
    while (g_fcount < WP_MAX_FACTIONS) {
        const char* obj = strchr(cur, '{'); if (!obj) break;
        // find matching closing brace (skip nested arrays like color_rgb:[...])
        const char* end = strchr(obj+1, '}'); if (!end) break;
        WPFaction& fa = g_facs[g_fcount]; memset(&fa, 0, sizeof(fa));
        fa.color[0]=0.7f; fa.color[1]=0.7f; fa.color[2]=0.7f;
        wp_str(obj,"\"id\"",fa.id,32); wp_str(obj,"\"name\"",fa.name,64);
        wp_str(obj,"\"ideology\"",fa.ideology,32); wp_str(obj,"\"attitude\"",fa.attitude,48);
        fa.slaving=wp_bool(obj,"\"slaving\""); fa.patrols=wp_bool(obj,"\"patrols\"");
        const char* col = strstr(obj, "\"color_rgb\"");
        if (col && col < end) {
            const char* cb = strchr(col, '[');
            if (cb && cb < end)
                sscanf(cb+1, "%f , %f , %f", &fa.color[0], &fa.color[1], &fa.color[2]);
        }
        g_fcount++; cur = end+1;
    }
}

inline void LoadTowns(const char* path = "game/data/md_world.json") {
    if (!EnsureWorldJson(path)) return;
    g_tcount = 0; g_sel_town = -1;
    const char* arr = strstr(g_world_json, "\"towns\""); if (!arr) return;
    arr = strchr(arr, '['); if (!arr) return;
    const char* cur = arr + 1;
    while (g_tcount < WP_MAX_TOWNS) {
        const char* obj = strchr(cur, '{'); if (!obj) break;
        const char* end = strchr(obj+1, '}'); if (!end) break;
        WPTown& t = g_towns[g_tcount]; memset(&t, 0, sizeof(t));
        wp_str(obj,"\"id\"",t.id,32); wp_str(obj,"\"name\"",t.name,64);
        wp_str(obj,"\"faction\"",t.faction,32); wp_str(obj,"\"type\"",t.town_type,16);
        wp_str(obj,"\"biome\"",t.biome,24);
        // md_world.json uses grid_x/grid_z (0-63); map draw divides by 64
        t.x = (float)wp_int(obj,"\"grid_x\"",0);
        t.z = (float)wp_int(obj,"\"grid_z\"",0);
        t.pop=wp_int(obj,"\"pop\"",0);
        g_tcount++; cur = end+1;
    }
}

inline void Init() { g_world_json_len = 0; LoadZones(); LoadFactions(); LoadTowns(); }

inline bool SaveFactions(const char* path = "game/data/factions_override.cfg") {
    FILE* f = fopen(path, "w"); if (!f) return false;
    fprintf(f, "# monkey_dust factions override — edited in monkey_dust_editor\n\n");
    for (int i = 0; i < g_fcount; ++i) {
        WPFaction& fa = g_facs[i];
        fprintf(f, "faction=%s\n", fa.id);
        fprintf(f, "  name=%s\n", fa.name);
        fprintf(f, "  attitude=%s\n", fa.attitude);
        fprintf(f, "  color=%.3f,%.3f,%.3f\n", fa.color[0], fa.color[1], fa.color[2]);
        fprintf(f, "  slaving=%d\n", fa.slaving?1:0);
        fprintf(f, "  patrols=%d\n\n", fa.patrols?1:0);
    }
    fclose(f); return true;
}

inline bool SaveTowns(const char* path = "game/data/towns.cfg") {
    FILE* f = fopen(path, "w"); if (!f) return false;
    fprintf(f, "# monkey_dust towns — edited in monkey_dust_editor\n\n");
    for (int i = 0; i < g_tcount; ++i) {
        WPTown& t = g_towns[i];
        fprintf(f, "town=%s\n", t.id);
        fprintf(f, "  name=%s\n", t.name);
        fprintf(f, "  faction=%s\n", t.faction);
        fprintf(f, "  type=%s\n", t.town_type);
        fprintf(f, "  x=%.2f\n", t.x);
        fprintf(f, "  z=%.2f\n", t.z);
        fprintf(f, "  pop=%d\n\n", t.pop);
    }
    fclose(f); return true;
}

// ─── World map texture ─────────────────────────────────────────────────────

inline void LoadMapTex() {
    if (g_map_tried) return; g_map_tried = true;
    int ch;
    unsigned char* px = stbi_load("game/data/textures/world_map.png", &g_mapw, &g_maph, &ch, 4);
    if (!px) return;
    GpuSamplerDesc sd;
    sd.min_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.wrap_s = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.wrap_t = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    if (g_maptex_gpu.InitFromMemory(px, g_mapw, g_maph, sd))
        g_maptex = g_maptex_gpu.SDLTexture();
    stbi_image_free(px);
}

// ─── Helper: zone grid pos → [0,1] on map (same formula as PinScreenRect) ────

static inline ImVec2 PinNorm(int zx, int zz) {
    return { 0.05f + (zx / 63.f) * 0.90f, 0.05f + (zz / 63.f) * 0.90f };
}

// ─── Draw ─────────────────────────────────────────────────────────────────────

inline void Draw(float dt) {
    if (g_status_t > 0.f) g_status_t -= dt;

    const float LEFT_W  = 220.f;
    const float RIGHT_W = 310.f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4, 4});

    // ── Left panel ────────────────────────────────────────
    ImGui::BeginChild("##wp_left", {LEFT_W, 0.f}, ImGuiChildFlags_Borders);

    const char* tabs[] = {"Zones","Factions","Towns"};
    const int   counts[]= { g_zcount, g_fcount, g_tcount };
    for (int t = 0; t < 3; ++t) {
        if (t > 0) ImGui::SameLine();
        bool active = (g_list_tab == t);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        char lbl[32]; snprintf(lbl, sizeof(lbl), "%s (%d)", tabs[t], counts[t]);
        if (ImGui::SmallButton(lbl)) { g_list_tab = t; g_search[0]='\0'; }
        if (active) ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##wp_srch", g_search, sizeof(g_search));
    ImGui::Separator();

    if (g_list_tab == 0) {
        for (int i = 0; i < g_zcount; ++i) {
            if (g_search[0] && !strstr(g_zones[i].id, g_search) && !strstr(g_zones[i].display_name, g_search)) continue;
            char lbl[64]; snprintf(lbl, sizeof(lbl), "%s##wz%d",
                g_zones[i].display_name[0] ? g_zones[i].display_name : g_zones[i].id, i);
            bool sel = (g_sel_zone==i && g_sel_type==0);
            if (ImGui::Selectable(lbl, sel)) { g_sel_zone=i; g_sel_type=0; }
        }
    } else if (g_list_tab == 1) {
        for (int i = 0; i < g_fcount; ++i) {
            if (g_search[0] && !strstr(g_facs[i].id,g_search) && !strstr(g_facs[i].name,g_search)) continue;
            ImGui::PushStyleColor(ImGuiCol_Text, {g_facs[i].color[0],g_facs[i].color[1],g_facs[i].color[2],1.f});
            char lbl[80]; snprintf(lbl, sizeof(lbl), "%s##wf%d", g_facs[i].name, i);
            bool sel = (g_sel_fac==i && g_sel_type==1);
            if (ImGui::Selectable(lbl, sel)) { g_sel_fac=i; g_sel_type=1; }
            ImGui::PopStyleColor();
        }
    } else {
        for (int i = 0; i < g_tcount; ++i) {
            if (g_search[0] && !strstr(g_towns[i].id,g_search) && !strstr(g_towns[i].name,g_search)) continue;
            char lbl[80]; snprintf(lbl, sizeof(lbl), "%s [%s]##wt%d", g_towns[i].name, g_towns[i].faction, i);
            bool sel = (g_sel_town==i && g_sel_type==2);
            if (ImGui::Selectable(lbl, sel)) { g_sel_town=i; g_sel_type=2; }
        }
    }
    ImGui::EndChild();

    // ── Center: world map ─────────────────────────────────
    ImGui::SameLine();
    float cw = ImGui::GetContentRegionAvail().x - RIGHT_W - 4.f;
    if (cw < 100.f) cw = 100.f;
    ImGui::BeginChild("##wp_map", {cw, 0.f}, ImGuiChildFlags_Borders);

    LoadMapTex();
    ImVec2 map_o = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float  side  = (avail.x < avail.y ? avail.x : avail.y);

    if (g_maptex) {
        ImGui::Image((ImTextureID)(uintptr_t)g_maptex, {side, side});
    } else {
        ImGui::Dummy({side, side});
        ImGui::GetWindowDrawList()->AddRectFilled(map_o, {map_o.x+side, map_o.y+side}, IM_COL32(25,25,25,255));
        ImGui::GetWindowDrawList()->AddText({map_o.x+8, map_o.y+8}, IM_COL32(120,120,120,255),
            "world_map.png not found\ngame/data/textures/world_map.png");
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mpos   = ImGui::GetMousePos();
    bool   hov    = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // Town squares
    for (int i = 0; i < g_tcount; ++i) {
        WPTown& t = g_towns[i];
        if (t.x == 0.f && t.z == 0.f) continue;
        float fx = 0.05f + (t.x / 64.f) * 0.90f;
        float fz = 0.05f + (t.z / 64.f) * 0.90f;
        float px = map_o.x + fx * side, py = map_o.y + fz * side;
        bool  sel = (g_sel_type==2 && g_sel_town==i);
        float hr  = sel ? 4.f : 3.f;
        ImU32 col = sel ? IM_COL32(100,220,255,255) : IM_COL32(80,180,220,160);
        dl->AddRectFilled({px-hr,py-hr},{px+hr,py+hr}, col);
        if (sel) dl->AddText({px+hr+2,py-6}, IM_COL32(160,230,255,255), t.name);
        if (hov && fabsf(mpos.x-px)<hr+4 && fabsf(mpos.y-py)<hr+4) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", t.name);
            ImGui::TextDisabled("faction: %s", t.faction);
            ImGui::TextDisabled("biome: %s  grid: %.0f,%.0f", t.biome, t.x, t.z);
            ImGui::EndTooltip();
            if (ImGui::IsMouseClicked(0)) { g_sel_town=i; g_sel_type=2; g_list_tab=2; g_search[0]='\0'; }
        }
    }

    // Zone pins
    const char* sel_zid = (g_sel_type==0 && g_sel_zone>=0) ? g_zones[g_sel_zone].id : nullptr;
    for (int i = 0; i < g_zcount; ++i) {
        WPZone& z = g_zones[i];
        if (z.grid_x==0 && z.grid_z==0) continue;
        ImVec2 n = PinNorm(z.grid_x, z.grid_z);
        float  px = map_o.x + n.x * side, py = map_o.y + n.y * side;
        bool   sel = (sel_zid && !strcmp(z.id, sel_zid));
        float  r   = sel ? 6.f : 4.f;

        ImU32  pin_col = sel ? IM_COL32(255,220,60,255) : IM_COL32(120,200,60,210);   // one colour: the old danger tint was our own number, not Kenshi data

        dl->AddCircleFilled({px,py}, r, pin_col);
        dl->AddCircle({px,py}, r+1.f, IM_COL32(0,0,0,150), 12, 1.f);
        if (sel) {
            const char* lbl = z.display_name[0] ? z.display_name : z.id;
            dl->AddText({px+r+3, py-6}, IM_COL32(255,235,100,255), lbl);
        }

        if (hov) {
            float dx = mpos.x-px, dz = mpos.y-py;
            if (sqrtf(dx*dx+dz*dz) < r+5.f) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(z.display_name[0] ? z.display_name : z.id);
                ImGui::TextDisabled("biome: %s", z.biome);
                ImGui::EndTooltip();
                if (ImGui::IsMouseClicked(0)) {
                    g_sel_zone=i; g_sel_type=0; g_list_tab=0; g_search[0]='\0';
                }
            }
        }
    }

    ImGui::EndChild();

    // ── Right: property editor ─────────────────────────────
    ImGui::SameLine();
    ImGui::BeginChild("##wp_right", {RIGHT_W, 0.f}, ImGuiChildFlags_Borders);

    auto push_bold = [](){ if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold); };
    auto pop_bold  = [](){ if (EditorUI::font_bold) ImGui::PopFont(); };

    if (g_sel_type == 0 && g_sel_zone >= 0) {
        WPZone& z = g_zones[g_sel_zone];
        push_bold(); ImGui::Text("Zone"); pop_bold();
        ImGui::SameLine(); if (EditorUI::font_mono) ImGui::PushFont(EditorUI::font_mono);
        ImGui::TextDisabled("  %s", z.id);
        if (EditorUI::font_mono) ImGui::PopFont();
        ImGui::Separator(); ImGui::Spacing();

        ImGui::Text("Name:  %s", z.display_name);
        ImGui::Text("Biome: %s  (the BIOMES record of the install that owns the colour at the cell)", z.biome);
        ImGui::Separator();
        ImGui::Text("Cell of its areasmap region (0-63 grid): X %d  Z %d", z.grid_x, z.grid_z);
        ImGui::TextDisabled("Read-only: the list comes from the install (FCS weather groups + areasmap).");
    } else if (g_sel_type == 1 && g_sel_fac >= 0) {
        WPFaction& fa = g_facs[g_sel_fac];
        push_bold(); ImGui::Text("Faction"); pop_bold();
        ImGui::SameLine(); if (EditorUI::font_mono) ImGui::PushFont(EditorUI::font_mono);
        ImGui::TextDisabled("  %s", fa.id);
        if (EditorUI::font_mono) ImGui::PopFont();
        ImGui::Separator(); ImGui::Spacing();

        ImGui::SetNextItemWidth(-1); ImGui::InputText("Name##fn",     fa.name,     64);
        ImGui::SetNextItemWidth(-1); ImGui::InputText("Ideology##fi", fa.ideology, 32);
        ImGui::SetNextItemWidth(-1); ImGui::InputText("Attitude##fa", fa.attitude, 48);
        ImGui::Separator();

        ImGui::ColorEdit3("Map colour##fc", fa.color);
        ImGui::Checkbox("Slaving##fsl", &fa.slaving);
        ImGui::SameLine(120); ImGui::Checkbox("Patrols##fpt", &fa.patrols);
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        {0.14f,0.43f,0.22f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.20f,0.58f,0.30f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.10f,0.32f,0.16f,1.f});
        if (ImGui::Button("Save factions_override.cfg", {-1.f,0.f})) {
            if (SaveFactions()) { snprintf(g_status,sizeof(g_status),"factions_override.cfg saved!"); g_status_t=3.f; }
            else                { snprintf(g_status,sizeof(g_status),"Save FAILED.");                 g_status_t=3.f; }
        }
        ImGui::PopStyleColor(3);

    } else if (g_sel_type == 2 && g_sel_town >= 0) {
        WPTown& t = g_towns[g_sel_town];
        push_bold(); ImGui::Text("Town"); pop_bold();
        ImGui::SameLine(); if (EditorUI::font_mono) ImGui::PushFont(EditorUI::font_mono);
        ImGui::TextDisabled("  %s", t.id);
        if (EditorUI::font_mono) ImGui::PopFont();
        ImGui::Separator(); ImGui::Spacing();

        ImGui::SetNextItemWidth(-1); ImGui::InputText("Name##tn",    t.name,      64);
        ImGui::SetNextItemWidth(-1); ImGui::InputText("Faction##tf", t.faction,   32);
        ImGui::SetNextItemWidth(-1); ImGui::InputText("Type##tt",    t.town_type, 16);
        ImGui::Separator();

        ImGui::InputFloat("X (zone grid)##tx", &t.x,  0.5f, 2.f, "%.2f");
        ImGui::InputFloat("Z (zone grid)##tz", &t.z,  0.5f, 2.f, "%.2f");
        ImGui::InputInt  ("Population##tp",    &t.pop, 10,  100);
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        {0.14f,0.43f,0.22f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.20f,0.58f,0.30f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.10f,0.32f,0.16f,1.f});
        if (ImGui::Button("Save towns.cfg", {-1.f,0.f})) {
            if (SaveTowns()) { snprintf(g_status,sizeof(g_status),"towns.cfg saved!"); g_status_t=3.f; }
            else             { snprintf(g_status,sizeof(g_status),"Save FAILED.");     g_status_t=3.f; }
        }
        ImGui::PopStyleColor(3);

    } else {
        ImGui::Spacing();
        ImGui::TextDisabled("← Select a zone, faction,");
        ImGui::TextDisabled("  or town from the list,");
        ImGui::TextDisabled("  or click a map pin.");
    }

    if (g_status_t > 0.f) {
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        float alpha = (g_status_t > 1.f) ? 1.f : g_status_t;
        ImGui::TextColored({0.4f,0.9f,0.5f,alpha}, "%s", g_status);
    }

    // ── Kenshi tools ─────────────────────────────────────────
    ImGui::Spacing(); ImGui::Separator();
    ImGui::TextDisabled("Kenshi Tools");

    // Import zones/factions/towns from Kenshi .mod files via Python script
    if (ImGui::Button("Import from .mod##ki", {-1.f, 0.f})) {
        int rc = system("python3 tools/md_mod_import.py 2>&1 | tee /tmp/ken_import.log");
        if (rc == 0) {
            snprintf(g_status, sizeof(g_status), "Import OK — reloading...");
            g_status_t = 3.f;
            Init(); LoadMapTex();
        } else {
            snprintf(g_status, sizeof(g_status), "Import FAILED — see /tmp/ken_import.log");
            g_status_t = 4.f;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run tools/md_mod_import.py\n(reads Kenshi .mod files → md_world.json)");

    // Native Kenshi mod import (replaces Wine/FCS)
    if (ImGui::Button("Import Kenshi .mod/.base##kf", {-1.f, 0.f})) {
        int rc = system("python3 tools/md_mod_import.py 2>&1 | tee /tmp/ken_import.log");
        snprintf(g_status, sizeof(g_status), rc == 0 ? "Kenshi data imported." : "Import FAILED — see /tmp/ken_import.log");
        g_status_t = 3.f;
        g_map_tried = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Parse gamedata.base + Newwworld.mod + rebirth.mod\n→ md_world.json (factions/towns)");

    // Reload button at bottom
    ImGui::Separator();
    if (ImGui::Button("Reload All", {-1.f,0.f})) {
        g_map_tried = false;
        g_world_json_len = 0;  // force re-read md_world.json
        Init(); LoadMapTex();
        snprintf(g_status, sizeof(g_status), "Reloaded.");
        g_status_t = 2.f;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace WorldPanel
