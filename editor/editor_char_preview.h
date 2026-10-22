#pragma once
// Character editor preview: the body of the session's current race and gender, read from the Kenshi install (md::ogre mesh + skeleton through md::character::SkinnedBody),
// posed by the idle stance and the body-slider bone sizes (md::character::SolveSkinMatrices), face sliders as mesh poses; drawn off-screen and shown with ImGui::Image.
// RenderFrame() runs once per frame before the ImGui render (main.cpp); DrawInImGui() inside the Characters tab. Data logic is in tools/editor/character/.
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_device.h>
#include <cstdio>

namespace md::character { class Session; }

namespace CharPreviewSDLGPU {

// Binds the session; the body is (re)loaded when its race or gender changes. False when the install lacks the files (the preview then shows nothing).
bool Bind(md::character::Session* session);
bool Active();
void RenderFrame(md::GpuCommandBufferHandle cmd);
void DrawInImGui(float W, float H);
void ReloadPipelines();
void GetCamera(float& pitch, float& distance, float& look_at_y);   // for the behaviour tests
void SetCameraForTab(int tab);          // 0 BODY: whole body, others: head and shoulders
void SetKenshiLight(bool on);           // Kenshi's character-screen lighting (Deferred_RTT, #192); default off until the owner approves the new look
void SetYaw(float radians);             // scenarios: fixed view, no rotation by input
void SetCamera(float pitch, float distance, float look_at_y);   // scenarios: pitch (radians), distance (m), height of the look-at point (m)
void DumpState(FILE* f);

} // namespace CharPreviewSDLGPU
