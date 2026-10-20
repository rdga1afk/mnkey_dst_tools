#include <filesystem>
#include "lua_editor_scenario_api.h"
#include <monkey_dust/scripting/lua_system.h>
#include <monkey_dust/world/terrain_gen.h>
#include "editor_world_3d_sdlgpu.h"
#include "editor_screenshot.h"
#include "editor_char_preview.h"
#include "character_editor.h"
#include "character_mygui.h"
#include "character/appearance_io.h"
#include "character/session.h"
#include <cstring>
#include <cstdio>
#include <cmath>

extern "C" {
#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
}

// ── md.editor_open_panel(name) ────────────────────────────────────────────────
// Etap 4 scope: "3D World" and "Characters" force-select their tab (the
// latter needs the tab to be drawn once so the install is loaded). Other tab names
// are accepted (stored, no error) but do nothing yet; wire them the same
// way if a future scenario needs them — don't pre-wrap every panel.
static char s_forced_tab[64] = {};

// ── Character editor, for scenarios (the editor itself has no switches): the session is the editor's own, the UI only draws it.
// md.char_editor_select(subrace_name, female) -> bool   md.char_editor_set(slider, value) / md.char_editor_set(nil) = RESET; "head" and "idle stance" take a 0-based index
// md.char_editor_randomise("all"|"body"|"face"|"hair"[, seed])   the RANDOMISE buttons of the tabs; seed = the generator's start
// md.char_editor_view(tab, yaw_radians[, pitch, distance, look_at_y])   tab 0 = whole body, others = head
static int l_md_char_editor_select(lua_State* L) {
    lua_pushboolean(L, CharacterEditor::GetSession().SelectSubraceByName(luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0));
    return 1;
}
static int l_md_char_editor_set(lua_State* L) {
    const std::string key = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
    if (key.empty()) CharacterEditor::GetSession().Reset();
    else if (key == "head") CharacterEditor::GetSession().SelectHead((int)luaL_checkinteger(L, 2));
    else if (key == "hair") CharacterEditor::GetSession().SelectHair((size_t)luaL_checkinteger(L, 2));
    else if (key == "beard") CharacterEditor::GetSession().SelectBeard((size_t)luaL_checkinteger(L, 2));
    else if (key == "idle stance") CharacterEditor::GetSession().SelectStance((int)luaL_checkinteger(L, 2));
    else CharacterEditor::GetSession().SetValue(lua_tostring(L, 1), (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_md_char_editor_export(lua_State* L) {
    std::string err;
    bool ok = md::character::ExportAppearance(CharacterEditor::GetSession(), luaL_checkstring(L, 1), err);
    if (!ok) fprintf(stderr, "[CharacterEditor] export: %s\n", err.c_str());
    lua_pushboolean(L, ok);
    return 1;
}
static int l_md_char_editor_confirm(lua_State* L) {
    lua_pushboolean(L, CharacterEditor::ConfirmForScenario());
    return 1;
}
// The Kenshi window (MyGUI) of the Characters tab, as a user sees it: md.char_ui_click(widget) -> bool, md.char_ui_caption(widget) -> string|nil,
// md.char_ui_slider(slider_name, value) -> bool (moves the slider of the open tab), md.char_ui_dump(path) (widget rectangles), md.char_ui_visible()
static int l_md_char_editor_get(lua_State* L) {   // current value of a slider by name (nil when never set), "tab" is the UI tab 0..2 handled elsewhere
    const auto& v = CharacterEditor::GetSession().Values().values; auto it = v.find(luaL_checkstring(L, 1));
    if (it == v.end()) lua_pushnil(L); else lua_pushnumber(L, it->second);
    return 1;
}
static int l_md_char_editor_digest(lua_State* L) {   // FNV-1a hash of all slider values (names and values) and how many values are set
    uint64_t h = 1469598103934665603ull; int n = 0;
    for (const auto& kv : CharacterEditor::GetSession().Values().values) {
        for (char c : kv.first) { h ^= (unsigned char)c; h *= 1099511628211ull; }
        const float v = kv.second; unsigned char b[4]; memcpy(b, &v, 4);
        for (unsigned char c : b) { h ^= c; h *= 1099511628211ull; }
        ++n;
    }
    const auto& se = CharacterEditor::GetSession();   // the list choices are part of the state: head, hair, beard, idle stance
    for (size_t v : {se.head(), se.hair_index(), se.beard_index(), se.stance()}) { h ^= (uint64_t)v + 0x9e3779b97f4a7c15ull; h *= 1099511628211ull; }
    lua_pushinteger(L, (lua_Integer)(h >> 1)); lua_pushinteger(L, n);
    return 2;
}
// md.char_io(mode, path) -> ok, err: what OK of the IMPORT (1) / EXPORT (2) dialog does
static int l_md_char_ui_multislider(lua_State* L) { lua_pushboolean(L, CharacterMyGui::DebugMultiSlider((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2))); return 1; }
static int l_md_char_ui_char_index(lua_State* L) { lua_pushinteger(L, CharacterMyGui::DebugCharIndex((int)luaL_optinteger(L, 1, 0))); return 1; }
static int l_md_char_ui_slider_key(lua_State* L) { const char* k = CharacterMyGui::DebugSliderKey((int)luaL_checkinteger(L, 1)); if (k) lua_pushstring(L, k); else lua_pushnil(L); return 1; }
static int l_md_char_ui_dialog(lua_State* L) { lua_pushboolean(L, CharacterMyGui::DebugDialog(luaL_optstring(L, 1, ""))); return 1; }
// md.char_file(path, remove) -> exists: scenario Lua has no io / os
static int l_md_char_file(lua_State* L) {
    std::error_code ec; const std::filesystem::path p(luaL_checkstring(L, 1));
    if (lua_toboolean(L, 2)) std::filesystem::remove(p, ec);
    lua_pushboolean(L, std::filesystem::exists(p, ec)); return 1;
}
static int l_md_char_camera(lua_State* L) { float p, d, y; CharPreviewSDLGPU::GetCamera(p, d, y); lua_pushnumber(L, d); lua_pushnumber(L, y); lua_pushnumber(L, p); return 3; }   // md.char_camera() -> distance, look-at height, pitch
static int l_md_char_io(lua_State* L) {
    std::string err; bool ok = CharacterEditor::ApplyIo((int)luaL_checkinteger(L, 1), luaL_checkstring(L, 2), err);
    lua_pushboolean(L, ok); lua_pushstring(L, err.c_str()); return 2;
}
static int l_md_char_ui_first_slider(lua_State* L) { const char* k = CharacterMyGui::DebugFirstSlider(); if (k) lua_pushstring(L, k); else lua_pushnil(L); return 1; }
static int l_md_char_ui_list_step(lua_State* L) { lua_pushboolean(L, CharacterMyGui::DebugListStep((int)luaL_checkinteger(L, 1))); return 1; }
static int l_md_char_ui_type(lua_State* L) { const char* c = CharacterMyGui::DebugType(luaL_checkstring(L, 1), luaL_checkstring(L, 2)); if (c) lua_pushstring(L, c); else lua_pushnil(L); return 1; }
static int l_md_char_ui_click(lua_State* L) { lua_pushboolean(L, CharacterMyGui::DebugClick(luaL_checkstring(L, 1))); return 1; }
static int l_md_char_ui_caption(lua_State* L) { const char* c = CharacterMyGui::DebugCaption(luaL_checkstring(L, 1)); if (c) lua_pushstring(L, c); else lua_pushnil(L); return 1; }
static int l_md_char_ui_slider(lua_State* L) { lua_pushboolean(L, CharacterMyGui::DebugSetSlider(luaL_checkstring(L, 1), (int)luaL_checkinteger(L, 2))); return 1; }
static int l_md_char_ui_dump(lua_State* L) { CharacterMyGui::DebugDump(luaL_checkstring(L, 1)); return 0; }
static int l_md_char_ui_visible(lua_State* L) { lua_pushboolean(L, CharacterMyGui::Visible()); return 1; }
static int l_md_char_editor_import(lua_State* L) {
    std::string err;
    bool ok = md::character::ImportAppearance(CharacterEditor::GetSession(), luaL_checkstring(L, 1), err);
    if (!ok) fprintf(stderr, "[CharacterEditor] import: %s\n", err.c_str());
    lua_pushboolean(L, ok);
    return 1;
}
static int l_md_char_editor_view(lua_State* L) {
    CharacterEditor::SetTab((int)luaL_checkinteger(L, 1));
    CharPreviewSDLGPU::SetCameraForTab((int)luaL_checkinteger(L, 1));
    CharPreviewSDLGPU::SetYaw((float)luaL_optnumber(L, 2, 0.0));
    if (lua_gettop(L) >= 5) CharPreviewSDLGPU::SetCamera((float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5));
    return 0;
}

static int l_md_char_editor_randomise(lua_State* L) {
    const std::string what = luaL_checkstring(L, 1);
    auto& ses = CharacterEditor::GetSession();
    if (lua_gettop(L) >= 2) ses.Seed((uint32_t)luaL_checkinteger(L, 2));
    if (what == "all") ses.RandomiseAll();
    else if (what == "hair") ses.RandomiseHair();
    else if (what == "face") ses.RandomiseCategory("Face");
    else if (what == "body") ses.RandomiseCategory("Body");
    return 0;
}

static int l_md_editor_open_panel(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    strncpy(s_forced_tab, name, sizeof(s_forced_tab) - 1);
    return 0;
}

const char* EditorPanels_ConsumeForcedTab() {
    if (s_forced_tab[0] == '\0') return nullptr;
    static char out[64];
    strncpy(out, s_forced_tab, sizeof(out) - 1);
    s_forced_tab[0] = '\0';  // one-shot — BeginTabItem only needs SetSelected once
    return out;
}

// ── md.editor_select_zone(x, y) — jump 3D World camera to a zone centre ──────
static int l_md_editor_select_zone(lua_State* L) {
    int zx = (int)luaL_checkinteger(L, 1);
    int zz = (int)luaL_checkinteger(L, 2);
    WorldEditor3D_SDLGPU::TeleportToZone(zx, zz);
    return 0;
}

// ── md.editor_camera_pos(x, y, z, yaw, pitch) — full camera pose control ─────
// Visual-diagnostic tool (not part of the original Etap 4 "3-5 operations"
// scope) — added specifically to inspect the POM/forward shader-pass
// crossfade boundary near ground level; TeleportToZone's fixed aerial
// altitude can't get close enough to see it.
static int l_md_editor_camera_pos(lua_State* L) {
    float x     = (float)luaL_checknumber(L, 1);
    float y     = (float)luaL_checknumber(L, 2);
    float z     = (float)luaL_checknumber(L, 3);
    float yaw   = (float)luaL_optnumber(L, 4, 0.0);
    float pitch = (float)luaL_optnumber(L, 5, 0.38);
    WorldEditor3D_SDLGPU::SetCameraPos(x, y, z, yaw, pitch);
    return 0;
}

// ── md.editor_terrain_op(op, x, z) -> result ─────────────────────────────────
// Etap 4 scope: "height" is the one operation actually needed for terrain
// verification (matches game-side md.terrain_height's TerrainQuery source of
// truth, via the editor's own already-loaded TerrainAtlas — same underlying
// world_hmap data, no second implementation of the sampling transform).
// Not a general brush-edit API — see plan's own "3-5 operations really
// needed, don't wrap every panel" scope note.
static int l_md_editor_terrain_op(lua_State* L) {
    const char* op = luaL_checkstring(L, 1);
    if (strcmp(op, "height") == 0) {
        float x = (float)luaL_checknumber(L, 2);
        float z = (float)luaL_checknumber(L, 3);
        float h = TerrainAtlas_SampleWorld(x, z);
        lua_pushnumber(L, (lua_Number)h);
        return 1;
    }
    if (strcmp(op, "chunks_loaded") == 0) {
        // World3D's full 64x64=4096-chunk background build (editor_world_3d_
        // sdlgpu.cpp) — designed for ~0.5s at 60fps, observed much slower in
        // this session's environment; scenarios must wait_until this reaches
        // chunks_total before a screenshot reflects real terrain, not the
        // partial-load placeholder.
        lua_pushinteger(L, WorldEditor3D_SDLGPU::GetChunksLoaded());
        lua_pushinteger(L, WorldEditor3D_SDLGPU::GetChunksTotal());
        return 2;
    }
    return luaL_error(L, "md.editor_terrain_op: unsupported op '%s' (Etap 4 supports \"height\", \"chunks_loaded\")", op);
}

// ── md.log(msg) — same convention as game's lua_scenario_api.cpp: bare
// stdout, [SCRIPT] prefix, not MD_LOG (defaults to stderr, md_log.h). Not
// part of the shared engine bootstrap (only wait_ticks/wait_until/quit are)
// — every consumer registers its own, matching the game side exactly.
static int l_md_log(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (msg) { printf("[SCRIPT] %s\n", msg); fflush(stdout); }
    return 0;
}

// ── md.assert_eq/assert_true — same [ASSERT FAIL] convention as game's
// lua_scenario_api.cpp, needed for VERIFY's RMSE threshold check.
static int l_md_assert_eq(lua_State* L) {
    double a   = luaL_checknumber(L, 1);
    double b   = luaL_checknumber(L, 2);
    double eps = luaL_optnumber(L, 3, 0.0001);
    const char* label = luaL_optstring(L, 4, "");
    if (fabs(a - b) > eps) {
        printf("[ASSERT FAIL] %s: ОЧІКУВАНЕ=%.6f ФАКТ=%.6f (eps=%.6f)\n", label, a, b, eps);
        return luaL_error(L, "assert_eq failed: %s", label);
    }
    return 0;
}

static int l_md_assert_true(lua_State* L) {
    bool cond = lua_toboolean(L, 1);
    const char* label = luaL_optstring(L, 2, "");
    if (!cond) {
        printf("[ASSERT FAIL] %s: ОЧІКУВАНЕ=true ФАКТ=false\n", label);
        return luaL_error(L, "assert_true failed: %s", label);
    }
    return 0;
}

// ── md.editor_screenshot(path) ────────────────────────────────────────────────
// Real SDL_GPU swapchain readback (editor_screenshot.h/.cpp) — the actual
// capture happens later THIS frame in main.cpp's render block (where the
// swapchain texture is available), not here. This just records the request.
static int l_md_editor_screenshot(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    EditorScreenshot_RequestPending(path);
    return 0;
}

void RegisterLuaEditorScenarioAPI(LuaSystem& sys) {
    sys.RegisterNamespaceFunction("editor_open_panel",  l_md_editor_open_panel);
    sys.RegisterNamespaceFunction("char_editor_select", l_md_char_editor_select);
    sys.RegisterNamespaceFunction("char_editor_set", l_md_char_editor_set);
    sys.RegisterNamespaceFunction("char_editor_view", l_md_char_editor_view);
    sys.RegisterNamespaceFunction("char_editor_randomise", l_md_char_editor_randomise);
    sys.RegisterNamespaceFunction("char_editor_export", l_md_char_editor_export);
    sys.RegisterNamespaceFunction("char_editor_import", l_md_char_editor_import);
    sys.RegisterNamespaceFunction("char_editor_confirm", l_md_char_editor_confirm);
    sys.RegisterNamespaceFunction("char_editor_get", l_md_char_editor_get);
    sys.RegisterNamespaceFunction("char_editor_digest", l_md_char_editor_digest);
    sys.RegisterNamespaceFunction("char_ui_click", l_md_char_ui_click);
    sys.RegisterNamespaceFunction("char_ui_list_step", l_md_char_ui_list_step);
    sys.RegisterNamespaceFunction("char_ui_type", l_md_char_ui_type);
    sys.RegisterNamespaceFunction("char_ui_first_slider", l_md_char_ui_first_slider);
    sys.RegisterNamespaceFunction("char_ui_caption", l_md_char_ui_caption);
    sys.RegisterNamespaceFunction("char_ui_slider", l_md_char_ui_slider);
    sys.RegisterNamespaceFunction("char_io", l_md_char_io);
    sys.RegisterNamespaceFunction("char_camera", l_md_char_camera);
    sys.RegisterNamespaceFunction("char_file", l_md_char_file);
    sys.RegisterNamespaceFunction("char_ui_dialog", l_md_char_ui_dialog);
    sys.RegisterNamespaceFunction("char_ui_slider_key", l_md_char_ui_slider_key);
    sys.RegisterNamespaceFunction("char_ui_char_index", l_md_char_ui_char_index);
    sys.RegisterNamespaceFunction("char_ui_multislider", l_md_char_ui_multislider);
    sys.RegisterNamespaceFunction("char_ui_dump", l_md_char_ui_dump);
    sys.RegisterNamespaceFunction("char_ui_visible", l_md_char_ui_visible);
    sys.RegisterNamespaceFunction("editor_select_zone", l_md_editor_select_zone);
    sys.RegisterNamespaceFunction("editor_camera_pos",  l_md_editor_camera_pos);
    sys.RegisterNamespaceFunction("editor_terrain_op",  l_md_editor_terrain_op);
    sys.RegisterNamespaceFunction("editor_screenshot",  l_md_editor_screenshot);
    sys.RegisterNamespaceFunction("log",                l_md_log);
    sys.RegisterNamespaceFunction("assert_eq",          l_md_assert_eq);
    sys.RegisterNamespaceFunction("assert_true",        l_md_assert_true);
}
