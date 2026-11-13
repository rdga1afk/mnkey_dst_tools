#include "../render/horizon_cull.h"
#include "monkey_dust/render/ground_slot.h"
#include "world/world_registry.h"
#include <monkey_dust/render/gpu_resource_tracker.h>
#include <fstream>
#include <map>
#include <sstream>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <monkey_dust/io/ogre_mesh.h>
#include "lua_scenario_api_internal.h"
#include <monkey_dust/nav/nav_world.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/zone_buildings.h>
#include <monkey_dust/io/fcs_gamedata.h>
#include "../render/hub_spike.h"
#include <monkey_dust/render/frame_counters.h>
#include "../render/town_buildings.h"
#include "../render/town_foliage.h"
#include <monkey_dust/world/feature_scatter.h>

float* s_cam_x = nullptr;
float* s_cam_z = nullptr;
float* s_cam_az = nullptr;
float* s_cam_el = nullptr;
float* s_cam_dist = nullptr;
uint32_t s_scenario_tick = 0;

// ── Scenario RNG (md.rand()) — separate from Combat::rng_state per-entity ──
// LCGs (see docs/AUTONOMY_LOG.md / MASTER PROMPT v2 principle 5: combat's
// own fixed-seed LCGs are already deterministic between runs, just not
// yet wired to --seed — Backlog item "seed-варіативність бою", not this).
uint32_t s_scenario_rng = 0x2545F491u;

void ScenarioRng_Seed(unsigned int seed) {
    s_scenario_rng = seed ? seed : 0x2545F491u;  // 0 would stall an xorshift
}

void ScenarioTick_Advance() { ++s_scenario_tick; ScenarioStateTrace_Tick(); }



// ── Registration ──────────────────────────────────────────────────────────────
extern bool g_terrain_cliff_normal_flip;
extern bool g_terrain_detail_n_atten;
extern bool g_terrain_lod;
extern int g_terrain_lod_patch;
extern bool g_opt_skip_flat_on_cliff;
extern bool g_opt_force_upscale;
extern int g_opt_ground_split;
extern bool g_opt_ground_reuse;
extern float g_terrain_lod_near_px, g_terrain_lod_far_px, g_terrain_lod_near_dist, g_terrain_lod_far_dist;
extern int g_terrain_ground_aniso_level;
extern int g_terrain_ground_min_lod;
extern bool g_terrain_light_clamp;
extern bool g_terrain_per_layer_mip;
extern bool g_terrain_ambientmap; // engine/src/render/terrain_shading_projected.cpp

// md.set_terrain_cliff_normal_flip(on): Kenshi step C (terrainfp4.hlsl:131-134), see npc_render_frame_prep.cpp.
static int l_md_set_terrain_cliff_normal_flip(lua_State* L) {
    g_terrain_cliff_normal_flip = lua_toboolean(L, 1) != 0;
    return 0;
}

// md.set_terrain_detail_n_atten(on): Kenshi step D (N.y^4 detail-normal attenuation, OFF by default since 2026-10-09), see npc_render_frame_prep.cpp.
static int l_md_set_terrain_detail_n_atten(lua_State* L) {
    g_terrain_detail_n_atten = lua_toboolean(L, 1) != 0;
    return 0;
}

// md.set_terrain_ground_min_lod(n): sampler min_lod (0..3) of the ground layers while md.set_terrain_ground_aniso >= 1; 1 = Kenshi's texture gimping 1.
static int l_md_set_terrain_ground_min_lod(lua_State* L) {
    const int v = (int)luaL_checkinteger(L, 1);
    g_terrain_ground_min_lod = v < 0 ? 0 : (v > 3 ? 3 : v);
    return 0;
}
// md.set_terrain_ground_aniso(level): ground layers through textureGrad with the pixel footprint. 1 = no anisotropy (default); 2/4/8/16 = anisotropic sampler (max_anisotropy).
static int l_md_set_terrain_ground_aniso(lua_State* L) {
    int v = (int)luaL_optinteger(L, 1, 1);
    g_terrain_ground_aniso_level = v < 1 ? 1 : (v > 16 ? 16 : v);
    return 0;
}

// md.set_terrain_lod(bool): #154 phase 3, Kenshi-style terrain LOD (quadtree by screen-space error, geomorph, skirts, vertex normals, 1.8 m near the camera); default on (13.10).
// md.set_terrain_lod_patch(16|64): quads per patch edge (restarts the table build; LOD is off until it is ready).
// md.set_terrain_lod_params(near_px, far_px, near_dist_m, far_dist_m): the detail threshold (defaults 5, 35, 750, 1500; Okran's setDetailThreshold(8, 8, 0) = 8, 8, 0, 1).
static int l_md_set_terrain_lod(lua_State* L) { g_terrain_lod = lua_toboolean(L, 1) != 0; return 0; }
// md.set_opt_skip_flat_on_cliff(bool): #155 step 1, skip the flat layer chain of the Kenshi ground shader where cliff_w >= 1 (pixel-identical); default on.
static int l_md_set_opt_skip_flat_on_cliff(lua_State* L) { g_opt_skip_flat_on_cliff = lua_toboolean(L, 1) != 0; return 0; }
// md.set_opt_force_upscale(on): the scene in its own target + the UI on the swapchain at scale 1.0 (for the 0-pixel proof against the normal path).
static int l_md_set_opt_ground_split(lua_State* L) { g_opt_ground_split = lua_isboolean(L, 1) ? (lua_toboolean(L, 1) ? 1 : 0) : (int)luaL_checkinteger(L, 1); return 0; }   // true / 1 = split, 2 = split without the copy (diagnostic)   // #163 5b-2a
extern int g_opt_ground_reuse_cr;
extern void md_settings_set_ground_reuse_period(int p);
extern void md_settings_set_ground_reuse_chain(int c);
extern int g_up_sharp;   // #207 (owner 31.10): 0 Quality, 1 Fast, 2 Off
static int l_md_set_up_sharp(lua_State* L) { g_up_sharp = (int)luaL_checkinteger(L, 1); if (g_up_sharp < 0 || g_up_sharp > 2) g_up_sharp = 0; return 0; }   // md.set_upscale_sharpness(0 Quality / 1 Fast / 2 Off): the sharpening of the upscale while C15 has lowered the resolution
static int l_md_set_hub_spike(lua_State* L) { HubSpike& h = HubSpike::Slot((int)luaL_checkinteger(L, 1)); if (lua_isnoneornil(L, 2)) { h.Shutdown(); lua_pushboolean(L, 1); } else { h.SetRange((float)luaL_optnumber(L, 3, 0.0)); lua_pushboolean(L, h.Load(luaL_checkstring(L, 2))); } return 1; }   // #119 spike: (slot 0..3, placement .txt | nil = off, range m)
static int l_md_set_hub_spike_instanced(lua_State* L) { HubSpike::SetInstanced(lua_toboolean(L, 1) != 0); HubSpike::SetVariant((int)luaL_optinteger(L, 2, 0)); return 0; }   // (instanced, fragment variant 0 full / 1 flat / 2 depth only / 3 depth + alpha)
static int l_md_set_hub_spike_occlusion(lua_State* L) { HubSpike::SetOcclusion(lua_toboolean(L, 1) != 0); return 0; }
static int l_md_set_hub_zone(lua_State* L) { HubSpike& h = HubSpike::Slot((int)luaL_checkinteger(L, 1)); h.SetRange((float)luaL_optnumber(L, 5, 0.0)); lua_pushboolean(L, h.LoadZone((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4))); return 1; }   // #119: (slot, cx m, cz m, radius m, range m) buildings straight from the zone files
void md_hub_set_cull_back(bool on);
extern bool md_settings_set_ground_preset(int n);
static int l_md_set_ground_preset(lua_State* L) { lua_pushboolean(L, md_settings_set_ground_preset((int)luaL_checkinteger(L, 1))); return 1; }   // #193: 0 dyn067_a1 (default), 1 dyn067, 2 reuse05
static int l_md_set_prop_cull(lua_State* L) { md_hub_set_cull_back(lua_toboolean(L, 1) != 0); return 0; }   // #119: back-face cull of building meshes by Kenshi material (before the buildings stream in)
// md.set_nav(on): the Kenshi navtile navmesh around the camera (md::nav::NavWorld), md.nav_path(ax, az, bx, bz) -> {{x, y, z}, ...}, complete (nil when a position has no polygon, nil + "busy" while the worker adds a tile), md.nav_stats() -> {tiles_live, tiles_requested, loaded_total, cache_hits, last_add_ms, max_add_ms, last_worker_ms}
// md.town_parts(cx, cz, r) -> { {mesh, x, y, z, y_off, scale, qw, qx, qy, qz}, ... }: the placed building parts of the zone files as the loader reads them (look-bug checks, #207)

