#pragma once
// Character editor v2 panel: layout of Kenshi_CharacterEditor.layout (spec §3a), contents from md::character::Session (races, sliders, buttons).
// All logic lives in tools/editor/character/; this file only draws it.
namespace CharEditorV2 {
bool Enable(bool on);     // loads the install (KENSHI_DIR) once and binds the preview; false when that fails (the old editor stays)
bool Active();
void Draw();              // inside the Characters tab, in place of CharacterEditor::Draw's body
}
