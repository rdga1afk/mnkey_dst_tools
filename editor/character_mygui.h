#pragma once
// Characters tab on the original Kenshi window (MyGUI, Kenshi_CharacterEditor.layout from the install), bound to md::character::Session (all logic stays there).
// Built only when the engine library monkey_dust_mygui is present (MD_MYGUI); Available() tells. The ImGui panel (character_editor.h) stays as the fallback path.
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_device.h>

#include <string>
namespace CharacterMyGui {

bool DebugTextFit(int& checked, int& failed, std::string& list);
bool DebugLayoutRects(int& checked, int& failed, std::string& list);   // sweep (D-006): rect of every widget = the layout file x UI scale, font height = height at scale 1 x scale
bool Available();                          // compiled in
// ImGui side, inside the Characters tab: draws the 3D preview in the middle of the region and reserves it; the window is drawn over it later (Draw).
// False when MyGUI or the install is not usable (the caller then draws the ImGui panel).
bool DrawTab(float w, float h);
void OnEvent(const SDL_Event& e);          // keyboard / text events of the SDL pump
void Prepare(md::GpuCommandBufferHandle cmd);                                   // before the render passes
void Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd);               // inside the ImGui pass of the frame, viewport and scissor of the tab region
bool Visible();                            // DrawTab ran this frame
// scenarios / tests (no ImGui): widget caption by name, click a button by name, slider position
const char* DebugCaption(const char* widget);
bool DebugClick(const char* widget);
int DebugClickCount();   // how many times a button handler ran
int DebugTab();   // open tab index (0 BODY, 1 FACE, 2 HAIR)
const char* DebugFocus();   // MyGUI widget under the mouse (nullptr: none)
bool DebugWidgetCentre(const char* widget, float& x, float& y);   // screen px of a widget centre, for real-input clicks
bool DebugSetSlider(const char* slider_caption, int value);
int DebugSliderPos(const char* key);   // scroll position of the open tab's slider row with this key (-1: none)
const char* DebugFirstSlider();
void DebugDraw(bool on);               // measurement: the window is (not) drawn
void DebugPointer(bool visible);       // shows / hides the MyGUI mouse pointer (scenarios hide it)
bool DebugDialog(const char* file);   // is the IMPORT/EXPORT dialog open; sets its file name field when file is non-empty
const char* DebugSliderKey(int n);
int DebugCharIndex(int set_count);   // CHANGE CHARACTER: set_count > 0 sets the number of characters (the editor has 1); returns the current index
bool DebugMultiSlider(int index, int value);   // the hair colour MultiSlider of the HAIR tab: sub-slider index 0..2
bool DebugListStep(int direction);         // the first ListScrollBar of the open tab (Face / Hair style), as its End (+1) / Start (-1) button
const char* DebugType(const char* widget, const char* utf8);   // types into an EditBox (NameText) through the SDL text events; returns its caption            // name of the first slider row of the open tab (nullptr: none)
void DebugDump(const char* path);

} // namespace CharacterMyGui