// md.town_sweep(cx, cz, radius) -> table (defect sweep table 2): every placed part inside the circle against the data: mesh readable, every submesh material found in the install's .material scripts
// with its texture files on disk (parent chain followed; a "_HI" file missing but the plain one present counts as a fallback), rotation unit length, tilt of the instance's up axis from vertical.
static int l_md_town_sweep(lua_State* L) {
    namespace fs = std::filesystem;
    struct Mat { std::string parent; std::vector<std::string> tex; };
    static std::unordered_map<std::string, Mat> mats; static std::unordered_set<std::string> files; static bool built = false;
    auto low = [](std::string a) { for (char& c : a) c = (char)std::tolower((unsigned char)c); return a; };
    if (!built) {
        built = true; std::error_code ec;
        for (fs::recursive_directory_iterator it(fs::path(md::KenshiDir()) / "data", fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = low(it->path().extension().string());
            if (ext == ".dds" || ext == ".png" || ext == ".tga" || ext == ".jpg") { files.insert(low(it->path().filename().string())); continue; }
            if (ext != ".material") continue;
            std::ifstream f(it->path()); std::string line, cur; int depth = 0;
            while (std::getline(f, line)) {
                const size_t c = line.find("//"); if (c != std::string::npos) line.resize(c);
                std::istringstream ss(line); std::string w0; ss >> w0;
                if (depth == 0 && w0 == "material") { std::string name, w; ss >> name >> w; cur = name; Mat& m = mats[cur]; m = Mat{}; if (w == ":") { std::string par; ss >> par; if (!par.empty() && par.back() == '{') par.pop_back(); m.parent = par; } }
                else if (depth > 0 && !cur.empty() && w0 == "texture") { std::string v; ss >> v; if (!v.empty()) { std::replace(v.begin(), v.end(), '\\', '/'); const size_t sl = v.rfind('/'); mats[cur].tex.push_back(low(sl == std::string::npos ? v : v.substr(sl + 1))); } }
                for (char ch : line) { if (ch == '{') ++depth; else if (ch == '}') { if (--depth <= 0) { depth = 0; cur.clear(); } } }
            }
        }
    }
    std::vector<md::world::PlacedPart> parts; std::string err;
    md::world::LoadBuildingsAt(md::KenshiDir(), (float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), parts, err);
    std::unordered_map<std::string, std::vector<std::string>> seen_mesh;   // mesh -> problems (empty = fine)
    int mt_count[6] = {0, 0, 0, 0, 0, 0}, tile_not1 = 0, athr_not128 = 0; int parts_no_spec = 0, spec_tex_missing = 0; std::string nospec_names; int bad_rot = 0, no_mesh = 0, mat_missing = 0, mat_notex = 0, tex_missing = 0, tex_hi_fallback = 0; float max_tilt = 0.f; int tilted = 0; std::string tilted_names; std::string first_problems; std::map<std::string, std::string> alias_seen; int sub_with_diffuse_alias = 0;
    auto problem = [&](const std::string& m, const std::string& what) { if (first_problems.size() < 900) first_problems += m + ": " + what + "; "; };
    for (const auto& p : parts) {
        const float n = std::sqrt(p.quat[0] * p.quat[0] + p.quat[1] * p.quat[1] + p.quat[2] * p.quat[2] + p.quat[3] * p.quat[3]);
        if (std::fabs(n - 1.f) > 1e-3f) ++bad_rot;
        ++mt_count[std::min(5, std::max(0, p.material_type))]; if (p.tile[0] != 1.f || p.tile[1] != 1.f) ++tile_not1; if (p.alpha_threshold != 128) ++athr_not128;
        if (p.tex.empty()) { ++parts_no_spec; nospec_names += p.mesh.substr(p.mesh.rfind('/') + 1) + "; "; }
        else { std::error_code fec; if (!fs::exists(fs::path(md::KenshiDir()) / (p.tex.rfind("./", 0) == 0 ? p.tex.substr(2) : p.tex), fec)) { ++spec_tex_missing; problem(p.mesh, "spec texture file missing " + p.tex); } }
        const float w = p.quat[0] / n, x = p.quat[1] / n, y = p.quat[2] / n, z = p.quat[3] / n; (void)z; (void)w;
        const float up_y = 1.f - 2.f * (x * x + p.quat[3] / n * (p.quat[3] / n));   // y of the rotated (0,1,0)
        const float tilt = std::acos(std::min(1.f, std::max(-1.f, up_y))) * 57.29578f; (void)y;
        if (tilt > max_tilt) max_tilt = tilt; if (tilt > 5.f) { ++tilted; if (tilted_names.size() < 700) { char tb[40]; std::snprintf(tb, sizeof tb, " %.0fdeg;", tilt); tilted_names += p.mesh.substr(p.mesh.rfind('/') + 1) + tb; } }
        if (seen_mesh.count(p.mesh)) continue;
        std::vector<std::string>& pr = seen_mesh[p.mesh];
        md::ogre::Mesh m; md::ogre::Error oe; std::string path = md::KenshiDir() + "/" + (p.mesh.rfind("./", 0) == 0 ? p.mesh.substr(2) : p.mesh);
        if (!md::ogre::ReadMesh(path, m, oe) || m.submeshes.empty()) { ++no_mesh; pr.push_back("mesh unreadable"); problem(p.mesh, "mesh unreadable"); continue; }
        for (const auto& sm : m.submeshes) {
            // Kenshi: the mesh's submesh texture aliases (Ogre M_SUBMESH_TEXTURE_ALIAS: alias -> file) feed the material's texture units; the material script (if any) supplies the rest
            std::vector<std::string> tex; bool has_diffuse = false;
            for (const auto& al : sm.texture_aliases) {
                std::string v = al.second; std::replace(v.begin(), v.end(), '\\', '/'); const size_t sl = v.rfind('/'); tex.push_back(low(sl == std::string::npos ? v : v.substr(sl + 1)));
                if (al.first == "diffuseMap") has_diffuse = true;
                if (alias_seen.size() < 40 && !alias_seen.count(al.first)) alias_seen[al.first] = al.second;
            }
            std::string name = sm.material; int hop = 0; bool found = false;
            while (!name.empty() && hop++ < 8) { auto it = mats.find(name); if (it == mats.end()) break; found = true; if (tex.empty()) tex = it->second.tex; name = it->second.parent; }
            if (!found) { ++mat_missing; if (tex.empty()) problem(p.mesh, "material " + sm.material + " not in any .material and no alias"); }
            if (tex.empty()) { ++mat_notex; continue; }
            if (has_diffuse) ++sub_with_diffuse_alias;
            for (const std::string& t : tex) {
                if (files.count(t)) continue;
                std::string plain = t; const size_t hi = plain.rfind("_hi."); if (hi != std::string::npos) plain.erase(hi, 3);
                if (plain != t && files.count(plain)) { ++tex_hi_fallback; continue; }
                ++tex_missing; problem(p.mesh, "texture " + t + " missing");
            }
        }
    }

    // offset chain (D-010): a part with an offset sits at (a zero-offset part of the same instance: same rotation and scale) + R(q) * offset * scale * 0.1 (rotation as a matrix, independent of the loader's quaternion form)
    int off_checked = 0, off_bad = 0;
    for (const auto& p : parts) {
        if (p.off[0] == 0.f && p.off[1] == 0.f && p.off[2] == 0.f) continue;
        const float n = std::sqrt(p.quat[0] * p.quat[0] + p.quat[1] * p.quat[1] + p.quat[2] * p.quat[2] + p.quat[3] * p.quat[3]);
        const float w = p.quat[0] / n, x = p.quat[1] / n, y = p.quat[2] / n, z = p.quat[3] / n;
        const float R[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) }, { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) }, { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
        float e[3]; for (int r = 0; r < 3; ++r) e[r] = (R[r][0] * p.off[0] + R[r][1] * p.off[1] + R[r][2] * p.off[2]) * 0.1f * p.scale;
        float best = 1e9f;
        for (const auto& b : parts) {
            if (b.off[0] != 0.f || b.off[1] != 0.f || b.off[2] != 0.f) continue;
            if (std::fabs(b.scale - p.scale) > 1e-6f || std::fabs(b.quat[0] - p.quat[0]) + std::fabs(b.quat[1] - p.quat[1]) + std::fabs(b.quat[2] - p.quat[2]) + std::fabs(b.quat[3] - p.quat[3]) > 1e-6f) continue;
            const float d = std::fabs(p.x - b.x - e[0]) + std::fabs(p.y - b.y - e[1]) + std::fabs(p.z - b.z - e[2]);
            if (d < best) best = d;
        }
        ++off_checked; if (best > 0.01f) ++off_bad;
    }
    lua_newtable(L);
#define TSF(k, v) lua_pushnumber(L, (lua_Number)(v)); lua_setfield(L, -2, k);
    TSF("offset_checked", off_checked) TSF("offset_bad", off_bad) TSF("mt0", mt_count[0]) TSF("mt1", mt_count[1]) TSF("mt2", mt_count[2]) TSF("mt3", mt_count[3]) TSF("mt4", mt_count[4]) TSF("tile_not1", tile_not1) TSF("athr_not128", athr_not128) TSF("parts", parts.size()) TSF("parts_no_spec", parts_no_spec) TSF("spec_texture_missing", spec_tex_missing) TSF("meshes", seen_mesh.size()) TSF("bad_rotation", bad_rot) TSF("mesh_unreadable", no_mesh) TSF("material_missing", mat_missing) TSF("material_no_texture", mat_notex)
    TSF("sub_with_diffuse_alias", sub_with_diffuse_alias) TSF("texture_missing", tex_missing) TSF("texture_hi_fallback", tex_hi_fallback) TSF("tilted_over_5deg", tilted) TSF("max_tilt_deg", max_tilt)
#undef TSF
    lua_pushstring(L, first_problems.c_str()); lua_setfield(L, -2, "problems"); lua_pushstring(L, tilted_names.c_str()); lua_setfield(L, -2, "tilted"); lua_pushstring(L, nospec_names.c_str()); lua_setfield(L, -2, "nospec"); { std::string as; for (auto& kv : alias_seen) as += kv.first + "=" + kv.second + "; "; lua_pushstring(L, as.c_str()); lua_setfield(L, -2, "aliases"); }
    return 1;
}

