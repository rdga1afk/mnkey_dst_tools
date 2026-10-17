#ifdef MONKEY_DUST_EDITOR
#include "editor_std_commands.h"
#include "editor_core.h"
#include "editor_command_palette.h"
#include "editor_reflect_bridge.h"
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/editor/cmd_path_validate.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

CmdResult DuplicateSelectedCmd(const CmdArgs&, MdRegistry& reg, uint8_t[64]) {
    auto& ec = EditorCore::Get();
    MdEntity src = ec.GetPrimary();
    if (!reg.Valid(src) || !reg.Handle(src).has<WorldTransform>()) {
        CmdResult r; r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "No valid primary selection to duplicate");
        return r;
    }
    MdEntity dst = reg.Create();
    reg.Handle(dst).emplace<WorldTransform>(reg.Handle(src).get_mut<WorldTransform>());
    auto& tr = reg.Handle(dst).get_mut<WorldTransform>();
    tr.x += 1.f; tr.slot = 0xFFFFFFFFu;
    tr.slot = TransformSoA::Get().Alloc(dst, tr.x, tr.z, 0);
    if (reg.Handle(src).has<Health>())     reg.Handle(dst).emplace<Health>(reg.Handle(src).get_mut<Health>());
    if (reg.Handle(src).has<Renderable>()) reg.Handle(dst).emplace<Renderable>(reg.Handle(src).get_mut<Renderable>());
    ec.Select(dst);
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "Duplicated entity %u -> %u", src.ToIntegral(), dst.ToIntegral());
    return r;
}

CmdResult SelectAllCmd(const CmdArgs&, MdRegistry& reg, uint8_t[64]) {
    auto& ec = EditorCore::Get();
    ec.DeselectAll();
    static auto q_all = reg.Raw().query().all<MdManagedTag&>();
    int n = 0;
    MdEach(q_all, [&](MdEntity fe, MdManagedTag) {
        if (ec.selected_count >= EditorCore::MAX_SELECTED) return;
        ec.selected[ec.selected_count++] = fe;
        ++n;
    });
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "Selected %d entities", n);
    return r;
}

CmdResult ReloadShadersCmd(const CmdArgs&, MdRegistry&, uint8_t[64]) {
    int ret = system("bash scripts/compile_shaders.sh 2>&1 | tail -3");
    CmdResult r;
    if (ret == 0) {
        extern void EditorReloadAllShaderPipelines();
        EditorReloadAllShaderPipelines();
        r.ok = true;
        snprintf(r.msg, sizeof(r.msg), "Shaders reloaded OK");
    } else {
        r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "Shader compilation FAILED");
    }
    return r;
}

// The registry's first genuinely undo-capable command (Phase 2): small,
// safe payload (2 uint32 ids + 2 int8 values, well under 64 bytes) — proves
// the mechanism end to end (registry-mediated capture -> tools/'s
// CommandStack push -> real Ctrl+Z) without the much bigger scope of
// capturing full entity+component state (Delete/Duplicate/Spawn/Kill stay
// NO_UNDO; see editor_std_commands.h's file header for why).

CmdResult FpsQueryCmd(const CmdArgs&, MdRegistry&, uint8_t[64]) {
    auto& ec = EditorCore::Get();
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "FPS=%.0f dt=%.2fms", ec.frame_fps, ec.frame_dt_ms);
    return r;
}

// Phase 3 (EDITOR_AUTOMATION_PLAN_v1.md agent authoring layer): named
// camera positions so an authoring script can jump to a consistent framing
// before md.editor.capture() — referenced directly in the plan's own text
// ("registered camera bookmarks (md.editor.exec('Camera Bookmark', ...)")
// but that single-command dual-purpose (save-or-goto) design doesn't fit
// CmdSchema's fixed-required-args shape cleanly, so this is two commands
// instead: Save/Goto. Fixed, no-heap slot table (MAX_BOOKMARKS), matching
// this project's array-not-container convention — oldest-name-wins is not
// implemented (a full slot table is a real, if unlikely, limitation for a
// script bookmarking more than MAX_BOOKMARKS distinct names).
namespace {
constexpr int MAX_BOOKMARKS = 16;
struct CameraBookmark {
    char name[32] = {};
    float x = 0.f, y = 0.f, z = 0.f, yaw = 0.f, pitch = 0.f;
    bool used = false;
};
CameraBookmark s_bookmarks[MAX_BOOKMARKS];

CameraBookmark* FindBookmark(const char* name) {
    for (int i = 0; i < MAX_BOOKMARKS; ++i) {
        if (s_bookmarks[i].used && strncmp(s_bookmarks[i].name, name, sizeof(s_bookmarks[i].name)) == 0) {
            return &s_bookmarks[i];
        }
    }
    return nullptr;
}
} // namespace

