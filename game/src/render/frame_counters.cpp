// F3 -> Debug "Counters by category" (G3 cheap half, coordinator 6720915651): sums the per-system stats that already exist.
#include <monkey_dust/render/frame_counters.h>
#include "render/scene_render.h"
#include "render/hub_spike.h"
#include "render/town_buildings.h"
#include "render/town_foliage.h"

extern bool g_terrain_lod;

// Filled once per call from the stats the systems keep; the editor draws the result. NPC is not counted yet (no per-frame draw / triangle counter exists in the NPC renderer).
void md_frame_counters(md::FrameCounters* out) {
    const TownBuildings::Stats tb = TownBuildings::Get().stats();
    out->Add("town buildings", tb.draws, tb.instances, tb.triangles);
    const TownFoliage::Stats tf = TownFoliage::Get().stats();
    out->Add("town foliage", tf.draws, tf.instances, tf.triangles);
    int pd = 0, pi = 0; int64_t pt = 0;
    for (int i = 0; i < HubSpike::SLOTS; ++i) { const HubSpike::Stats s = HubSpike::Slot(i).stats(); pd += s.draws; pi += s.instances; pt += s.triangles; }
    out->Add("props (hub)", pd, pi, pt);
    auto& sr = SceneRender::Get();
    const int nodes = sr.terrain_quadtree_last_visible_count_;
    const int tpn = g_terrain_lod && sr.terrain_quadtree_renderer_.IsLodReady() ? sr.terrain_quadtree_renderer_.LodTrianglesPerNode() : sr.terrain_quadtree_renderer_.FilledTrianglesPerNode();
    out->Add("terrain", nodes, nodes, (int64_t)nodes * tpn);
}