// md.fcs_dump(name_substring, max) -> text: records of the merged install data whose name contains the text: kind, string id, reference lists (targets), files, strings, instance count. Research / sweep helper.
static int l_md_fcs_dump(lua_State* L) {
    static md::fcs::GameDataContainer data; static bool loaded = false;
    if (!loaded) { loaded = true; md::fcs::Install inst(md::KenshiDir()); md::fcs::LoadOrder order; md::fcs::Error e; data.replace_reference_lists = true; if (inst.LoadOrderFor(order, e)) data.Load(order, e); }
    const std::string key = luaL_checkstring(L, 1); const int mx = (int)luaL_optinteger(L, 2, 6); const std::string need_ref = luaL_optstring(L, 3, ""); int n = 0, total = 0; std::string out;
    for (const std::string& id : data.order) {
        const md::fcs::GameData* g = data.Get(id); if (!g) continue;
        if (g->name.find(key) == std::string::npos && g->string_id != key) continue;
        if (!need_ref.empty()) { bool has = false; for (const auto& r : g->references) if (r.first == need_ref && !r.second.empty()) has = true; if (!has) continue; }
        ++total; if (++n > mx) continue;
        out += "[" + std::to_string(g->kind) + "] " + g->name + " (" + g->string_id + ")";
        for (const auto& r : g->references) { out += " ref " + r.first + "={"; for (const auto& t : r.second) out += t.first + ","; out += "}"; }
        for (const auto& f : g->files) out += " file " + f.first + "=" + f.second;
        for (const auto& f : g->strings) out += " str " + f.first + "=" + f.second;
        for (const auto& f : g->ints) out += " int " + f.first + "=" + std::to_string(f.second);
        for (const auto& f : g->floats) out += " flt " + f.first + "=" + std::to_string(f.second);
        for (const auto& f : g->bools) out += " bool " + f.first + "=" + std::to_string((int)f.second);
        out += " instances=" + std::to_string(g->instances.size()) + "\n";
    }
    out += "total=" + std::to_string(total) + "\n";
    lua_pushstring(L, out.c_str()); return 1;
}
extern bool g_town_building_textures, g_town_building_cutout;
static int l_md_set_town_cutout(lua_State* L) { g_town_building_cutout = lua_toboolean(L, 1) != 0; return 0; }   // md.set_town_cutout(on): D-009 2c (zones loaded after the call)
static int l_md_set_town_textures(lua_State* L) { g_town_building_textures = lua_toboolean(L, 1) != 0; return 0; }   // md.set_town_textures(on): D-009 building textures (applies to zones loaded after the call)
static int l_md_town_parts(lua_State* L) {
    std::vector<md::world::PlacedPart> parts; std::string err;
    md::world::LoadBuildingsAt(md::KenshiDir(), (float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), parts, err);
    lua_createtable(L, (int)parts.size(), 0);
    int i = 0;
    for (const auto& p : parts) {
        lua_createtable(L, 0, 10);
        lua_pushstring(L, p.mesh.c_str()); lua_setfield(L, -2, "mesh"); lua_pushstring(L, p.tex.c_str()); lua_setfield(L, -2, "tex"); lua_pushstring(L, p.tex2.c_str()); lua_setfield(L, -2, "tex2"); lua_pushinteger(L, p.material_type); lua_setfield(L, -2, "mtype");
        const char* nm[] = { "x", "y", "z", "y_off", "scale", "qw", "qx", "qy", "qz", "ox", "oy", "oz" };
        const float v[] = { p.x, p.y, p.z, p.y_off, p.scale, p.quat[0], p.quat[1], p.quat[2], p.quat[3], p.off[0], p.off[1], p.off[2] };
        for (int k = 0; k < 12; ++k) { lua_pushnumber(L, v[k]); lua_setfield(L, -2, nm[k]); }
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}
static int l_md_set_nav(lua_State* L) { md::nav::NavWorld::Get().SetEnabled(lua_toboolean(L, 1) != 0, md::KenshiDir()); return 0; }
static int l_md_nav_path(lua_State* L) {
    float pts[256 * 3]; int n = 0; bool complete = false, busy = false;
    if (!md::nav::NavWorld::Get().FindPath((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4), pts, 256, &n, &complete, &busy)) { lua_pushnil(L); if (busy) { lua_pushstring(L, "busy"); return 2; } return 1; }
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) { lua_createtable(L, 3, 0); for (int k = 0; k < 3; ++k) { lua_pushnumber(L, pts[i * 3 + k]); lua_rawseti(L, -2, k + 1); } lua_rawseti(L, -2, i + 1); }
    lua_pushboolean(L, complete);
    return 2;
}
static int l_md_nav_stats(lua_State* L) {
    const auto& st = md::nav::NavWorld::Get().GetStats();
    lua_newtable(L);
    auto set = [&](const char* k, double v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); };
    set("tiles_live", st.tiles_live); set("tiles_requested", st.tiles_requested); set("loaded_total", (double)st.loaded_total); set("cache_hits", (double)st.cache_hits);
    set("last_add_ms", st.last_add_ms); set("max_add_ms", st.max_add_ms); set("last_worker_ms", st.last_worker_ms); set("max_worker_ms", st.max_worker_ms); set("max_tick_ms", st.max_tick_ms);
    return 1;
}
static int l_md_set_town_buildings(lua_State* L) { TownBuildings::Get().SetEnabled(lua_toboolean(L, 1) != 0, (float)luaL_optnumber(L, 2, 300.0)); return 0; }   // #119 step 2: (on, range m)
extern int g_kenshi_sky;
extern int g_kenshi_water;
static int l_md_set_kenshi_water(lua_State* L) { g_kenshi_water = (int)luaL_checkinteger(L, 1); return 0; }   // #173: 0 old water, 1 Kenshi water
static int l_md_set_kenshi_sky(lua_State* L) { g_kenshi_sky = (int)luaL_checkinteger(L, 1); return 0; }   // #173: 0 old gradient, 1 SkyX
static int l_md_set_town_foliage(lua_State* L) { TownFoliage::Get().SetEnabled(lua_toboolean(L, 1) != 0, (float)luaL_optnumber(L, 2, 100.0)); return 0; }   // #120 step 2: (on, range m)
static int l_md_set_town_grass(lua_State* L) { TownFoliage::Get().SetGrass(lua_toboolean(L, 1) != 0, (float)luaL_optnumber(L, 2, 50.0), (float)luaL_optnumber(L, 3, 1.0)); return 0; }   // #120: (on, range m, density x), after set_town_foliage(true)
static int l_md_town_foliage_stats(lua_State* L) { TownFoliage::Stats s = TownFoliage::Get().stats(); lua_newtable(L);
#define TFF(k, v) lua_pushnumber(L, (lua_Number)(v)); lua_setfield(L, -2, k);
    TFF("zones", s.zones) TFF("loading", s.loading) TFF("requested", s.requested) TFF("instances", s.instances) TFF("draws", s.draws) TFF("batches", s.batches) TFF("triangles", s.triangles) TFF("loaded_total", s.loaded_total) TFF("max_step_ms", s.max_step_ms) TFF("grass_blades", s.grass_blades) TFF("grass_gen_ms", s.grass_gen_ms)
#undef TFF
    return 1; }
