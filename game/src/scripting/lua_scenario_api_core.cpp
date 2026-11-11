#include "lua_scenario_api_internal.h"

static float ScenarioRandF01() {
    // xorshift32
    uint32_t x = s_scenario_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_scenario_rng = x;
    return (float)(x >> 8) / (float)(1u << 24);
}

MdEntity ToEntity(lua_State* L, int idx) {
    return MdRegistry::Get().FromIndex(
        static_cast<uint32_t>(luaL_checkinteger(L, idx)));
}

// ── md.rand() / md.rand(a, b) ────────────────────────────────────────────────
int l_md_rand(lua_State* L) {
    int nargs = lua_gettop(L);
    if (nargs >= 2) {
        lua_Integer a = luaL_checkinteger(L, 1);
        lua_Integer b = luaL_checkinteger(L, 2);
        if (b < a) { lua_Integer t = a; a = b; b = t; }
        lua_Integer span = b - a + 1;
        lua_Integer v = a + (lua_Integer)(ScenarioRandF01() * (double)span);
        if (v > b) v = b;  // guard the (rare) f==1.0 edge
        lua_pushinteger(L, v);
    } else {
        lua_pushnumber(L, (lua_Number)ScenarioRandF01());
    }
    return 1;
}

// ── md.teleport_camera(x, z) ──────────────────────────────────────────────────
// Y auto-lift happens every frame via the existing orbit-camera terrain
// clamp (main.cpp UpdateOrbitCamera) — this only needs to set x/z.
int l_md_teleport_camera(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float z = (float)luaL_checknumber(L, 2);
    if (s_cam_x) *s_cam_x = x;
    if (s_cam_z) *s_cam_z = z;
    return 0;
}

// ── md.set_camera_orbit(yaw_deg, pitch_deg, dist) ─────────────────────────
// terrain-perf-measure (2026-08-13): controls the release build's fixed
// third-person orbit camera (g_cam_az/el/dist, main.cpp) directly -- the
// release build has no free F3 camera, so before this a perf-measure
// script's only lever was teleport_player(x,z), and how much terrain ended
// up in frame depended entirely on which direction the DEFAULT orbit
// (az=0/south, ~22m distance) happened to face from that point -- not
// reproducible across runs (confirmed live: same intended scene gave wildly
// different tier_counts session to session because of this). yaw_deg is
// degrees clockwise from south (matches g_cam_az's 0=south convention);
// pitch_deg is elevation in degrees above horizon; dist is orbit distance
// in metres. No-op (silently ignored) if called before
// RegisterLuaScenarioOrbitCamera has wired the pointers (main()'s loop-
// local g_cam_az/el/dist don't exist until after InitGame() returns).
int l_md_set_camera_orbit(lua_State* L) {
    float yaw_deg   = (float)luaL_checknumber(L, 1);
    float pitch_deg = (float)luaL_checknumber(L, 2);
    float dist      = (float)luaL_checknumber(L, 3);
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    if (s_cam_az)   *s_cam_az   = yaw_deg * kDegToRad;
    if (s_cam_el)   *s_cam_el   = pitch_deg * kDegToRad;
    if (s_cam_dist) *s_cam_dist = dist;
    return 0;
}

// ── md.spawn(archetype, x, z, count) -> {entity_ids} ──────────────────────────
// Etap 1 scope: "npc" is the only supported archetype (matches the
// scenario examples in AUTONOMY_SYSTEM_PROMPT_v2.md Etap 3 — a single
// walking/combat-capable NPC). A full archetype-JSON spawner is
// editor-only (tools/editor/npc_archetype_spawn.h) and cannot be reused
// here per split-readiness (tools/ must not be #included from game/).
// ── md.teleport_player(x, z) ──────────────────────────────────────────────────
// x/z are REAL absolute Kenshi world metres (same convention as
// the zone grid cell (grid_x/z) * CHUNK_SIZE, matching l_md_world_to_local's
// documented input) -- WorldTransform.x/z themselves are NOT absolute
// metres, they're SceneRender's local floating-window space (see
// SceneRender::tnoff_x's doc comment; InitGame's spawn_x = tnoff_x +
// spawn_half proves this). Must convert via the same abs->local relation
// l_md_world_to_local uses, or the player ends up at a bogus local
// position far outside the loaded 9x9 window (found via probe_player_map_
// marker.lua: an earlier version of this function wrote raw absolute
// metres straight into tr.x, silently teleporting the player to nonsense
// local coords and desyncing HandleTerrainStreaming's convergence).
int l_md_teleport_player(lua_State* L) {
    float wx = (float)luaL_checknumber(L, 1);
    float wz = (float)luaL_checknumber(L, 2);
    auto& sr = SceneRender::Get();
    sr.gr_reset_ = true;   // #163: the ground shading history is invalid after a teleport
    float x = wx + sr.tnoff_x - (float)sr.zone_ox * CHUNK_SIZE;
    float z = wz + sr.tnoff_z - (float)sr.zone_oz * CHUNK_SIZE;
    auto& reg = MdRegistry::Get();
    MdEntity player = MdEntity::Null();
    static auto q_player_lookup = reg.Raw().query().all<PlayerController>().all<WorldTransform>();
    player = MdFirst(q_player_lookup);
    if (!reg.Valid(player)) return 0;
    auto& tr = reg.Handle(player).get_mut<WorldTransform>();
    tr.x = x;
    tr.z = z;
    tr.y = TerrainQuery::Get().GetHeight(x, z) + 0.1f;
    if (tr.slot != TransformSoA::INVALID_SLOT) {
        auto& soa = TransformSoA::Get();
        soa.px[tr.slot] = tr.x;
        soa.pz[tr.slot] = tr.z;
        soa.py[tr.slot] = tr.y;
        soa.MarkTransformDirty(tr.slot);
    }
    return 0;
}

// ── md.terrain_height(x, z) -> h | nil ───────────────────────────────────────
// Via TerrainQuery — the single source of truth (terrain_query.h) — never
// TerrainChunk::SampleHeight() ad-hoc.
