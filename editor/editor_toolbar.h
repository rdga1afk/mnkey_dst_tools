#pragma once
#ifdef MONKEY_DUST_EDITOR
#include "imgui.h"
#include "editor_core.h"

class EditorToolbar {
public:
    static EditorToolbar& Get() { static EditorToolbar inst; return inst; }

    void Draw(float dt);
    void SpawnEntity(const char* type);   // public — called by command palette

private:
    EditorToolbar() = default;

    void DrawMenuBar();
    void DrawButtonBar();
};
#endif