static int l_md_town_buildings_stats(lua_State* L) { TownBuildings::Stats s = TownBuildings::Get().stats(); lua_newtable(L);
#define TBF(k, v) lua_pushnumber(L, (lua_Number)(v)); lua_setfield(L, -2, k);
    TBF("zones", s.zones) TBF("loading", s.loading) TBF("requested", s.requested) TBF("instances", s.instances) TBF("draws", s.draws) TBF("batches", s.batches) TBF("textured_batches", s.textured_batches) TBF("no_texture_batches", s.no_texture_batches) TBF("triangles", s.triangles) TBF("loaded_total", s.loaded_total) TBF("unloaded_total", s.unloaded_total) TBF("max_step_ms", s.max_step_ms)
#undef TBF
    return 1; }
extern void md_frame_counters(md::FrameCounters* out);   // game/src/render/frame_counters.cpp: the same rows F3 -> Debug shows
static int l_md_frame_counters(lua_State* L) {   // G3: md.frame_counters() -> { {name, draws, instances, triangles}, ... } (the category rows, no total)
    md::FrameCounters fc; md_frame_counters(&fc); lua_newtable(L);
    for (int i = 0; i < fc.count; ++i) {
        lua_newtable(L);
        lua_pushstring(L, fc.rows[i].name); lua_setfield(L, -2, "name");
        lua_pushinteger(L, fc.rows[i].draws); lua_setfield(L, -2, "draws");
        lua_pushinteger(L, fc.rows[i].instances); lua_setfield(L, -2, "instances");
        lua_pushnumber(L, (lua_Number)fc.rows[i].triangles); lua_setfield(L, -2, "triangles");
        lua_rawseti(L, -2, i + 1);
    }
    return 1; }
