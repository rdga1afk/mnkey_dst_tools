#include "slice_game.h"
#include "game_common.h"
#include <monkey_dust/nav/nav_world.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/world/slice_squad.h>
#include <monkey_dust/world/start_squad.h>
#include <monkey_dust/world/town_nodes.h>
#include <monkey_dust/world/town_spawn.h>
#include <monkey_dust/world/terrain_query.h>
#include <monkey_dust/world/chunk_def.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/animator.h>
#include <monkey_dust/components/nav_agent.h>
#include "render/scene_render.h"
#include <cmath>
#include <cstdlib>

namespace md::slice {
namespace {
constexpr float kPickRadius = 1.2f;        // m around a character's centre, 1 m above the feet

md::world::SliceSquad s_squad;
MdEntity s_ent[md::world::kSliceMaxChars];
bool s_init = false;

// local (WorldTransform) <-> absolute metres, the relation md.spawn_npc / md.teleport_player use
void ToAbs(float lx, float lz, float& ax, float& az) { auto& sr = SceneRender::Get(); ax = lx - sr.tnoff_x + (float)sr.zone_ox * CHUNK_SIZE; az = lz - sr.tnoff_z + (float)sr.zone_oz * CHUNK_SIZE; }
void ToLocal(float ax, float az, float& lx, float& lz) { auto& sr = SceneRender::Get(); lx = ax + sr.tnoff_x - (float)sr.zone_ox * CHUNK_SIZE; lz = az + sr.tnoff_z - (float)sr.zone_oz * CHUNK_SIZE; }

float Ground(float x, float z, float fallback) { float h; return TerrainQuery::Get().GetHeight(x, z, h) ? h : fallback; }

void Put(int i) {
    auto& reg = MdRegistry::Get();
    auto& tr = reg.Handle(s_ent[i]).get_mut<WorldTransform>();
    const md::world::SliceChar& c = s_squad.c[i];
    tr.x = c.pos[0]; tr.z = c.pos[2]; tr.y = c.pos[1];
    if (tr.slot != TransformSoA::INVALID_SLOT) {
        auto& soa = TransformSoA::Get();
        soa.px[tr.slot] = tr.x; soa.pz[tr.slot] = tr.z; soa.py[tr.slot] = tr.y; soa.MarkTransformDirty(tr.slot);
    }
}

// TEST-ONLY start (owner 18.11, DE-350): the squad is the record "The Cannibal Hunters" of the MERGED load order (base plus the core mods, as the game reads it), placed at the player's position
// (the Hub from MD_START_WORLD) with the exe's +-50 m offsets, not at the record's own start; the original start of this record is (-43500, -6625) m. The offsets come from a fixed-seed generator
// (the exe's random source differs). A record that cannot be read, or lists no member, spawns nobody besides the player and logs why: there is no silent fallback to another squad.
int SquadCount() {
    md::fcs::Install inst(md::KenshiDir()); md::fcs::LoadOrder order; md::fcs::Error err; md::fcs::GameDataContainer data;
    if (!inst.LoadOrderFor(order, err) || !data.Load(order, err)) { MD_LOG(MD_LOG_WARNING, "[slice] load order not readable (%s): the squad is only the player", err.Message().c_str()); return 1; }
    const auto m = md::world::StartSquadMembers(data, "The Cannibal Hunters");
    if (m.empty()) { MD_LOG(MD_LOG_WARNING, "[slice] merged record The Cannibal Hunters lists no squad member: the squad is only the player"); return 1; }
    return (int)m.size() < md::world::kSliceMaxChars ? (int)m.size() : md::world::kSliceMaxChars;
}

float Offset() {   // uniform in -kStartJitterMetres..+kStartJitterMetres, fixed seed
    static uint32_t st = 0x2918u;
    st = st * 1664525u + 1013904223u;
    return ((float)(st >> 8) / (float)(1u << 24) * 2.f - 1.f) * md::world::kStartJitterMetres;
}

// one NPC entity at (x, z) in WorldTransform coordinates, on the ground (fallback y_fallback); the null entity when no transform slot is free. Shared by the slice squad and the MD_TOWN_NPC residents.
MdEntity MakeNpc(float x, float z, float y_fallback, float& y_out) {
    auto& reg = MdRegistry::Get();
    auto e = reg.Create();
    reg.Handle(e).emplace<WorldTransform>();
    auto& tr = reg.Handle(e).get_mut<WorldTransform>();
    const float y = Ground(x, z, y_fallback);
    tr.x = x; tr.z = z; tr.y = y; tr.rot_y = 0.f;
    tr.slot = TransformSoA::Get().Alloc(e, x, z, 0);
    if (tr.slot == TransformSoA::INVALID_SLOT) return MdEntity{};
    TransformSoA::Get().py[tr.slot] = y; TransformSoA::Get().MarkTransformDirty(tr.slot);
    reg.Handle(e).emplace<NavAgent>();
    reg.Handle(e).emplace<AnimatorComponent>();
    y_out = y;
    return e;
}

// MD_TOWN_NPC=1 (off by default, DE-364): the Hub's residents of the merged load order (list: town_spawn.h, 4 templates of rebirth.mod) are placed on the squad spawn nodes of the Hub's buildings
// (town_nodes.h; DEVIATION: town membership by radius) and created as NPC entities, capped by kTownNpcBudget. Fixed seed; the Hub centre and radius come from the townlist / `size radius`.
void TownNpcSpawn(float fallback_y) {
    static const bool on = getenv("MD_TOWN_NPC") && getenv("MD_TOWN_NPC")[0] == '1'; if (!on) return;
    constexpr float kHubX = 9647.7f, kHubZ = 15038.8f, kHubR = 350.f;
    md::fcs::Install inst(md::KenshiDir()); md::fcs::LoadOrder order; md::fcs::Error err; md::fcs::GameDataContainer data;
    if (!inst.LoadOrderFor(order, err) || !data.Load(order, err)) { MD_LOG(MD_LOG_WARNING, "[town-npc] load order not readable (%s)", err.Message().c_str()); return; }
    std::vector<md::world::TownNode> nodes; std::string e2;
    if (!md::world::LoadTownNodes(md::KenshiDir(), data, kHubX, kHubZ, kHubR, nodes, e2)) { MD_LOG(MD_LOG_WARNING, "[town-npc] nodes not readable (%s)", e2.c_str()); return; }
    const auto l = md::world::MakeTownSpawnList(data, "18919-Newwworld.mod", 1);
    md::world::NodePlacer placer(nodes, 1);
    const auto planned = md::world::PlanTownSpawn(l, md::world::kTownNpcBudget, placer);
    size_t made = 0;
    for (const auto& m : planned) { float lx, lz, y; ToLocal(m.x, m.z, lx, lz); if (MakeNpc(lx, lz, fallback_y, y) != MdEntity{}) ++made; }
    MD_LOG(MD_LOG_INFO, "[town-npc] the Hub: owner %s, %zu templates, %zu anonymous + %zu named listed, %zu nodes, %zu planned, %zu created", l.owner.c_str(), l.templates.size(), l.anonymous.size(), l.named.size(), nodes.size(), planned.size(), made);
}

void Spawn(MdEntity player) {
    auto& reg = MdRegistry::Get();
    const WorldTransform pt = reg.Handle(player).get_mut<WorldTransform>();
    const int count = SquadCount();
    s_ent[0] = player; s_squad.Add(pt.x, pt.y, pt.z);
    for (int i = 1; i < count; ++i) {
        const float x = pt.x + Offset(), z = pt.z + Offset();
        float y; const MdEntity e = MakeNpc(x, z, pt.y, y);
        if (e == MdEntity{}) { MD_LOG(MD_LOG_WARNING, "[slice] no free transform slot"); break; }
        s_ent[s_squad.Add(x, y, z)] = e;
    }
}

// ray through the pixel (the construction UpdateOrbitCamera uses for click-to-move)
void PixelRay(const MdCamera& camera, const Input& in, float o[3], float d[3]) {
    static constexpr float DEG2R = 0.01745329251f;
    const float asp = (float)in.screen_w / (float)in.screen_h;
    Mat4 view = camera.ViewMatrix(); const float* vf = mat4_ptr(view);
    const float thf = tanf(camera.fovy * 0.5f * DEG2R);
    const float ndx = (2.f * in.mouse_x / (float)in.screen_w) - 1.f, ndy = 1.f - (2.f * in.mouse_y / (float)in.screen_h);
    d[0] = vf[0] * (ndx * thf * asp) + vf[1] * (ndy * thf) - vf[2];
    d[1] = vf[4] * (ndx * thf * asp) + vf[5] * (ndy * thf) - vf[6];
    d[2] = vf[8] * (ndx * thf * asp) + vf[9] * (ndy * thf) - vf[10];
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; ++k) d[k] /= len;
    o[0] = camera.pos.x; o[1] = camera.pos.y; o[2] = camera.pos.z;
}

int PickChar(const float o[3], const float d[3]) {
    int best = -1; float best_t = 1e9f;
    for (int i = 0; i < s_squad.n; ++i) {
        const float cx = s_squad.c[i].pos[0] - o[0], cy = s_squad.c[i].pos[1] + 1.f - o[1], cz = s_squad.c[i].pos[2] - o[2];
        const float t = cx * d[0] + cy * d[1] + cz * d[2];
        if (t < 0.f) continue;
        const float px = cx - t * d[0], py = cy - t * d[1], pz = cz - t * d[2];
        if (px * px + py * py + pz * pz <= kPickRadius * kPickRadius && t < best_t) { best_t = t; best = i; }
    }
    return best;
}

// ground point under the cursor: the ray against the horizontal plane through the camera target (as the existing click-to-move does)
bool GroundPoint(const MdCamera& camera, const float o[3], const float d[3], float& x, float& z) {
    if (d[1] > -1e-6f) return false;
    const float t = (camera.target.y - o[1]) / d[1];
    x = o[0] + t * d[0]; z = o[2] + t * d[2];
    return true;
}

void OrderMoveTo(float lx, float lz, bool walk) {
    const md::world::SliceChar* lead = nullptr;
    for (int i = 0; i < s_squad.n; ++i) if (s_squad.sel.Has(i + 1)) { lead = &s_squad.c[i]; break; }
    if (!lead) return;
    float ax, az, bx, bz;
    ToAbs(lead->pos[0], lead->pos[2], ax, az); ToAbs(lx, lz, bx, bz);
    float pts[256 * 3]; int n = 0; bool complete = false, busy = false;
    float local[256][3];
    if (md::nav::NavWorld::Get().FindPath(ax, az, bx, bz, pts, 256, &n, &complete, &busy) && n > 0) {
        for (int k = 0; k < n; ++k) { ToLocal(pts[k * 3], pts[k * 3 + 2], local[k][0], local[k][2]); local[k][1] = pts[k * 3 + 1]; }
    } else {                                           // no navmesh polygon (or a tile still being added): a straight line, the steering still avoids neighbours
        n = 1; local[0][0] = lx; local[0][1] = lead->pos[1]; local[0][2] = lz;
    }
    s_squad.OrderMove(local, n, walk ? 0 : 1);
}
}  // namespace

