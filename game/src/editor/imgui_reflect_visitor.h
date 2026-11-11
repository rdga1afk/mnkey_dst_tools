#pragma once
#ifdef MONKEY_DUST_EDITOR
#include "imgui.h"
#include <monkey_dust/ecs/component_reflect.h>
#include <cstdio>

// Draws an ImGui::DragFloat per F32 field of a reflected component
// (engine's ComponentReflect registry — engine/src/ecs/component_reflect.cpp).
// Non-float fields (e.g. WorldTransform::slot, a TransformSoA bookkeeping
// index) are silently skipped — not user-editable.
inline void DrawReflectedFloatFields(void* ptr, const md::ComponentDesc& desc) {
    uint8_t* base = static_cast<uint8_t*>(ptr);
    for (int i = 0; i < desc.field_count; ++i) {
        const md::FieldDesc& f = desc.fields[i];
        if (f.type != md::FieldType::F32) continue;
        float* val = reinterpret_cast<float*>(base + f.offset);
        char label[40];
        snprintf(label, sizeof(label), "%s##refl", f.name);
        if (f.ui_min < f.ui_max)
            ImGui::DragFloat(label, val, f.ui_speed, f.ui_min, f.ui_max);
        else
            ImGui::DragFloat(label, val, f.ui_speed);
    }
}
#endif
