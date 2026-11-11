#pragma once
// lua_ui_driver_api.h — md.ui.* Lua bindings for game/src/ui_driver/ui_driver.h.
//
// Registers a NEW md.ui.* sub-namespace (LuaSystem::RegisterSubNamespaceFunction,
// same mechanism as md.editor.*/md.ecs.* from EDITOR_AUTOMATION_PLAN_v1.md
// Phase 2) — only meaningful once RegisterLuaScenarioAPI has ALSO been
// called (md.screenshot lives there), and only functional (find/click
// resolve real widgets, key_down/hold_key actually move the player) when
// built with -DMD_UI_TESTS=ON — see ui_driver.h's doc comment. Without
// MD_UI_TESTS, these functions still exist (no Lua error), they just
// always report ok=false / return false.

class LuaSystem;
void RegisterLuaUiDriverAPI(LuaSystem& sys);
