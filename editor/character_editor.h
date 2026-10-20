#pragma once
// Character editor tab state: the window itself is the original Kenshi layout in character_mygui.{h,cpp}; contents from md::character::Session; the 3D preview is editor_char_preview.h.
#include "imgui.h"
#include <string>
namespace md::character { class Session; }
namespace CharacterEditor {
extern bool   g_detached;
extern ImVec2 g_win_pos, g_win_size;
bool ConfirmForScenario();                 // CONFIRM as a click; true when the .bod2 was written
md::character::Session& GetSession();
bool EnsureLoaded();                       // loads the install (KENSHI_DIR) on the first call; false when it is missing
bool ApplyIo(int mode, const char* path, std::string& err);   // what SAVE / OPEN of the file dialog does: 1 IMPORT, 2 EXPORT (also for the behaviour tests)
bool& Clothes();                           // CLOTHES state
std::string& ConfirmMessage();             // result of the last CONFIRM     // scenarios (Lua) set slider values through it
}
