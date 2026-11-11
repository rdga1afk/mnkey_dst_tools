// WorldRegistry biome grid and zone list from the install (owner 11.11: the record is the truth, no hand classification). Needs KENSHI_DIR.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/world_registry.h>
#include <cstring>
#include <cstdint>
#include <monkey_dust/io/fcs_gamedata.h>
#include <monkey_dust/world/biome_def.h>
#include <stb_image.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <vector>
#include <string>



// The biome of a cell is the BIOMES record that owns the colour of the cell centre: every colour the 4096 centres sample is the colour field of exactly one record (no nearest-colour
// fallback), and GetBiomeAt returns that record's slug for every cell.
TEST(BiomeMapInstall, EveryCellIsTheRecordThatOwnsItsColour) {
    ASSERT_FALSE(md::KenshiDir().empty() || !std::filesystem::exists(md::KenshiDir() + "/data/newland/land/biomemap.png") || !std::filesystem::exists(md::KenshiDir() + "/data/gamedata.base")) << "needs a Kenshi install (KENSHI_DIR)";
    const std::string dir = md::KenshiDir();
    WorldRegistry& wr = WorldRegistry::Get();
    ASSERT_TRUE(wr.BuildBiomeMapFromInstall());
    const BiomeRegistry& R = BiomeRegistry::Get();
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load((dir + "/data/newland/land/biomemap.png").c_str(), &w, &h, &ch, 4);
    ASSERT_NE(px, nullptr);
    std::set<uint32_t> colours; int missing = 0, wrong = 0;
    for (int z = 0; z < 64; ++z) for (int x = 0; x < 64; ++x) {
        const unsigned char* q = px + 4 * ((size_t)std::min(z * 16 + 8, h - 1) * w + std::min(x * 16 + 8, w - 1));
        const uint32_t c = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
        colours.insert(c);
        const int id = R.FindByLegend(c);
        if (id < 0) { ++missing; continue; }
        if (R.LegendAt(id) != c || strcmp(wr.GetBiomeAt(x, z), R.SlugAt(id)) != 0) ++wrong;
    }
    stbi_image_free(px);
    printf("[biome] %zu distinct colours, %d cells without a record, %d cells with another slug\n", colours.size(), missing, wrong);
    EXPECT_EQ(colours.size(), (size_t)57);
    EXPECT_EQ(missing, 0);
    EXPECT_EQ(wrong, 0);
}

// Zones from the install (owner 11.11, coordinator 6791171375 / 6791643957), tested on the EXACT set:
//  1. the zones built are exactly the kind-95 weather groups (not NONE) that have a region on the areasmap;
//  2. every zone's cell holds a texel of its group's own areasmap colour;
//  3. the 39 Kenshi names of the old zone list (the retired terrain_config.txt) are among the zones, asserted by name.
// The 20 old-only entries are our own labels, dropped: Great Desert, Rocky Canyon, Swamplands, The Hub Area, Shek Territory, The Spine, Holy Nation Territory, United Cities (East), United Cities (South), Shek Kingdom (Capital), Black Desert City, Kona's Crossing, North Wetlands, Mongrel (Fog Islands Interior), Copper Mountains, Rhinolands, Catlon (Robot Ruins), North Hive Village, South Hive, Distant Coast.
TEST(ZonesFromInstall, ExactSetOfGroupsWithARegion) {
    ASSERT_FALSE(md::KenshiDir().empty() || !std::filesystem::exists(md::KenshiDir() + "/data/newland/land/biomemap.png") || !std::filesystem::exists(md::KenshiDir() + "/data/gamedata.base")) << "needs a Kenshi install (KENSHI_DIR)";
    const std::string dir = md::KenshiDir();
    WorldRegistry& wr = WorldRegistry::Get();
    ASSERT_GT(wr.BuildZonesFromInstall(), 0);
    int w = 0, h = 0, ch = 0;
    unsigned char* areas = stbi_load((dir + "/data/newland/land/areasmap.tga").c_str(), &w, &h, &ch, 3);
    ASSERT_NE(areas, nullptr);
    md::fcs::Install inst(dir); md::fcs::LoadOrder order; md::fcs::Error e; md::fcs::GameDataContainer gd;
    ASSERT_TRUE(inst.LoadOrderFor(order, e) && gd.Load(order, e));
    std::set<uint32_t> map_colours;
    for (int i = 0; i < w * h; ++i) map_colours.insert((uint32_t)areas[3 * i] << 16 | (uint32_t)areas[3 * i + 1] << 8 | areas[3 * i + 2]);
    std::map<std::string, uint32_t> index_of; std::set<std::string> expected;
    for (const md::fcs::GameData* g : gd.OfType(95)) {
        const auto it = g->ints.find("index"); if (it == g->ints.end() || g->name == "NONE") continue;
        const uint32_t key = (uint32_t)it->second & 0xFFFFFFu; index_of[g->name] = key;
        if (map_colours.count(key)) expected.insert(g->name);
    }
    std::set<std::string> built; int n = 0; ZoneRecord* z = wr.GetAll(n); int outside = 0;
    for (int i = 0; i < n; ++i) {
        built.insert(z[i].display_name);
        bool found = false;
        for (int y = z[i].grid_z * h / 64; y < (z[i].grid_z + 1) * h / 64 && !found; ++y) for (int x = z[i].grid_x * w / 64; x < (z[i].grid_x + 1) * w / 64; ++x) {
            const unsigned char* p = areas + ((size_t)y * w + x) * 3;
            if ((((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]) == index_of[z[i].display_name]) { found = true; break; }
        }
        if (!found) ++outside;
    }
    stbi_image_free(areas);
    EXPECT_EQ(built, expected);
    EXPECT_EQ(built.size(), (size_t)n);   // no duplicate names
    EXPECT_EQ(outside, 0);
    for (const char* kenshi_name : { "Stenn Desert", "Skimsands", "Bonefields", "High Bonefields", "Stobe's Gamble", "Ashlands", "Venge", "Deadlands", "Dreg", "The Pits", "Stobe's Garden", "Okran's Pride", "Shem", "Gut", "Greenbeach", "Floodlands", "Sonorous Dark", "Border Zone", "Cannibal Plains", "Skinner's Roam", "Iron Valleys", "Howler Maze", "The Crags", "Bast", "Fog Islands", "Leviathan Coast", "Watcher's Rim", "Heng", "Rebirth", "Burning Forest", "Arm of Okran", "Okran's Valley", "The Eye", "Fishman Island", "Sinkuun", "Grey Desert", "South Wetlands", "Forbidden Isle", "Berserker Country" }) EXPECT_TRUE(built.count(kenshi_name)) << kenshi_name;
}