static int l_md_hub_spike_stats(lua_State* L) { HubSpike::Stats s = HubSpike::Slot((int)luaL_checkinteger(L, 1)).stats(); lua_newtable(L); lua_pushinteger(L, s.instances); lua_setfield(L, -2, "instances"); lua_pushinteger(L, s.draws); lua_setfield(L, -2, "draws"); lua_pushinteger(L, s.batches); lua_setfield(L, -2, "batches"); lua_pushinteger(L, (lua_Integer)s.triangles); lua_setfield(L, -2, "triangles"); lua_pushinteger(L, s.occluded); lua_setfield(L, -2, "occluded"); return 1; }
float g_feature_terrain_radius = 1500.f;
static int l_md_set_feature_terrain(lua_State* L) { FeatureScatterSystem::Get().SetTerrainInGbuffer(lua_toboolean(L, 1) != 0); g_feature_terrain_radius = (float)luaL_optnumber(L, 2, 1500.0); return 0; }   // #176: TERRAIN-mode map features into the terrain G-buffer (default off), draw radius m
extern bool g_opt_fxaa;
extern bool g_opt_sgsr1;
static int l_md_set_sgsr1(lua_State* L) { g_opt_sgsr1 = lua_toboolean(L, 1) != 0; return 0; }   // #198 S1: Snapdragon GSR 1 upscale instead of bilinear + sharpen (scene below native only), default off
static int l_md_set_fxaa(lua_State* L) { g_opt_fxaa = lua_toboolean(L, 1) != 0; return 0; }   // #180: FXAA 3.11 (Kenshi's parameters) as the last pass, default off
extern bool g_dynres_on; extern float g_dynres_scale, g_dynres_cur, g_dynres_v_move, g_dynres_w_move, g_dynres_v_stop, g_dynres_w_stop; extern int g_dynres_down, g_dynres_up;
// md.set_dyn_res(on, scale[, down_frames, up_frames]) -- #182, default on (owner 17.10); md.dyn_res_state() -> current scale
static int l_md_set_dyn_res(lua_State* L) {
    g_dynres_on = lua_toboolean(L, 1) != 0;
    if (lua_isnumber(L, 2)) g_dynres_scale = (float)lua_tonumber(L, 2);
    if (lua_isnumber(L, 3)) g_dynres_down = (int)lua_tonumber(L, 3);
    if (lua_isnumber(L, 4)) g_dynres_up = (int)lua_tonumber(L, 4);
    return 0;
}
static int l_md_wall_ms(lua_State* L) { lua_pushnumber(L, (double)SDL_GetTicksNS() * 1e-6); return 1; }   // md.wall_ms(): monotonic wall clock in ms (scenario Lua has no os / io): per-frame wall time of a flight
static int l_md_dyn_res_state(lua_State* L) { lua_pushnumber(L, g_dynres_cur); return 1; }
static int l_md_push_key(lua_State* L) { SDL_Event ev = {}; ev.type = SDL_EVENT_KEY_DOWN; ev.key.scancode = (SDL_Scancode)luaL_checkinteger(L, 1); ev.key.down = true; SDL_PushEvent(&ev); ev.type = SDL_EVENT_KEY_UP; ev.key.down = false; SDL_PushEvent(&ev); return 0; }   // md.push_key(scancode): a synthetic key press (dynres input oracle)
extern float g_dynres_ema; extern int g_dynres_hold; extern long g_dynres_changes;
static int l_md_set_horizon_cull(lua_State* L) { md::HorizonCullSet(lua_toboolean(L, 1) != 0); return 0; }   // md.set_horizon_cull(on): CPU horizon map drops terrain nodes hidden behind terrain (#207); default off
static int l_md_horizon_cull_stats(lua_State* L) { const md::HorizonCullStats s = md::HorizonCullGetStats(); lua_pushinteger(L, s.kept); lua_pushinteger(L, s.dropped); lua_pushnumber(L, s.filter_ms); lua_pushnumber(L, s.build_ms); return 4; }   // md.horizon_cull_stats() -> kept, dropped, render-thread filter ms, worker table build ms (last frame)
static int l_md_set_dyn_res_damping(lua_State* L) { g_dynres_ema = std::max(0.f, std::min(0.99f, (float)luaL_optnumber(L, 1, 0.0))); g_dynres_hold = std::max(0, (int)luaL_optinteger(L, 2, 0)); return 0; }   // md.set_dyn_res_damping(ema, hold_frames): EMA factor of the camera speed / turn signals (0 = off) and the frames a level change holds (0 = off); defaults off
static int l_md_dyn_res_changes(lua_State* L) { lua_pushinteger(L, (lua_Integer)g_dynres_changes); if (lua_toboolean(L, 1)) g_dynres_changes = 0; return 1; }   // md.dyn_res_changes([reset]) -> number of level changes since the start (or the last reset)
static int l_md_set_gpu_frames_in_flight(lua_State* L) { lua_pushboolean(L, SDL_SetGPUAllowedFramesInFlight(md::GpuDevice::Get().SDLDevice(), (Uint32)luaL_checkinteger(L, 1))); return 1; }   // experiment: 1 / 2 / 3
static int l_md_set_opt_ground_reuse_cr(lua_State* L) { g_opt_ground_reuse_cr = lua_isboolean(L, 1) ? (lua_toboolean(L, 1) ? 1 : 0) : (int)luaL_checkinteger(L, 1); return 0; }   // #163 reopened: false/0 nearest, true/1 Catmull-Rom, 2 + live-only history
extern int g_ground_variant;   // engine/src/render/terrain_shading_projected.cpp
extern bool g_ground_pvt, g_ground_hist_fast;
static int l_md_set_ground_hist_fast(lua_State* L) { g_ground_hist_fast = luaL_checkinteger(L, 1) != 0; return 0; }   // md.set_ground_history_fast(0 Quality / 1 Fast): Settings «History filter while moving»
static int l_md_set_ground_pvt(lua_State* L) { g_ground_pvt = luaL_checkinteger(L, 1) != 0; return 0; }   // md.set_ground_cache_pvt(0 Off / 1 On): Settings «Ground cache (PVT)»
static int l_md_set_ground_variant(lua_State* L) { g_ground_variant = (int)luaL_checkinteger(L, 1); return 0; }   // measurement (#207 variant_eval): the pass-B variant of $MD_SPV_VARIANTS (0..n-1), -1 = the shipped one
static int l_md_set_ground_live_switch(lua_State* L) {   // md.set_ground_live_switch("origlight"|"tiny"|"groups", value): run-time arm switch (env var = initial value); MD_GROUND_LIVE / _LIGHT stay start-up only
    const std::string n = luaL_checkstring(L, 1); const int v = (int)luaL_checkinteger(L, 2); auto& r = md::GroundLiveRtState();
    if (n == "origlight") r.orig_light = v ? 1 : 0; else if (n == "tiny") r.tiny = v ? 1 : 0; else if (n == "groups") r.groups = std::max(1, std::min(31, v)); else return luaL_error(L, "unknown ground live switch '%s'", n.c_str());
    return 0; }