bool Enabled() { static const bool on = [] { const char* e = std::getenv("MD_SLICE"); return e && e[0] == '1'; }(); return on; }

void Update(MdEntity player, MdCamera& camera, const Input& in, float dt, float& cam_x, float& cam_z) {
    if (!s_init) { Spawn(player); TownNpcSpawn(s_squad.c[0].pos[1]); s_init = true; s_squad.Click(0, false); }
    if (in.screen_w > 0 && in.screen_h > 0 && (in.lmb_click || in.rmb_click)) {
        float o[3], d[3]; PixelRay(camera, in, o, d);
        if (in.lmb_click) { const int i = PickChar(o, d); if (i >= 0) s_squad.Click(i, in.shift); }
        else { float gx, gz; if (GroundPoint(camera, o, d, gx, gz)) OrderMoveTo(gx, gz, in.shift); }
    }
    md::world::SliceTick(s_squad, dt);
    auto& reg = MdRegistry::Get();
    float sx = 0.f, sz = 0.f; int cnt = 0;
    for (int i = 0; i < s_squad.n; ++i) {
        md::world::SliceChar& c = s_squad.c[i];
        c.pos[1] = Ground(c.pos[0], c.pos[2], c.pos[1]);
        Put(i);
        const float vx = c.vel[0], vz = c.vel[2];
        auto& nav = reg.Handle(s_ent[i]).get_mut<NavAgent>();
        nav.is_moving = c.moving; nav.move_speed = c.moving ? (c.cap > md::world::kWalkCapDefault ? 2.f : 1.f) : 0.f;
        nav.desired_vel_x = vx; nav.desired_vel_z = vz;
        if (c.moving && (vx != 0.f || vz != 0.f)) reg.Handle(s_ent[i]).get_mut<WorldTransform>().rot_y = std::atan2(vx, vz);
        if (s_squad.sel.Has(i + 1)) { sx += c.pos[0]; sz += c.pos[2]; ++cnt; }
    }
    if (cnt) { cam_x = sx / (float)cnt; cam_z = sz / (float)cnt; }
}

}  // namespace md::slice
