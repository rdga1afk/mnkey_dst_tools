#pragma once
#ifdef MONKEY_DUST_EDITOR
#include "imgui.h"
#include <monkey_dust/ecs/md_entity.h>

class EditorInspector {
public:
    static EditorInspector& Get() { static EditorInspector inst; return inst; }

    void Draw();
    void DrawContent();

private:
    EditorInspector() = default;

    void DrawTransform(MdEntity e);
    void DrawHealth(MdEntity e);
    void DrawNavAgent(MdEntity e);
    void DrawPlayerController(MdEntity e);
    void DrawCharacterDef(MdEntity e);
    void DrawAddComponent(MdEntity e);

    bool CollapsingSection(const char* label, ImVec4 color);
};
#endif