static int l_md_set_ground_reuse_period(lua_State* L) { md_settings_set_ground_reuse_period((int)luaL_checkinteger(L, 1)); return 0; }   // #191 stage 6: 4 (default) / 8 / 16
static int l_md_set_ground_reuse_chain(lua_State* L) { md_settings_set_ground_reuse_chain((int)luaL_checkinteger(L, 1)); return 0; }   // #207: 0 standard, 3, 5
static int l_md_set_opt_ground_reuse(lua_State* L) { g_opt_ground_reuse = lua_toboolean(L, 1) != 0; return 0; }   // #163 5b-2b: needs md.set_opt_ground_split(true)
static int l_md_ground_reuse_reset(lua_State*) { SceneRender::Get().gr_reset_ = true; return 0; }   // #163
static int l_md_set_opt_force_upscale(lua_State* L) { g_opt_force_upscale = lua_toboolean(L, 1) != 0; return 0; }
extern float g_speed_override;
static int l_md_set_speed(lua_State* L) { g_speed_override = (float)luaL_optnumber(L, 1, -1.0); return 0; }   // md.set_speed(v): the camera speed (m/s) C15 and the ring switch see, set by the scenario; no argument = measured
// md.gpu_live_resources(): live counts of the GPU resources created through the HAL wrapper classes (GpuResourceTracker; buffers, textures, pipelines, compute pipelines, depth textures) -- leak check across zone reloads (#185 step 4.5)
void TransitionToZone(const char* zone_slug, float player_x, float player_z);   // game/src/zone_transition.cpp
// md.travel_zone(i) -> name, exit_x, exit_z or nil: the explicit zone transition of the world map (hot-reloads the terrain chunks of zone record i among the records with a grid position); the
// caller teleports the player to the returned exit position. Diagnostic for the zone-reload leak check (#185 step 4.5). TransitionToZone saves game/data/world_registry.txt; the file is put back afterwards (a scenario run must not change it -- the gate `frames` depends on its current_zone).
static int l_md_travel_zone(lua_State* L) {
    int want = (int)luaL_checkinteger(L, 1), n = 0, idx = 0;
    ZoneRecord* z = WorldRegistry::Get().GetAll(n);
    for (int i = 0; i < n; ++i) {
        if (z[i].grid_x <= 0) continue;
        if (idx++ != want) continue;
        const float ex = z[i].exit_x, ez = z[i].exit_z;
        const char* kRegPath = getenv("MD_WORLD_REGISTRY") ? getenv("MD_WORLD_REGISTRY") : "game/data/world_registry.txt";
        char saved[16384]; size_t saved_n = 0; bool existed = false, fits = true;
        if (FILE* f = fopen(kRegPath, "rb")) { existed = true; saved_n = fread(saved, 1, sizeof saved, f); fits = saved_n < sizeof saved; fclose(f); }
        TransitionToZone(z[i].name, 0.f, 0.f);
        if (!existed) remove(kRegPath);
        else if (fits) { if (FILE* f = fopen(kRegPath, "wb")) { fwrite(saved, 1, saved_n, f); fclose(f); } }
        lua_pushstring(L, z[i].name); lua_pushnumber(L, ex); lua_pushnumber(L, ez);
        return 3;
    }
    return 0;
}
static int l_md_gpu_live_resources(lua_State* L) {
    auto& t = md::GpuResourceTracker::Get();
    lua_newtable(L);
    lua_pushinteger(L, t.LiveBuffers());          lua_setfield(L, -2, "buffers");
    lua_pushinteger(L, t.LiveTextures());         lua_setfield(L, -2, "textures");
    lua_pushinteger(L, t.LivePipelines());        lua_setfield(L, -2, "pipelines");
    lua_pushinteger(L, t.LiveComputePipelines()); lua_setfield(L, -2, "compute_pipelines");
    lua_pushinteger(L, t.LiveDepthTextures());    lua_setfield(L, -2, "depth_textures");
    lua_pushinteger(L, t.LiveTotal());            lua_setfield(L, -2, "total");
    long rss_kb = -1;
    if (FILE* f = fopen("/proc/self/status", "r")) {   // process resident set, KB (the scenario sandbox has no io library)
        char line[256];
        while (fgets(line, sizeof line, f)) if (sscanf(line, "VmRSS: %ld", &rss_kb) == 1) break;
        fclose(f);
    }
    lua_pushinteger(L, rss_kb);                   lua_setfield(L, -2, "rss_kb");
    return 1;
}
static int l_md_set_terrain_lod_patch(lua_State* L) { g_terrain_lod_patch = (lua_tointeger(L, 1) == 64) ? 64 : 16; return 0; }
static int l_md_set_terrain_lod_params(lua_State* L) {
    g_terrain_lod_near_px = (float)luaL_checknumber(L, 1); g_terrain_lod_far_px = (float)luaL_checknumber(L, 2);
    g_terrain_lod_near_dist = (float)luaL_checknumber(L, 3); g_terrain_lod_far_dist = (float)luaL_checknumber(L, 4);
    return 0;
}

// md.set_terrain_light_clamp(on): diagnostic. false removes min(light, 1) in TS_ApplyLighting; default true.
static int l_md_set_terrain_light_clamp(lua_State* L) {
    g_terrain_light_clamp = lua_toboolean(L, 1) != 0;
    return 0;
}

// md.set_terrain_per_layer_mip(on): zone-seam fix (default on); false = the old home-zone ground mip and home-only cliff distortion, for A/B.
static int l_md_set_terrain_per_layer_mip(lua_State* L) {
    g_terrain_per_layer_mip = lua_toboolean(L, 1) != 0;
    return 0;
}

// md.set_terrain_ambientmap(on): divergence #14 -- Kenshi ambientmap.png sun/ambient multipliers in the resolve (default off).
static int l_md_set_terrain_ambientmap(lua_State* L) {
    g_terrain_ambientmap = lua_toboolean(L, 1) != 0;
    return 0;
}