// args: [0]=name(str)
CmdResult SaveCameraBookmarkCmd(const CmdArgs& args, MdRegistry&, uint8_t[64]) {
    const char* name = args.values[0].str;
    CameraBookmark* bm = FindBookmark(name);
    if (!bm) {
        for (int i = 0; i < MAX_BOOKMARKS; ++i) {
            if (!s_bookmarks[i].used) { bm = &s_bookmarks[i]; break; }
        }
    }
    CmdResult r;
    if (!bm) {
        r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "No free bookmark slots (max %d)", MAX_BOOKMARKS);
        return r;
    }
    auto& ec = EditorCore::Get();
    strncpy(bm->name, name, sizeof(bm->name) - 1);
    bm->name[sizeof(bm->name) - 1] = '\0';
    bm->x = ec.cam_target.x; bm->y = ec.cam_target.y; bm->z = ec.cam_target.z;
    bm->yaw = ec.cam_yaw; bm->pitch = ec.cam_pitch;
    bm->used = true;
    snprintf(r.msg, sizeof(r.msg), "Saved camera bookmark '%s'", name);
    return r;
}

// args: [0]=name(str)
CmdResult GotoCameraBookmarkCmd(const CmdArgs& args, MdRegistry&, uint8_t[64]) {
    const char* name = args.values[0].str;
    CameraBookmark* bm = FindBookmark(name);
    CmdResult r;
    if (!bm) {
        r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "Unknown camera bookmark: %s", name);
        return r;
    }
    auto& ec = EditorCore::Get();
    ec.cam_target.x = bm->x;
    ec.cam_target.y = bm->y;
    ec.cam_target.z = bm->z;
    ec.cam_yaw = bm->yaw;
    ec.cam_pitch = bm->pitch;
    snprintf(r.msg, sizeof(r.msg), "Moved camera to bookmark '%s'", name);
    return r;
}

// args: [0]=entity_id(entity), [1]=component_name(str) — matches
// EcsReflectBridge::Desc(i).name (snake_case reflect name, not the
// PascalCase internal type name used for by-name resolution).
// gaia-ecs migration (Phase 5, PROMPT_GAIA_MIGRATION.md §7 p.2): ported off
// the flecs-only stub via the same EcsBridge* wrappers editor_reflect_inspector.cpp
// uses. EcsBridgeAddDefault() (not the plain tag-only add) is required here
// specifically because some reflected components are GAIA_STORAGE(Sparse)
// (Phase 2) -- gaia's bare add(entity, componentEntity) asserts for those,
// add_raw() with a zeroed default payload is the one path that works for
// both storage kinds uniformly (verified by reading World::add_raw()'s body).
CmdResult AddComponentCmd(const CmdArgs& args, MdRegistry& reg, uint8_t[64]) {
    EcsBridgeWorldT* world = &reg.Raw();
    EcsBridgeIdT eid = EcsBridgeIdFromRaw(args.values[0].entity_id);
    CmdResult r;
    if (!EcsBridgeIdValid(eid) || !EcsBridgeIsAlive(world, eid)) {
        r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "Invalid or dead entity");
        return r;
    }
    const char* name = args.values[1].str;
    auto& bridge = EcsReflectBridge::Get();
    for (int i = 0; i < bridge.Count(); ++i) {
        if (strncmp(bridge.Desc(i).name, name, sizeof(bridge.Desc(i).name)) != 0) continue;
        EcsBridgeIdT cid = bridge.Id(i);
        if (!EcsBridgeIdValid(cid)) {
            r.ok = false;
            snprintf(r.msg, sizeof(r.msg), "Component '%s' has no resolved id", name);
            return r;
        }
        if (!EcsBridgeAddDefault(world, eid, cid, bridge.Desc(i).component_size)) {
            r.ok = false;
            snprintf(r.msg, sizeof(r.msg), "Failed to add component '%s'", name);
            return r;
        }
        r.ok = true;
        snprintf(r.msg, sizeof(r.msg), "Added %s to entity", name);
        return r;
    }
    r.ok = false;
    snprintf(r.msg, sizeof(r.msg), "Unknown component: %s", name);
    return r;
}

void RegisterStdEditorCommands(uint32_t owner_module_id) {
    auto& cmds = EditorCmdRegistry::Get();
    static CmdSchema kNoArgsNoUndo{ {}, 0, true };
    cmds.Register("Delete Selected",    DeleteSelectedCmd,    nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("Duplicate Selected", DuplicateSelectedCmd, nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("Select All",         SelectAllCmd,         nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("Reload Shaders",     ReloadShadersCmd,     nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("FPS Query",          FpsQueryCmd,          nullptr, nullptr, kNoArgsNoUndo, owner_module_id);

    static CmdSchema kAddComponentSchema{
        { {"entity_id", CmdArgType::Entity}, {"component_name", CmdArgType::Str} }, 2, true };
    cmds.Register("Add Component", AddComponentCmd, nullptr, nullptr, kAddComponentSchema, owner_module_id);

    static CmdSchema kCameraBookmarkSchema{ { {"name", CmdArgType::Str} }, 1, true };
    cmds.Register("Save Camera Bookmark", SaveCameraBookmarkCmd, nullptr, nullptr, kCameraBookmarkSchema, owner_module_id);
    cmds.Register("Goto Camera Bookmark", GotoCameraBookmarkCmd, nullptr, nullptr, kCameraBookmarkSchema, owner_module_id);
}

CmdResult DispatchEditorCmd(const char* name, const CmdArgs& args) {
    DispatchOutcome out = EditorCmdRegistry::Get().Dispatch(name, args, MdRegistry::Get());
    if (out.has_undo) {
        Command c;
        c.execute = out.redo;
        c.undo = out.undo;
        memcpy(c.data, out.undo_data, sizeof(c.data));
        EditorCore::Get().history.Push(c);
    }
    return out.result;
}
#endif
