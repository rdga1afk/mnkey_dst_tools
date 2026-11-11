#pragma once
// ui_driver.h — live ImGui-widget driver for the game's F3 debug overlay.
//
// Built only with -DMD_UI_TESTS=ON (same flag Phase 5 uses for
// monkey_dust_editor's panel smoke tests — vendors the SAME
// third_party/imgui_test_engine). Unlike Phase 5's usage (registered
// ImGuiTest/TestFunc run once through the test-runner's coroutine), this
// uses ONLY the engine's low-level, coroutine-free item-lookup API
// (ImGuiTestEngine_FindItemInfo) — verified to work every frame from the
// main thread with just ImGuiTestEngine_Start() called once, no
// QueueTests/TestContext/coroutine ever needed (the hooks that populate
// the item registry are gated by a per-ImGuiContext bool
// (TestEngineHookItems), not by "a test is running" — see
// imgui_te_engine.cpp's ImGuiTestEngine_UpdateHooks).
//
// Item lookup uses ImHashDecoratedPath() (imgui_te_utils.h) to turn a
// path string like "##editor/##tabs/Items/Detach##items" into the exact
// same ImGuiID the real nested widget has at runtime: ImGui's ID stack
// nesting is a streaming CRC32 fold (PushID(s) computes
// crc32(parent_id, s)), which is associative for concatenation — hashing
// "Hello/world" (segment-by-segment, "/" stripped as a pure delimiter) is
// mathematically identical to the real nested hash(hash(seed,"Hello"),
// "world"). No ImGuiTestContext, no navigation, needed for lookup itself.
//
// Player/camera movement (WASD) is NOT driven via ImGui at all — this
// codebase's F3 Flythrough camera and player movement both read
// input_key_down() (engine/include/monkey_dust/platform/input.h). Key/
// mouse simulation here is done via SDL_PushEvent() with synthetic
// SDL_EVENT_KEY_DOWN/UP / SDL_EVENT_MOUSE_* events, which flows through
// the SAME SDL_PollEvent loop main.cpp runs every frame and correctly
// drives both ImGui (via ImGui_ImplSDL3_ProcessEvent) and
// input_key_down()/input_key_pressed(), since those now read a
// SDL_AddEventWatch-fed _sdl3_input::s_down[]/s_keys[] array rather than
// SDL_GetKeyboardState() (which does NOT observe SDL_PushEvent-injected
// events — see input_key_down's doc comment in input.h for the verified
// root cause). No game-layer override is needed here anymore.

void UiDriver_Init();

// Call from imgui_platform.h's imgui_new_frame(), directly between
// ImGui_ImplSDL3_NewFrame() and ImGui::NewFrame() — re-asserts any active
// synthetic mouse position/button state as the last event of the frame, so
// it always wins over the SDL backend's own real-mouse-leave-driven
// io.MousePos resets (see the doc comment on the .cpp definition for why
// this environment hits that race constantly). No-op if MD_UI_TESTS is off
// or no click is currently active.
void UiDriver_PreImGuiNewFrame();

// Call once per frame from the game's normal interactive loop (right
// after the F3 overlay has been built for this frame — FindItemInfo's
// freshness window is +-2 frames, so exact ordering isn't critical, but
// logically this comes after BuildUI). No-op if not built with MD_UI_TESTS.
void UiDriver_Tick();

// Result of a find: matches md.ui.find()'s return table.
struct UiDriverFindResult {
    bool  ok = false;
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

// Resolve a decorated ID path (e.g. "##editor/##tabs/Camera/Game") to its
// current on-screen rect. Returns ok=false on the first call for a given
// path (info task warm-up — see doc comment above) or if the widget isn't
// currently drawn (wrong tab active, detached window closed, etc).
UiDriverFindResult UiDriver_Find(const char* path);

// Resolves the path, then schedules a synthetic mouse click (move + down
// this frame, up ~2 frames later) at its center. Returns false if the
// path can't be resolved right now (same warm-up/visibility caveats as
// Find()).
bool UiDriver_Click(const char* path);

// Direct synthetic key down/up (SDL scancode name, e.g. "W", "A", "Left
// Shift" — matches SDL_GetScancodeFromName()). Held until explicitly
// released via key_up (or hold_key's own countdown) — safe to call
// key_down again while already down (idempotent, no double-press).
bool UiDriver_KeyDown(const char* key_name);
bool UiDriver_KeyUp(const char* key_name);

// Convenience: key_down now, auto key_up after `frames` real frames
// (counted in UiDriver_Tick(), not tied to any coroutine).
bool UiDriver_HoldKey(const char* key_name, int frames);

// Temporary diagnostic: dumps ImGui's actual g.HoveredWindow/HoveredId/
// ActiveId/io.MousePos/MouseDown[0] into a fixed buffer (caller-owned,
// at least 256 bytes). Used to root-cause why tab-item clicks weren't
// registering while plain Button clicks worked.
void UiDriver_Debug(char* out, int out_size);

// Selects an F3 tab directly via ImGuiTabItemFlags_SetSelected — bypasses
// mouse simulation entirely (same pattern already used by the standalone
// editor's md.editor_open_panel/EditorPanels_ConsumeForcedTab). One-shot:
// consumed by editor_core.cpp's Update() on the next frame it runs, then
// cleared automatically. Valid names: "Scene","AI","Animation","FlowGraph",
// "Debug","Camera","Settings". No-op if MD_UI_TESTS is off.
void UiDriver_SelectTab(const char* name);

// Reads + clears the pending tab name set by UiDriver_SelectTab(). Returns
// nullptr if none pending. Called once per frame by editor_core.cpp.
const char* UiDriver_ConsumeForcedTab();