void RegisterLuaScenarioAPI(float* cam_x, float* cam_z) {
    s_cam_x = cam_x;
    s_cam_z = cam_z;
    s_scenario_tick = 0;

    auto& sys = LuaSystem::Get();
    sys.RegisterNamespaceFunction("rand",               l_md_rand);
    sys.RegisterNamespaceFunction("teleport_camera",     l_md_teleport_camera);
    sys.RegisterNamespaceFunction("teleport_player",     l_md_teleport_player);
    sys.RegisterNamespaceFunction("terrain_height",      l_md_terrain_height);
    sys.RegisterNamespaceFunction("ray_ground",          l_md_ray_ground);
    sys.RegisterNamespaceFunction("pvt_sample",          l_md_pvt_sample);
    sys.RegisterNamespaceFunction("chunk_stats",         l_md_chunk_stats);
    sys.RegisterNamespaceFunction("zone_info",           l_md_zone_info);
    sys.RegisterNamespaceFunction("world_to_local",      l_md_world_to_local);
    sys.RegisterNamespaceFunction("get_camera_pose",     l_md_get_camera_pose);
    sys.RegisterNamespaceFunction("get_player_pos",      l_md_get_player_pos);
    sys.RegisterNamespaceFunction("probe_world_heightmap", l_md_probe_world_heightmap);
    sys.RegisterNamespaceFunction("granite_terrain_stats", l_md_granite_terrain_stats);
    sys.RegisterNamespaceFunction("pvt_flush", l_md_pvt_flush);
    sys.RegisterNamespaceFunction("gpu_act_freq", l_md_gpu_act_freq);
    sys.RegisterNamespaceFunction("terrain_pixel_fraction", l_md_terrain_pixel_fraction);
    sys.RegisterNamespaceFunction("dump_depth", l_md_dump_depth);
    sys.RegisterNamespaceFunction("ground_reuse_stats", l_md_ground_reuse_stats);
    sys.RegisterNamespaceFunction("splash_shot", l_md_splash_shot);
    sys.RegisterNamespaceFunction("ground_reuse_dump", l_md_ground_reuse_dump);
    sys.RegisterNamespaceFunction("debug_open_map",      l_md_debug_open_map);
    sys.RegisterNamespaceFunction("scan_terrain_gaps",   l_md_scan_terrain_gaps);
    sys.RegisterNamespaceFunction("scan_terrain_seam_tjunctions", l_md_scan_terrain_seam_tjunctions);
    sys.RegisterNamespaceFunction("assert_eq",           l_md_assert_eq);
    sys.RegisterNamespaceFunction("assert_true",         l_md_assert_true);
    sys.RegisterNamespaceFunction("assert_above_terrain",l_md_assert_above_terrain);
    sys.RegisterNamespaceFunction("screenshot",          l_md_screenshot);
    sys.RegisterNamespaceFunction("bugcap_start",        l_md_bugcap_start);
    sys.RegisterNamespaceFunction("bugcap_stop",         l_md_bugcap_stop);
    sys.RegisterNamespaceFunction("render_frame",        l_md_render_frame);
    sys.RegisterNamespaceFunction("clock_probe",         l_md_clock_probe);
    sys.RegisterNamespaceFunction("gpu_freq_mhz",        l_md_gpu_freq_mhz);
    sys.RegisterNamespaceFunction("pkg_temp_c",          l_md_pkg_temp_c);
    sys.RegisterNamespaceFunction("place_window_on_largest_display", l_md_place_window_on_largest_display);
    sys.RegisterNamespaceFunction("proc_mem_kb",         l_md_proc_mem_kb);
    sys.RegisterNamespaceFunction("render_burst",        l_md_render_burst);
    sys.RegisterNamespaceFunction("render_burst_ms",     l_md_render_burst_ms);
    sys.RegisterNamespaceFunction("render_burst_freq",   l_md_render_burst_freq);
    sys.RegisterNamespaceFunction("set_burst_paced",     l_md_set_burst_paced);
    sys.RegisterNamespaceFunction("render_burst_cpu_ms", l_md_render_burst_cpu_ms);
    sys.RegisterNamespaceFunction("render_burst_path", l_md_render_burst_path);
    sys.RegisterNamespaceFunction("render_burst_dynres", l_md_render_burst_dynres);
    sys.RegisterNamespaceFunction("render_burst_stream_ms", l_md_render_burst_stream_ms);
    sys.RegisterNamespaceFunction("render_burst_stream", l_md_render_burst_stream);
    sys.RegisterNamespaceFunction("renderdoc_capture",   l_md_renderdoc_capture);
    sys.RegisterNamespaceFunction("dump_frame_stats",    l_md_dump_frame_stats);
    sys.RegisterNamespaceFunction("set_camera_pose",     l_md_set_camera_pose);
    sys.RegisterNamespaceFunction("set_camera_pose_world", l_md_set_camera_pose_world);
    sys.RegisterNamespaceFunction("set_camera_fov", l_md_set_camera_fov);
    sys.RegisterNamespaceFunction("set_camera_mode",     l_md_set_camera_mode);
    sys.RegisterNamespaceFunction("dump_state",          l_md_dump_state);
    sys.RegisterNamespaceFunction("state_hash",          l_md_state_hash);
    sys.RegisterNamespaceFunction("state_trace",         l_md_state_trace);
    sys.RegisterNamespaceFunction("log",                 l_md_log);
    sys.RegisterNamespaceFunction("set_vsync",           l_md_set_vsync);
    sys.RegisterNamespaceFunction("set_fps_cap",         l_md_set_fps_cap);
    sys.RegisterNamespaceFunction("set_view_distance",   l_md_set_view_distance);
    sys.RegisterNamespaceFunction("set_window_size",     l_md_set_window_size);
    sys.RegisterNamespaceFunction("get_frame_ms",        l_md_get_frame_ms);
    sys.RegisterNamespaceFunction("set_gpu_sync_timing", l_md_set_gpu_sync_timing);
    sys.RegisterNamespaceFunction("get_gpu_ms",          l_md_get_gpu_ms);
    sys.RegisterNamespaceFunction("gpu_poll",            l_md_gpu_poll);
    sys.RegisterNamespaceFunction("set_terrain_resolve_repeat", l_md_set_terrain_resolve_repeat);
    sys.RegisterNamespaceFunction("set_time_of_day", l_md_set_time_of_day);
    sys.RegisterNamespaceFunction("light_tick", l_md_light_tick);
    sys.RegisterNamespaceFunction("set_weather", l_md_set_weather);
    sys.RegisterNamespaceFunction("kenshi_sky", l_md_kenshi_sky);
    sys.RegisterNamespaceFunction("light_sun_dir", l_md_light_sun_dir);
    sys.RegisterNamespaceFunction("adapt_lum_debug", l_md_adapt_lum_debug);
    sys.RegisterNamespaceFunction("adapt_lum_read", l_md_adapt_lum_read);
    sys.RegisterNamespaceFunction("set_kenshi_lighting", l_md_set_kenshi_lighting);
    sys.RegisterNamespaceFunction("set_resolution_scale", l_md_set_resolution_scale);
    sys.RegisterNamespaceFunction("set_terrain_wireframe", l_md_set_terrain_wireframe);
    sys.RegisterNamespaceFunction("set_editor_panels_hidden", l_md_set_editor_panels_hidden);
    sys.RegisterNamespaceFunction("set_npc_prepass_disabled", l_md_set_npc_prepass_disabled);
    sys.RegisterNamespaceFunction("set_render_pass_enabled", l_md_set_render_pass_enabled);
    sys.RegisterNamespaceFunction("set_terrain_cliff_normal_flip", l_md_set_terrain_cliff_normal_flip);
    sys.RegisterNamespaceFunction("set_terrain_detail_n_atten", l_md_set_terrain_detail_n_atten);
    sys.RegisterNamespaceFunction("set_terrain_ground_aniso", l_md_set_terrain_ground_aniso);
    sys.RegisterNamespaceFunction("set_terrain_ground_min_lod", l_md_set_terrain_ground_min_lod);
    sys.RegisterNamespaceFunction("set_terrain_lod", l_md_set_terrain_lod);
    sys.RegisterNamespaceFunction("set_terrain_lod_params", l_md_set_terrain_lod_params);
    sys.RegisterNamespaceFunction("set_terrain_lod_patch", l_md_set_terrain_lod_patch);
    sys.RegisterNamespaceFunction("set_opt_skip_flat_on_cliff", l_md_set_opt_skip_flat_on_cliff);
    sys.RegisterNamespaceFunction("set_opt_ground_split", l_md_set_opt_ground_split);
    sys.RegisterNamespaceFunction("set_opt_ground_reuse_cr", l_md_set_opt_ground_reuse_cr);
    sys.RegisterNamespaceFunction("set_ground_reuse_period", l_md_set_ground_reuse_period);
    sys.RegisterNamespaceFunction("set_ground_live_switch", l_md_set_ground_live_switch);
    sys.RegisterNamespaceFunction("set_ground_variant", l_md_set_ground_variant);
    sys.RegisterNamespaceFunction("set_ground_history_fast", l_md_set_ground_hist_fast);
    sys.RegisterNamespaceFunction("set_ground_cache_pvt", l_md_set_ground_pvt);
    sys.RegisterNamespaceFunction("set_ground_reuse_chain", l_md_set_ground_reuse_chain);
    sys.RegisterNamespaceFunction("set_hub_spike", l_md_set_hub_spike);
    sys.RegisterNamespaceFunction("set_hub_zone", l_md_set_hub_zone);
    sys.RegisterNamespaceFunction("set_town_buildings", l_md_set_town_buildings);
    sys.RegisterNamespaceFunction("set_nav", l_md_set_nav);
    sys.RegisterNamespaceFunction("nav_path", l_md_nav_path);
    sys.RegisterNamespaceFunction("nav_stats", l_md_nav_stats);
    sys.RegisterNamespaceFunction("set_town_foliage", l_md_set_town_foliage);
    sys.RegisterNamespaceFunction("set_kenshi_sky", l_md_set_kenshi_sky);
    sys.RegisterNamespaceFunction("set_kenshi_water", l_md_set_kenshi_water);
    sys.RegisterNamespaceFunction("set_town_grass", l_md_set_town_grass);
    sys.RegisterNamespaceFunction("town_foliage_stats", l_md_town_foliage_stats);
    sys.RegisterNamespaceFunction("set_prop_cull", l_md_set_prop_cull);
    sys.RegisterNamespaceFunction("set_ground_preset", l_md_set_ground_preset);
    sys.RegisterNamespaceFunction("town_parts", l_md_town_parts);
    sys.RegisterNamespaceFunction("town_sweep", l_md_town_sweep);
    sys.RegisterNamespaceFunction("set_town_textures", l_md_set_town_textures);
    sys.RegisterNamespaceFunction("set_town_cutout", l_md_set_town_cutout);
    sys.RegisterNamespaceFunction("fcs_dump", l_md_fcs_dump);
    sys.RegisterNamespaceFunction("town_buildings_stats", l_md_town_buildings_stats);
    sys.RegisterNamespaceFunction("set_feature_terrain", l_md_set_feature_terrain);
    sys.RegisterNamespaceFunction("hub_spike_stats", l_md_hub_spike_stats);
    sys.RegisterNamespaceFunction("frame_counters", l_md_frame_counters);
    sys.RegisterNamespaceFunction("set_hub_spike_occlusion", l_md_set_hub_spike_occlusion);
    sys.RegisterNamespaceFunction("set_hub_spike_instanced", l_md_set_hub_spike_instanced);
    sys.RegisterNamespaceFunction("set_gpu_frames_in_flight", l_md_set_gpu_frames_in_flight);
    sys.RegisterNamespaceFunction("set_upscale_sharpness", l_md_set_up_sharp);
    sys.RegisterNamespaceFunction("set_opt_ground_reuse", l_md_set_opt_ground_reuse);
    sys.RegisterNamespaceFunction("ground_reuse_reset", l_md_ground_reuse_reset);
    sys.RegisterNamespaceFunction("set_opt_force_upscale", l_md_set_opt_force_upscale);
    sys.RegisterNamespaceFunction("set_speed", l_md_set_speed);
    sys.RegisterNamespaceFunction("set_fxaa", l_md_set_fxaa);
    sys.RegisterNamespaceFunction("set_sgsr1", l_md_set_sgsr1);
    sys.RegisterNamespaceFunction("set_dyn_res", l_md_set_dyn_res);
    sys.RegisterNamespaceFunction("dyn_res_state", l_md_dyn_res_state);
    sys.RegisterNamespaceFunction("push_key", l_md_push_key);
    sys.RegisterNamespaceFunction("set_dyn_res_damping", l_md_set_dyn_res_damping);
    sys.RegisterNamespaceFunction("set_horizon_cull", l_md_set_horizon_cull);
    sys.RegisterNamespaceFunction("horizon_cull_stats", l_md_horizon_cull_stats);
    sys.RegisterNamespaceFunction("dyn_res_changes", l_md_dyn_res_changes);
    sys.RegisterNamespaceFunction("wall_ms", l_md_wall_ms);
    sys.RegisterNamespaceFunction("gpu_live_resources", l_md_gpu_live_resources);
    sys.RegisterNamespaceFunction("travel_zone", l_md_travel_zone);
    sys.RegisterNamespaceFunction("set_terrain_light_clamp", l_md_set_terrain_light_clamp);
    sys.RegisterNamespaceFunction("set_terrain_per_layer_mip", l_md_set_terrain_per_layer_mip);
    sys.RegisterNamespaceFunction("set_terrain_ambientmap", l_md_set_terrain_ambientmap);
    sys.RegisterNamespaceFunction("set_editor_open",     l_md_set_editor_open);
    sys.RegisterNamespaceFunction("set_camera_orbit",    l_md_set_camera_orbit);

    // Alias the 17 pre-existing flat md_foo() functions into md.* under the
    // SAME name (md.get_pos = md_get_pos, ...) — Etap 1 API-convention rule:
    // new functions live ONLY in md.*, old ones stay reachable both ways.
    static const char* const kFlatNames[] = {
        "get_pos", "set_target", "get_health", "get_faction",
        "on_event", "fire_event", "quest_active", "quest_complete",
        "play_sfx", "play_music", "stop_music", "play_ambience", "stop_ambience",
        "set_volume", "assign_power", "use_power", "get_power_cooldown",
    };
    char alias_src[2048] = {};
    int off = snprintf(alias_src, sizeof(alias_src), "md = md or {}\n");
    for (const char* name : kFlatNames) {
        off += snprintf(alias_src + off, sizeof(alias_src) - off,
                         "if md_%s then md.%s = md_%s end\n", name, name, name);
    }
    sys.Exec(alias_src);
}

// terrain-perf-measure (2026-08-13): pointer wiring ONLY, separate from
// RegisterLuaScenarioAPI's function registration above -- called after
// g_cam_az/el/dist are declared in main()'s scope (they don't exist yet
// when InitGame() -> RegisterLuaScenarioAPI(&cam_x,&cam_z) runs, see
// s_cam_az's doc comment in lua_scenario_api_internal.h). The Lua function
// itself is already registered (md.set_camera_orbit is callable as soon as
// the game starts); it's just a no-op (s_cam_az still null) until this
// runs, same as s_cam_x/z's window between RegisterLuaScenarioAPI and
// InitGame() finishing. Lets a perf-measure script deterministically orient
// the release build's fixed third-person orbit camera instead of depending
// on luck (whatever direction the default az=0/south orbit happens to face
// from a given teleport_player(x,z) target).
void RegisterLuaScenarioOrbitCamera(float* az, float* el, float* dist) {
    s_cam_az = az;
    s_cam_el = el;
    s_cam_dist = dist;
}
