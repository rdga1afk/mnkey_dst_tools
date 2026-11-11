#include "ui_driver.h"
#include <cstring>

#ifdef MD_UI_TESTS
#include "imgui.h"
#include "imgui_internal.h" // temp diagnostic: GImGui->HoveredWindow/HoveredId/ActiveId
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_internal.h" // ImGuiTestEngine_FindItemInfo
#include "imgui_test_engine/imgui_te_utils.h"
#include <monkey_dust/platform/window.h>
#include <SDL3/SDL.h>
#include <cstring>
#include <cstdio>

namespace {

ImGuiTestEngine* g_engine = nullptr;

// ImGuiTestEngine_FindItemInfo's underlying ImGuiTestInfoTask self-expires
// after 20 frames without a renewing query (ImGuiTestEngine_PostNewFrame's
// garbage collection) — fine for a TestFunc polling every frame inside a
// tight loop, but this driver is queried from md.ui.find/click roughly
// once per real command-file poll, which (being driven by an external
// agent sending one command, waiting, sending the next) is routinely
// 30-60+ frames apart. Without re-querying in between, every external
// call would hit an already-expired (or not-yet-warmed) task and always
// see ok=false, no matter how long the widget has actually been on
// screen. Fix: track every path anyone has ever asked about and
// re-query all of them every single frame from UiDriver_Tick(), keeping
// their tasks perpetually renewed; md.ui.find/click then just reads the
// last cached result instead of calling FindItemInfo directly.
constexpr int MAX_TRACKED = 24;
struct TrackedItem {
    bool               used = false;
    char               path[96] = {};
    ImGuiID            id = 0;
    UiDriverFindResult last;
};
TrackedItem g_tracked[MAX_TRACKED];

TrackedItem* TrackPath(const char* path) {
    for (int i = 0; i < MAX_TRACKED; ++i)
        if (g_tracked[i].used && strncmp(g_tracked[i].path, path, sizeof(g_tracked[i].path)) == 0)
            return &g_tracked[i];
    for (int i = 0; i < MAX_TRACKED; ++i) {
        if (g_tracked[i].used) continue;
        g_tracked[i].used = true;
        strncpy(g_tracked[i].path, path, sizeof(g_tracked[i].path) - 1);
        g_tracked[i].id = ImHashDecoratedPath(path, nullptr, 0);
        return &g_tracked[i];
    }
    return nullptr; // table full — caller is tracking too many distinct widgets at once
}

// One pending click at a time — a script/agent issuing overlapping clicks
// is a caller bug, not something worth a queue for. Countdown counted in
// real frames via UiDriver_Tick(), no coroutine involved.
struct PendingClick {
    bool  active = false;
    int   frames_left = 0;
    float x = 0.f, y = 0.f;
};
PendingClick g_pending_click;

// Synthetic mouse override, re-asserted directly into io.MousePos/
// io.MouseDown[0] every frame from UiDriver_PreImGuiNewFrame() — see that
// function's doc comment for why this bypasses SDL_PushEvent entirely.
bool  g_mouse_active = false;
float g_mouse_x = 0.f, g_mouse_y = 0.f;
bool  g_mouse_down = false;

// Held keys with a scheduled auto-release (UiDriver_HoldKey). Fixed array,
// no heap (CLAUDE.md convention) — MAX_HELD covers WASD+Shift+a few extras
// with headroom; UiDriver_KeyDown() alone (no countdown) is unlimited and
// doesn't use this table at all.
constexpr int MAX_HELD = 8;
struct HeldKey {
    bool         active = false;
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    int          frames_left = 0;
};
HeldKey g_held[MAX_HELD];

// SDL_Event is a union — writing the Uint32 ev.type field also sets each
// substruct's own SDL_EventType-typed .type field (same 4 bytes), so only
// one assignment is needed; the two fields have different C++ types
// (Uint32 vs SDL_EventType) and cannot both be assigned the same value
// without a cast, hence setting only ev.type here.
void PushKeyEvent(SDL_Scancode sc, bool down) {
    SDL_Event ev = {};
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.windowID = SDL_GetWindowID(_wnd::ptr());
    ev.key.scancode = sc;
    ev.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    ev.key.down = down;
    SDL_PushEvent(&ev);
}

} // namespace

void UiDriver_Init() {
    g_engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngine_Start(g_engine, ImGui::GetCurrentContext());
    fprintf(stdout, "[UiDriver] started (MD_UI_TESTS)\n");
}

void UiDriver_PreImGuiNewFrame() {
    if (!g_mouse_active) return;
    // Re-assert every frame: SDL_GetKeyboardFocus()-driven mouse tracking
    // (ImGui_ImplSDL3_UpdateMouseData()/NewFrame()'s MousePendingLeaveFrame
    // logic) resets io.MousePos to (-FLT_MAX,-FLT_MAX) whenever this window
    // gets a real SDL_EVENT_WINDOW_MOUSE_LEAVE — which fires repeatedly in
    // this headless/background environment since the window never holds
    // genuine OS mouse focus. Driving io.AddMousePosEvent/AddMouseButtonEvent
    // directly, called right after ImGui_ImplSDL3_NewFrame() but before
    // ImGui::NewFrame() drains the event queue (see imgui_platform.h), makes
    // our position/button state the last-applied event each frame — always
    // winning over that race. Keyboard is unaffected (still SDL_PushEvent,
    // see PushKeyEvent) since it doesn't hit this leave-focus path and needs
    // to land in real SDL keystate for player movement.
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(g_mouse_x, g_mouse_y);
    io.AddMouseButtonEvent(0, g_mouse_down);
}

