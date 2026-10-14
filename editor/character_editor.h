#pragma once
// Character editor tab: layout of Kenshi_CharacterEditor.layout (spec §3a), contents from md::character::Session (races, sliders, buttons); the 3D preview is editor_char_preview.h.
// All logic lives in tools/editor/character/; this panel only draws it.
#include "imgui.h"
namespace md::character { class Session; }
namespace CharacterEditor {
extern bool   g_detached;
extern ImVec2 g_win_pos, g_win_size;
void Draw(bool kenshi_theme = true);
md::character::Session& GetSession();     // scenarios (Lua) set slider values through it
}
