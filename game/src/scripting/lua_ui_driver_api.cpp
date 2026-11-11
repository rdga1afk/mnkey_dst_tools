#include "lua_ui_driver_api.h"
#include <monkey_dust/scripting/lua_system.h>
#include "../ui_driver/ui_driver.h"

extern "C" {
#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
}

namespace {

// md.ui.find(path) -> {ok, x, y, w, h}
int l_md_ui_find(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    UiDriverFindResult r = UiDriver_Find(path);
    lua_newtable(L);
    lua_pushboolean(L, r.ok); lua_setfield(L, -2, "ok");
    lua_pushnumber(L, r.x);   lua_setfield(L, -2, "x");
    lua_pushnumber(L, r.y);   lua_setfield(L, -2, "y");
    lua_pushnumber(L, r.w);   lua_setfield(L, -2, "w");
    lua_pushnumber(L, r.h);   lua_setfield(L, -2, "h");
    return 1;
}

// md.ui.click(path) -> ok
int l_md_ui_click(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    lua_pushboolean(L, UiDriver_Click(path));
    return 1;
}

// md.ui.key_down(name) -> ok
int l_md_ui_key_down(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, UiDriver_KeyDown(name));
    return 1;
}

// md.ui.key_up(name) -> ok
int l_md_ui_key_up(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, UiDriver_KeyUp(name));
    return 1;
}

// md.ui.hold_key(name, frames) -> ok
int l_md_ui_hold_key(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_Integer frames = luaL_checkinteger(L, 2);
    lua_pushboolean(L, UiDriver_HoldKey(name, (int)frames));
    return 1;
}

// md.ui.debug() -> string (temp diagnostic, see ui_driver.h)
int l_md_ui_debug(lua_State* L) {
    char buf[256];
    UiDriver_Debug(buf, sizeof(buf));
    lua_pushstring(L, buf);
    return 1;
}

// md.ui.select_tab(name) -> nothing. Selects an F3 tab directly
// (ImGuiTabItemFlags_SetSelected), no mouse simulation involved.
int l_md_ui_select_tab(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    UiDriver_SelectTab(name);
    return 0;
}

} // namespace

void RegisterLuaUiDriverAPI(LuaSystem& sys) {
    sys.RegisterSubNamespaceFunction("ui", "find",     l_md_ui_find);
    sys.RegisterSubNamespaceFunction("ui", "click",    l_md_ui_click);
    sys.RegisterSubNamespaceFunction("ui", "key_down", l_md_ui_key_down);
    sys.RegisterSubNamespaceFunction("ui", "key_up",   l_md_ui_key_up);
    sys.RegisterSubNamespaceFunction("ui", "hold_key", l_md_ui_hold_key);
    sys.RegisterSubNamespaceFunction("ui", "debug",    l_md_ui_debug);
    sys.RegisterSubNamespaceFunction("ui", "select_tab", l_md_ui_select_tab);
}