void UiDriver_Tick() {
    if (!g_engine) return;

    if (g_pending_click.active) {
        if (--g_pending_click.frames_left <= 0) {
            g_mouse_down = false;
            g_pending_click.active = false;
        }
    }
    for (int i = 0; i < MAX_HELD; ++i) {
        if (!g_held[i].active) continue;
        if (--g_held[i].frames_left <= 0) {
            PushKeyEvent(g_held[i].scancode, false);
            g_held[i].active = false;
        }
    }

    // Re-query every tracked path EVERY frame so its ImGuiTestInfoTask
    // never goes 20 frames without a renewing query (which would let the
    // test engine garbage-collect it — see TrackPath's doc comment above).
    // This is what makes md.ui.find/click work correctly when called from
    // an external command-file poll tens of frames apart, not just from a
    // tight in-process loop.
    for (int i = 0; i < MAX_TRACKED; ++i) {
        if (!g_tracked[i].used) continue;
        ImGuiTestItemInfo* info = ImGuiTestEngine_FindItemInfo(g_engine, g_tracked[i].id, g_tracked[i].path);
        if (!info) { g_tracked[i].last = UiDriverFindResult{}; continue; }
        g_tracked[i].last.ok = true;
        g_tracked[i].last.x = info->RectFull.Min.x;
        g_tracked[i].last.y = info->RectFull.Min.y;
        g_tracked[i].last.w = info->RectFull.Max.x - info->RectFull.Min.x;
        g_tracked[i].last.h = info->RectFull.Max.y - info->RectFull.Min.y;
    }
}

UiDriverFindResult UiDriver_Find(const char* path) {
    if (!g_engine || !path || !path[0]) return UiDriverFindResult{};
    TrackedItem* t = TrackPath(path);
    if (!t) return UiDriverFindResult{}; // tracking table full
    return t->last;
}

bool UiDriver_Click(const char* path) {
    UiDriverFindResult found = UiDriver_Find(path);
    if (!found.ok) return false;
    float cx = found.x + found.w * 0.5f;
    float cy = found.y + found.h * 0.5f;
    g_mouse_active = true;
    g_mouse_x = cx;
    g_mouse_y = cy;
    g_mouse_down = true;
    g_pending_click.active = true;
    g_pending_click.frames_left = 2;
    g_pending_click.x = cx;
    g_pending_click.y = cy;
    return true;
}

bool UiDriver_KeyDown(const char* key_name) {
    if (!key_name || !key_name[0]) return false;
    SDL_Scancode sc = SDL_GetScancodeFromName(key_name);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    PushKeyEvent(sc, true);
    return true;
}

bool UiDriver_KeyUp(const char* key_name) {
    if (!key_name || !key_name[0]) return false;
    SDL_Scancode sc = SDL_GetScancodeFromName(key_name);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    PushKeyEvent(sc, false);
    // Clear any matching held-key slot so its own countdown doesn't
    // send a redundant key-up later.
    for (int i = 0; i < MAX_HELD; ++i)
        if (g_held[i].active && g_held[i].scancode == sc) g_held[i].active = false;
    return true;
}

bool UiDriver_HoldKey(const char* key_name, int frames) {
    if (!key_name || !key_name[0] || frames <= 0) return false;
    SDL_Scancode sc = SDL_GetScancodeFromName(key_name);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    int slot = -1;
    for (int i = 0; i < MAX_HELD; ++i) {
        if (g_held[i].active && g_held[i].scancode == sc) { slot = i; break; }
        if (slot < 0 && !g_held[i].active) slot = i;
    }
    if (slot < 0) return false; // table full — caller is holding too many keys at once
    PushKeyEvent(sc, true);
    g_held[slot].active = true;
    g_held[slot].scancode = sc;
    g_held[slot].frames_left = frames;
    return true;
}

void UiDriver_Debug(char* out, int out_size) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const char* hovered_win = g.HoveredWindow ? g.HoveredWindow->Name : "(null)";
    const char* active_win  = g.ActiveIdWindow ? g.ActiveIdWindow->Name : "(null)";
    snprintf(out, (size_t)out_size,
        "hoveredWin=%s hoveredId=0x%08X activeWin=%s activeId=0x%08X "
        "mousePos=(%.1f,%.1f) mouseDown0=%d",
        hovered_win, g.HoveredId, active_win, g.ActiveId,
        g.IO.MousePos.x, g.IO.MousePos.y, g.IO.MouseDown[0] ? 1 : 0);
}

#else // !MD_UI_TESTS — no-op stubs so callers don't need their own #ifdef

void UiDriver_Init() {}
void UiDriver_PreImGuiNewFrame() {}
void UiDriver_Tick() {}
UiDriverFindResult UiDriver_Find(const char*) { return UiDriverFindResult{}; }
bool UiDriver_Click(const char*) { return false; }
bool UiDriver_KeyDown(const char*) { return false; }
bool UiDriver_KeyUp(const char*) { return false; }
bool UiDriver_HoldKey(const char*, int) { return false; }
void UiDriver_Debug(char* out, int out_size) { if (out_size > 0) out[0] = '\0'; }

#endif // MD_UI_TESTS

// Tab selection needs no test engine (ImGuiTabItemFlags_SetSelected), so it works in every build: md.ui.select_tab (#114 Settings check)
namespace {
char g_forced_tab[32] = {};
}

void UiDriver_SelectTab(const char* name) {
    if (!name) return;
    strncpy(g_forced_tab, name, sizeof(g_forced_tab) - 1);
}

const char* UiDriver_ConsumeForcedTab() {
    if (g_forced_tab[0] == '\0') return nullptr;
    static thread_local char out[32];
    strncpy(out, g_forced_tab, sizeof(out) - 1);
    g_forced_tab[0] = '\0'; // one-shot
    return out;
}

