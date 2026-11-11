#pragma once
// ImGui integration helpers for game builds.
// Rule M-A: include only from main.cpp.
// SDL_GPU path: imgui_impl_sdlgpu3; GL path: imgui_impl_opengl3.
#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include <monkey_dust/platform/window.h>
#include <monkey_dust/render/gpu_pipeline.h>

// Pump SDL events and feed to ImGui. Call immediately before imgui_new_frame().
// Also handles SDL_EVENT_WINDOW_DISPLAY_CHANGED for SDL_GPU path.
inline void imgui_pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL3_ProcessEvent(&e);
        if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED)
            SDL_MaximizeWindow(_wnd::ptr());
    }
}

#  include "backends/imgui_impl_sdlgpu3.h"

inline void imgui_init() { /* no-op: call imgui_sdlgpu_init() after GpuDevice is ready */ }
inline void imgui_shutdown() {
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}
inline void imgui_sdlgpu_init(md::GpuDeviceHandle device, SDL_GPUTextureFormat fmt) {
    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForSDLGPU(_wnd::ptr());
    ImGui_ImplSDLGPU3_InitInfo info = {};
    info.Device            = device;
    info.ColorTargetFormat = fmt;
    ImGui_ImplSDLGPU3_Init(&info);
}
inline void imgui_new_frame() {
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
#ifdef MONKEY_DUST_EDITOR
    extern void UiDriver_PreImGuiNewFrame();
    UiDriver_PreImGuiNewFrame();
#endif
    ImGui::NewFrame();
}
inline void imgui_render() { ImGui::Render(); }
// Call after all render passes but before Submit().
// sc_tex = swapchain texture acquired via GpuDevice::AcquireSwapchainTexture().
inline void imgui_sdlgpu_draw(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle sc_tex) {
    if (!sc_tex) return;
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) return;
    ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd = cmd;
    cpd.color_tex[0] = sc_tex;
    cpd.load_color = true;
    cb.BeginColorPass(cpd);
    if (cb.SDLPass()) ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, cb.SDLPass());
    cb.EndPass();
}

