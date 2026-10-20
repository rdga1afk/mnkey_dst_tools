#pragma once
// Character editor tab: layout of Kenshi_CharacterEditor.layout (spec §3a), contents from md::character::Session (races, sliders, buttons); the 3D preview is editor_char_preview.h.
// All logic lives in tools/editor/character/; this panel only draws it.
#include "imgui.h"
#include <string>
namespace md::character { class Session; }
namespace CharacterEditor {
extern bool   g_detached;
extern ImVec2 g_win_pos, g_win_size;
void Draw(bool kenshi_theme = true);
bool ConfirmForScenario();                 // CONFIRM as a click; true when the .bod2 was written
void SetTab(int tab);                      // scenarios: BODY 0 / FACE 1 / HAIR 2, as a click on the tab
md::character::Session& GetSession();
bool EnsureLoaded();                       // loads the install (KENSHI_DIR) on the first call; false when it is missing
void OpenIoDialog(int mode);               // 1 IMPORT, 2 EXPORT: opens the path dialog; DrawIoPopup() must run every frame inside an ImGui window
void DrawIoPopup();
bool ApplyIo(int mode, const char* path, std::string& err);   // what OK of the dialog does: 1 IMPORT, 2 EXPORT (also for the behaviour tests)
bool& Clothes();                           // CLOTHES state
std::string& ConfirmMessage();             // result of the last CONFIRM     // scenarios (Lua) set slider values through it
}
