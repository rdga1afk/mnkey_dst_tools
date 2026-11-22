// Hair / beard attachment meshes of the Hub residents build in the NPC skin layout: every mesh the look picks reads, skins by the body skeleton and indexes inside its vertex buffer.
// usage: md_npc_hair_mesh_test (KENSHI_DIR in the environment)
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/io/kenshi_resources.h>
#include <monkey_dust/io/ogre_mesh.h>
#include <monkey_dust/world/town_spawn.h>
#include <monkey_dust/world/npc_look.h>
#include <attachments.h>
#include "npc_hair_mesh.h"
#include "npc_skin_live.h"
#include <cstdio>
#include <cstdlib>
#include <map>

int main() {
    const char* env = std::getenv("KENSHI_DIR");
    if (!env || !*env) { std::printf("needs KENSHI_DIR\n"); return 1; }
    md::fcs::GameDataContainer data;
    { md::fcs::Install inst(md::KenshiDir()); md::fcs::LoadOrder order; md::fcs::Error err;
      if (!(inst.LoadOrderFor(order, err) && data.Load(order, err))) { std::printf("load failed\n"); return 1; } }
    md::KenshiResources res; res.Index(md::KenshiDir());
    const auto hub = md::world::MakeTownSpawnList(data, "18919-Newwworld.mod", 1);
    if (!hub.found) { std::printf("Hub not found\n"); return 1; }
    int tried = 0, built = 0, bad = 0, coloured = 0; std::map<uint32_t, int> palette;
    for (const auto& e : hub.anonymous) {
        const auto* t = data.Get(e.template_id); std::string fac = hub.owner;
        if (t) if (const auto* f = t->Refs("faction")) if (!f->empty()) fac = (*f)[0].first;
        const auto look = md::world::ResolveNpcLook(data, e.character, e.template_id, fac, 7);
        if (!look.resolved) continue;
        // hair colour pick: one entry of the race's `hair colors`, its `color 1` is the ARGB the draw multiplies with
        if (!look.hair_colour.empty()) {
            const auto* race = data.Get(look.race); const auto* cols = race ? race->Refs("hair colors") : nullptr; const auto* c = data.Get(look.hair_colour);
            bool in = false; if (cols) for (const auto& x : *cols) in = in || x.first == look.hair_colour;
            if (!in || !c || (c->ints.count("color 1") ? (uint32_t)c->ints.at("color 1") : 0u) != look.hair_argb) { std::printf("  bad hair colour: %s race=%s argb=%08x\n", look.hair_colour.c_str(), look.race.c_str(), look.hair_argb); ++bad; } else { ++coloured; ++palette[look.hair_argb]; }
        }
        md::ogre::Mesh bm; md::ogre::Error be;
        if (!md::ogre::ReadMesh(res.Path(look.mesh), bm, be) || !bm.has_skeleton) continue;
        for (const std::string& aid : { look.hair, look.beard }) {
            if (aid.empty()) continue;
            md::character::AttachmentInfo ai;
            if (!md::character::LoadAttachment(data, aid, ai) || !ai.HasMesh(look.female)) continue;
            const std::string mesh = res.Path(ai.Mesh(look.female));
            if (mesh.empty()) continue;
            ++tried;
            std::vector<uint8_t> v; std::vector<uint32_t> ix;
            if (!md::npcskin::BuildAttachmentMesh(mesh, res.Path(bm.skeleton), v, ix)) { std::printf("  build failed: %s (%s)\n", aid.c_str(), mesh.c_str()); ++bad; continue; }
            const size_t nv = v.size() / md::npcskin::kStride;
            bool ok = !v.empty() && v.size() % md::npcskin::kStride == 0 && !ix.empty() && ix.size() % 3 == 0;
            for (uint32_t i : ix) if (i >= nv) { ok = false; break; }
            if (!ok) { std::printf("  bad buffers: %s v=%zu i=%zu\n", aid.c_str(), v.size(), ix.size()); ++bad; continue; }
            ++built;
        }
    }
    std::printf("Hub attachment meshes tried %d, built %d, bad %d; hair colours picked %d (%zu distinct)\n", tried, built, bad, coloured, palette.size());
    return (tried > 0 && built == tried && bad == 0) ? 0 : 1;
}
