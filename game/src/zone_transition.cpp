#include "world/world_registry.h"
#include <monkey_dust/platform/audio.h>
#include "render/scene_render.h"
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/world/terrain_query.h>
#include <monkey_dust/physics/physics_terrain_region.h>
#include <cstring>
#include <cstdio>

// ── Zone Transition ───────────────────────────────────────────────────────────
// Hot-reload terrain chunks for a new Kenshi zone without restarting.
// Call from game loop — regenerates all 256 chunks in-place.
void TransitionToZone(const char* zone_slug, float player_x, float player_z) {
    auto& wr = WorldRegistry::Get();

    ZoneRecord* old = wr.Find(wr.CurrentZone());
    if (old) { old->exit_x = player_x; old->exit_z = player_z; }

    wr.SetCurrentZone(zone_slug);
    ZoneRecord* nz = wr.GetOrCreate(zone_slug);
    if (nz && nz->biome[0] == '\0') {
        const char* b = wr.GetBiomeAt(nz->grid_x, nz->grid_z);
        strncpy(nz->biome, b, sizeof(nz->biome) - 1);
    }

    // W-4: ambient audio — biome keyword → ambient file
    {
        const char* biome = nz ? nz->biome : "desert";
        const char* amb_path = nullptr;
        if      (strstr(biome, "snow")    || strstr(biome, "ice"))    amb_path = "data/audio/ambient/wind_cold.ogg";
        else if (strstr(biome, "desert")  || strstr(biome, "arid"))   amb_path = "data/audio/ambient/desert_wind.ogg";
        else if (strstr(biome, "swamp")   || strstr(biome, "wetland")) amb_path = "data/audio/ambient/swamp_insects.ogg";
        else if (strstr(biome, "forest")  || strstr(biome, "green"))   amb_path = "data/audio/ambient/forest_birds.ogg";
        else if (strstr(biome, "canyon")  || strstr(biome, "rock"))    amb_path = "data/audio/ambient/canyon_wind.ogg";
        else                                                             amb_path = "data/audio/ambient/scrubland_wind.ogg";
        AudioSystem::Get().PlayAmbience(amb_path, 0.35f);
    }

    if (nz) nz->visited = true;
    wr.Save();

    if (nz && nz->grid_x > 0) {
        SceneRender::Get().zone_ox = nz->grid_x - SceneRender::TNKN / 2;
        SceneRender::Get().zone_oz = nz->grid_z - SceneRender::TNKN / 2;
    }

    TerrainGenParams tgp = TerrainGenParams::ProcWorld();
    tgp.nav_cs         = 50.0f;
    tgp.nav_ch         = 1.0f;
    tgp.world_offset_x = SceneRender::Get().tnoff_x;
    tgp.world_offset_z = SceneRender::Get().tnoff_z;
    tgp.force_noise    = false;
    tgp.zone_origin_x  = SceneRender::Get().zone_ox;
    tgp.zone_origin_z  = SceneRender::Get().zone_oz;

    SceneRender::Get().base_cx = 0; SceneRender::Get().base_cz = 0;
    for (int cz = 0; cz < SceneRender::TNKN; ++cz)
        for (int cx = 0; cx < SceneRender::TNKN; ++cx) {
            TerrainGen_Build(SceneRender::Get().terrain_chunks[cz][cx], {cx, cz}, tgp);
            TerrainGen_Upload(SceneRender::Get().terrain_chunks[cz][cx]);
        }

    TerrainQuery::Get().Init(&SceneRender::Get().terrain_chunks[0][0],
                             SceneRender::TNKN, CHUNK_SIZE,
                             SceneRender::Get().tnoff_x, SceneRender::Get().tnoff_z, 0, 0,
                             SceneRender::Get().zone_ox, SceneRender::Get().zone_oz);
    PhysicsTerrainRegion::Get().SetOrigin(
        SceneRender::Get().tnoff_x, SceneRender::Get().tnoff_z,
        SceneRender::Get().zone_ox * CHUNK_SIZE, SceneRender::Get().zone_oz * CHUNK_SIZE);


    // Per-biome fog colour override removed: real Kenshi's fog is a global
    // screen-space post-process driven by sun angle/time-of-day, not
    // per-biome data (see scene_render.cpp for the full rationale).
    // GraphicsSettings::fog_color keeps its single global default.

    fprintf(stdout, "[Zone] transitioned to '%s' (Kenshi zones %d-%d x %d-%d)\n",
            zone_slug,
            SceneRender::Get().zone_ox, SceneRender::Get().zone_ox + SceneRender::TNKN - 1,
            SceneRender::Get().zone_oz, SceneRender::Get().zone_oz + SceneRender::TNKN - 1);
}
