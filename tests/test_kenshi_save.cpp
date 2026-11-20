// Kenshi save folder reader / writer (engine/src/world/kenshi_save.cpp). Synthetic files only: the parser is checked on files our own writer makes
// (write -> read -> same values, write -> read -> write -> same bytes) and on damaged files; the round trip on the owner's real saves is pending (never committed).
#include <gtest/gtest.h>
#include <monkey_dust/io/fcs.h>
#include <monkey_dust/world/kenshi_save.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>

using namespace md::save;
namespace fs = std::filesystem;

namespace {
KenshiSave Sample() {
    KenshiSave s; s.day = 12; s.hour = 7; s.minute = 30; s.money = 4321; s.camera[0] = 9647.7f; s.camera[1] = 80.f; s.camera[2] = 15038.8f;
    s.faction_name = "Nameless"; s.version = "test-1"; s.zones = { {18, 30}, {19, 30} };
    KSquad sq; sq.id = "Nameless_0"; sq.faction = "10-gamedata.base"; sq.position[0] = 1.f; sq.position[1] = 2.f; sq.position[2] = 3.f;
    for (int i = 0; i < 3; ++i) {
        KChar c; c.template_id = "char" + std::to_string(i); c.name = "Guy " + std::to_string(i);
        c.position[0] = 100.f + i; c.position[1] = 5.f; c.position[2] = 200.f - i;
        c.rotation[0] = 1.f; c.rotation[2] = 0.25f * i;
        c.stats = { {"strength", 20.f + i}, {"attack", 15.5f} };
        c.dead = i == 2; c.unconscious = i == 1; c.coma = i == 1; c.ko = 3.5f * i; c.bleeding = 0.25f * i; c.blood = 100.f - 10 * i; c.hunger = 2.f + i; c.fed = 0.5f * i;
        if (i == 1) c.parts = { {"head", 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f}, {"arm", 0.5f, 0.f, 0.f, 0.f, 0.f, 0.f, 9.f} };
        sq.characters.push_back(c);
    }
    s.squads.push_back(sq);
    s.orders.present = true; s.orders.heal = true; s.orders.rescue = true; s.orders.sleep = true; s.orders.sit = true; s.orders.shootfirst = true; s.orders.formation = 3;
    s.sky.present = true; s.sky.current_time = 12.5f; s.sky.total_time = 600.f; s.sky.ambient[0] = 0.5f; s.sky.ambient[2] = 0.75f; s.sky.ambient_speed[1] = 0.125f; s.sky.clouds_density = 0.6f; s.sky.clouds_speed = 0.01f;
    s.squad_list.has_prosperity = true; s.squad_list.prosperity = 3.5f;
    { KSquadListEntry e; e.name = "squad_a"; e.time = 2.5f; e.refs = { {1, 2, 3, 4, 5}, {6, 7, 8, 9, 10} }; s.squad_list.entries.push_back(e); }
    { KSquadListEntry e; e.name = "squad_b"; e.time = 0.f; s.squad_list.entries.push_back(e); }
    return s;
}
std::vector<char> Slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return { std::istreambuf_iterator<char>(f), {} }; }
fs::path Tmp(const char* n) { fs::path p = fs::temp_directory_path() / (std::string("md_save_test_") + n); fs::remove_all(p); return p; }
}  // namespace

TEST(KenshiSave, RoundTripValues) {
    const fs::path d = Tmp("values"); std::string err;
    const KenshiSave s = Sample();
    ASSERT_TRUE(WriteKenshiSave(d.string(), s, err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_EQ(r.day, 12); EXPECT_EQ(r.hour, 7); EXPECT_EQ(r.minute, 30); EXPECT_EQ(r.money, 4321);
    EXPECT_FLOAT_EQ(r.camera[0], 9647.7f); EXPECT_EQ(r.faction_name, "Nameless"); EXPECT_EQ(r.version, "test-1");
    ASSERT_EQ(r.zones.size(), 2u); EXPECT_EQ(r.zones[1], std::make_pair(19, 30));
    ASSERT_EQ(r.squads.size(), 1u); ASSERT_EQ(r.squads[0].characters.size(), 3u);
    EXPECT_EQ(r.squads[0].id, "Nameless_0"); EXPECT_FLOAT_EQ(r.squads[0].position[2], 3.f);
    const KChar& c = r.squads[0].characters[2];
    EXPECT_EQ(c.template_id, "char2"); EXPECT_EQ(c.name, "Guy 2"); EXPECT_FLOAT_EQ(c.position[2], 198.f); EXPECT_FLOAT_EQ(c.rotation[2], 0.5f);
    ASSERT_EQ(c.stats.size(), 2u); EXPECT_EQ(c.stats[0].first, "strength"); EXPECT_FLOAT_EQ(c.stats[0].second, 22.f);
    EXPECT_TRUE(c.dead); EXPECT_FALSE(c.unconscious); EXPECT_TRUE(r.squads[0].characters[1].unconscious);
    EXPECT_FLOAT_EQ(r.squads[0].characters[1].ko, 3.5f); EXPECT_FLOAT_EQ(r.squads[0].characters[2].bleeding, 0.5f); EXPECT_TRUE(r.squads[0].characters[1].coma);
    EXPECT_FLOAT_EQ(c.blood, 80.f); EXPECT_FLOAT_EQ(c.hunger, 4.f); EXPECT_FLOAT_EQ(c.fed, 1.f);
}

TEST(KenshiSave, RoundTripSameBytes) {
    const fs::path a = Tmp("bytes_a"), b = Tmp("bytes_b"); std::string err;
    ASSERT_TRUE(WriteKenshiSave(a.string(), Sample(), err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(a.string(), r, err)) << err;
    ASSERT_TRUE(WriteKenshiSave(b.string(), r, err)) << err;
    EXPECT_EQ(Slurp(a / "quick.save"), Slurp(b / "quick.save"));
    EXPECT_EQ(Slurp(a / "platoon" / "Nameless_0.platoon"), Slurp(b / "platoon" / "Nameless_0.platoon"));
    // the files are v15: the parser of the FCS layer reads them back and finds no leftover bytes
    md::fcs::File f; md::fcs::Error e;
    ASSERT_TRUE(md::fcs::ReadFile((a / "quick.save").string(), f, e)) << e.Message();
    EXPECT_EQ(f.header.filetype, md::fcs::FILETYPE_V15);
}

TEST(KenshiSave, OtherFactionSquadIsSkipped) {
    const fs::path d = Tmp("other"); std::string err;
    KenshiSave s = Sample(); KSquad other = s.squads[0]; other.id = "Other_0"; s.squads.push_back(other);
    ASSERT_TRUE(WriteKenshiSave(d.string(), s, err)) << err;
    // rewrite quick.save's second platoon as another faction by reading, changing and writing the FCS file
    md::fcs::File f; md::fcs::Error e; ASSERT_TRUE(md::fcs::ReadFile((d / "quick.save").string(), f, e));
    for (auto& r : f.records) if (r.kind == 34 && r.name == "Other_0") for (auto& kv : r.strings) if (kv.first == "faction name") kv.second = "Holy Nation";
    ASSERT_TRUE(md::fcs::WriteFile((d / "quick.save").string(), f, e));
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    ASSERT_EQ(r.squads.size(), 1u); EXPECT_EQ(r.squads[0].id, "Nameless_0");
}

TEST(KenshiSave, DamagedFilesGiveReadableErrors) {
    const fs::path d = Tmp("damaged"); std::string err; KenshiSave r;
    EXPECT_FALSE(ReadKenshiSave(d.string(), r, err)); EXPECT_NE(err.find("quick.save"), std::string::npos); EXPECT_NE(err.find("not found"), std::string::npos);
    ASSERT_TRUE(WriteKenshiSave(d.string(), Sample(), err)) << err;
    // truncated quick.save
    auto bytes = Slurp(d / "quick.save"); bytes.resize(bytes.size() / 2);
    { std::ofstream o(d / "quick.save", std::ios::binary); o.write(bytes.data(), (std::streamsize)bytes.size()); }
    EXPECT_FALSE(ReadKenshiSave(d.string(), r, err)); EXPECT_NE(err.find("quick.save"), std::string::npos) << err;
    // garbage
    { std::ofstream o(d / "quick.save", std::ios::binary); o << "not a save file at all"; }
    EXPECT_FALSE(ReadKenshiSave(d.string(), r, err)); EXPECT_FALSE(err.empty());
    // a missing platoon file
    ASSERT_TRUE(WriteKenshiSave(d.string(), Sample(), err)) << err;
    fs::remove(d / "platoon" / "Nameless_0.platoon");
    EXPECT_FALSE(ReadKenshiSave(d.string(), r, err)); EXPECT_NE(err.find("Nameless_0.platoon"), std::string::npos) << err;
    // a platoon file cut in half
    ASSERT_TRUE(WriteKenshiSave(d.string(), Sample(), err)) << err;
    auto pb = Slurp(d / "platoon" / "Nameless_0.platoon"); pb.resize(pb.size() - 7);
    { std::ofstream o(d / "platoon" / "Nameless_0.platoon", std::ios::binary); o.write(pb.data(), (std::streamsize)pb.size()); }
    EXPECT_FALSE(ReadKenshiSave(d.string(), r, err)); EXPECT_NE(err.find("Nameless_0.platoon"), std::string::npos) << err;
}

// Facts of the exe writer (save_manager_save_game 0x140375360, record_store_write_file 0x1406bd570; ksdp/functions/exe_save.yml): the file is FCS v15, the first record is the camera record
// (kind 56) with the ints "squads" and "members", zones are references (x, z, 1), platoon records are kind 34 with "platoon stringID".
TEST(KenshiSave, FormatFollowsTheExeWriter) {
    const fs::path d = Tmp("exefacts"); std::string err;
    ASSERT_TRUE(WriteKenshiSave(d.string(), Sample(), err)) << err;
    auto bytes = Slurp(d / "quick.save");
    ASSERT_GE(bytes.size(), 12u);
    int32_t head[3]; memcpy(head, bytes.data(), 12);
    EXPECT_EQ(head[0], 15);                      // filetype, written first
    md::fcs::File f; md::fcs::Error e; ASSERT_TRUE(md::fcs::ReadFile((d / "quick.save").string(), f, e)) << e.Message();
    EXPECT_EQ(head[2], (int32_t)f.records.size());   // the record count follows the id counter
    ASSERT_FALSE(f.records.empty());
    const md::fcs::Record& cam = f.records[0];
    EXPECT_EQ(cam.kind, 56);
    auto getint = [&](const char* k) { for (auto& kv : cam.ints) if (kv.first == k) return kv.second; return -1; };
    EXPECT_EQ(getint("squads"), 1); EXPECT_EQ(getint("members"), 3);
    ASSERT_EQ(cam.references.size(), 1u); EXPECT_EQ(cam.references[0].first, "zones");
    for (const auto& z : cam.references[0].second) EXPECT_EQ(z.values[2], 1);
    int platoons = 0; for (const auto& r : f.records) if (r.kind == 34) { ++platoons; bool has = false; for (auto& kv : r.strings) if (kv.first == "platoon stringID") has = true; EXPECT_TRUE(has); }
    EXPECT_EQ(platoons, 1);
    // the platoon record names its file ("content file" = platoon/<name>.platoon) and the reader follows that name
    for (auto& r : f.records) if (r.kind == 34) { ASSERT_EQ(r.files.size(), 1u); EXPECT_EQ(r.files[0].first, "content file"); EXPECT_EQ(r.files[0].second, "platoon/Nameless_0.platoon"); }
    fs::rename(d / "platoon" / "Nameless_0.platoon", d / "platoon" / "moved.platoon");
    for (auto& r : f.records) if (r.kind == 34) r.files[0].second = "platoon/moved.platoon";
    ASSERT_TRUE(md::fcs::WriteFile((d / "quick.save").string(), f, e));
    KenshiSave back; ASSERT_TRUE(ReadKenshiSave(d.string(), back, err)) << err;
    ASSERT_EQ(back.squads.size(), 1u); EXPECT_EQ(back.squads[0].characters.size(), 3u);
}

// Facts of the exe's load side (STATS loader 0x14064b6a0, save-info loader 0x14047e390; ksdp/functions/exe_save.yml).
TEST(KenshiSave, StatKeysAreTheExeWriterList) {
    const auto& k = StatKeys();
    ASSERT_EQ(k.size(), 44u);
    EXPECT_EQ(k.front(), "strength"); EXPECT_EQ(k[4], "toughness2"); EXPECT_EQ(k[9], "ff"); EXPECT_EQ(k[40], "bow"); EXPECT_EQ(k[42], "xp"); EXPECT_EQ(k.back(), "free attribute points");
}

TEST(KenshiSave, MissingStatReadsZeroAndUnknownIsIgnored) {
    KChar c; c.stats = { {"strength", 30.f}, {"no such stat", 9.f} };
    EXPECT_FLOAT_EQ(StatValue(c, "strength"), 30.f);
    EXPECT_FLOAT_EQ(StatValue(c, "attack"), 0.f);          // absent key: the exe's map lookup inserts 0
    EXPECT_FLOAT_EQ(StatValue(c, "farming"), 0.f);
}

TEST(KenshiSave, ToughnessAndLockpickingFallBacks) {
    KChar c; c.stats = { {"defence", 41.f}, {"thievery", 30.f} };
    EXPECT_FLOAT_EQ(StatValue(c, "toughness2"), 41.f);     // absent: the defence value
    EXPECT_FLOAT_EQ(StatValue(c, "lockpicking"), 15.f);    // absent: thievery * 0.5
    c.stats.push_back({"toughness2", 7.f}); c.stats.push_back({"lockpicking", 3.f});
    EXPECT_FLOAT_EQ(StatValue(c, "toughness2"), 7.f); EXPECT_FLOAT_EQ(StatValue(c, "lockpicking"), 3.f);
}

TEST(KenshiSave, MissingCameraIntsReadZero) {
    const fs::path d = Tmp("camzero"); std::string err;
    ASSERT_TRUE(WriteKenshiSave(d.string(), Sample(), err)) << err;
    md::fcs::File f; md::fcs::Error e; ASSERT_TRUE(md::fcs::ReadFile((d / "quick.save").string(), f, e));
    auto& ints = f.records[0].ints;
    for (auto it = ints.begin(); it != ints.end();) { if (it->first == "time day" || it->first == "player money") it = ints.erase(it); else ++it; }
    ASSERT_TRUE(md::fcs::WriteFile((d / "quick.save").string(), f, e));
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_EQ(r.day, 0); EXPECT_EQ(r.money, 0); EXPECT_EQ(r.hour, 7);
}

// player-order toggles and formation (exe writer 0x1403eb710, loader 0x1403ebf50)
TEST(KenshiSave, OrdersRoundTripValuesAndKeys) {
    const fs::path d = Tmp("orders"); std::string err;
    ASSERT_TRUE(WriteKenshiSave(d.string(), Sample(), err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_TRUE(r.orders.present); EXPECT_EQ(r.orders.formation, 3);
    EXPECT_TRUE(r.orders.heal); EXPECT_FALSE(r.orders.help); EXPECT_TRUE(r.orders.rescue); EXPECT_FALSE(r.orders.stay); EXPECT_FALSE(r.orders.feed); EXPECT_FALSE(r.orders.share);
    EXPECT_TRUE(r.orders.sleep); EXPECT_FALSE(r.orders.ditch); EXPECT_TRUE(r.orders.sit); EXPECT_FALSE(r.orders.eject); EXPECT_TRUE(r.orders.shootfirst);
    // the key set and its order are the exe writer's: bools ditch sleep feed heal help rescue share stay sit eject shootfirst, int formation
    md::fcs::File f; md::fcs::Error e; ASSERT_TRUE(md::fcs::ReadFile((d / "quick.save").string(), f, e)) << e.Message();
    const md::fcs::Record* cam = nullptr; for (const auto& rec : f.records) if (rec.kind == 56) cam = &rec;
    ASSERT_NE(cam, nullptr);
    const char* want[] = {"ditch", "sleep", "feed", "heal", "help", "rescue", "share", "stay", "sit", "eject", "shootfirst"};
    ASSERT_EQ(cam->bools.size(), 12u);   // eleven orders + `sky updating`
    for (int i = 0; i < 11; ++i) EXPECT_EQ(cam->bools[i].first, want[i]);
    bool has_formation = false; for (const auto& kv : cam->ints) if (kv.first == "formation") has_formation = true;
    EXPECT_TRUE(has_formation);
}

TEST(KenshiSave, OrdersAbsentKeepsDefaults) {
    const fs::path d = Tmp("orders_absent"); std::string err;
    KenshiSave s = Sample(); s.orders = KOrders{};
    ASSERT_TRUE(WriteKenshiSave(d.string(), s, err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_FALSE(r.orders.present); EXPECT_EQ(r.orders.formation, 0); EXPECT_FALSE(r.orders.heal);
}

// world clock and sky (exe writer 0x14066d7d0, loader 0x14066db50)
TEST(KenshiSave, SkyRoundTripValuesAndKeys) {
    const fs::path d = Tmp("sky"); std::string err;
    ASSERT_TRUE(WriteKenshiSave(d.string(), Sample(), err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_TRUE(r.sky.present); EXPECT_TRUE(r.sky.updating);
    EXPECT_FLOAT_EQ(r.sky.current_time, 12.5f); EXPECT_FLOAT_EQ(r.sky.total_time, 600.f); EXPECT_FLOAT_EQ(r.sky.clouds_density, 0.6f); EXPECT_FLOAT_EQ(r.sky.clouds_speed, 0.01f);
    EXPECT_FLOAT_EQ(r.sky.ambient[0], 0.5f); EXPECT_FLOAT_EQ(r.sky.ambient[2], 0.75f); EXPECT_FLOAT_EQ(r.sky.ambient_speed[1], 0.125f);
    md::fcs::File f; md::fcs::Error e; ASSERT_TRUE(md::fcs::ReadFile((d / "quick.save").string(), f, e)) << e.Message();
    const md::fcs::Record* cam = nullptr; for (const auto& rec : f.records) if (rec.kind == 56) cam = &rec;
    ASSERT_NE(cam, nullptr);
    auto has = [](const auto& tbl, const char* k) { for (const auto& kv : tbl) if (kv.first == k) return true; return false; };
    for (const char* k : {"sky update current time", "sky update total time", "sky clouds density", "sky clouds density speed"}) EXPECT_TRUE(has(cam->floats, k)) << k;
    for (const char* k : {"sky ambient color mult", "sky ambient color mult speed"}) EXPECT_TRUE(has(cam->vec3s, k)) << k;
    EXPECT_TRUE(has(cam->bools, "sky updating"));
    for (const char* k : {"time day", "time hour", "time minute"}) EXPECT_TRUE(has(cam->ints, k)) << k;
}

TEST(KenshiSave, SkyAbsentKeepsExeDefaults) {
    const fs::path d = Tmp("sky_absent"); std::string err;
    KenshiSave s = Sample(); s.sky = KSky{};
    ASSERT_TRUE(WriteKenshiSave(d.string(), s, err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_FALSE(r.sky.present); EXPECT_TRUE(r.sky.updating); EXPECT_FLOAT_EQ(r.sky.clouds_density, 0.25f); EXPECT_FLOAT_EQ(r.sky.ambient[1], 1.f); EXPECT_FLOAT_EQ(r.sky.clouds_speed, 0.f);
}

TEST(KenshiSave, SkyUpdatingOffIsResetByLoader) {
    const fs::path d = Tmp("sky_off"); std::string err;
    KenshiSave s = Sample(); s.sky.updating = false; s.sky.ambient_speed[0] = 0.5f;
    ASSERT_TRUE(WriteKenshiSave(d.string(), s, err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_TRUE(r.sky.updating); EXPECT_FLOAT_EQ(r.sky.ambient_speed[0], 0.f); EXPECT_FLOAT_EQ(r.sky.clouds_speed, 0.f); EXPECT_FLOAT_EQ(r.sky.current_time, 0.f); EXPECT_FLOAT_EQ(r.sky.total_time, 0.f);
    EXPECT_FLOAT_EQ(r.sky.clouds_density, 0.6f); EXPECT_FLOAT_EQ(r.sky.ambient[0], 0.5f);   // the values read before the check stay
}

TEST(KenshiSave, NegativeClockClampsToZero) {
    const fs::path d = Tmp("clock_neg"); std::string err;
    KenshiSave s = Sample(); s.day = -3; s.hour = -1; s.minute = 5;
    ASSERT_TRUE(WriteKenshiSave(d.string(), s, err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_EQ(r.day, 0); EXPECT_EQ(r.hour, 0); EXPECT_EQ(r.minute, 5);
}

// squad list (exe writer 0x1402deea0, loader 0x1402df0c0)
TEST(KenshiSave, SquadListRoundTripValuesAndKeys) {
    const fs::path d = Tmp("sqlist"); std::string err;
    ASSERT_TRUE(WriteKenshiSave(d.string(), Sample(), err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_TRUE(r.squad_list.has_prosperity); EXPECT_FLOAT_EQ(r.squad_list.prosperity, 3.5f);
    ASSERT_EQ(r.squad_list.entries.size(), 2u);
    EXPECT_EQ(r.squad_list.entries[0].name, "squad_a"); EXPECT_FLOAT_EQ(r.squad_list.entries[0].time, 2.5f);
    ASSERT_EQ(r.squad_list.entries[0].refs.size(), 2u);
    const KSquadRef& q = r.squad_list.entries[0].refs[1]; EXPECT_EQ(q.type, 6); EXPECT_EQ(q.c, 7); EXPECT_EQ(q.i, 8); EXPECT_EQ(q.s, 9); EXPECT_EQ(q.cs, 10);
    EXPECT_EQ(r.squad_list.entries[1].name, "squad_b"); EXPECT_TRUE(r.squad_list.entries[1].refs.empty());
    md::fcs::File f; md::fcs::Error e; ASSERT_TRUE(md::fcs::ReadFile((d / "quick.save").string(), f, e)) << e.Message();
    const md::fcs::Record* cam = nullptr; for (const auto& rec : f.records) if (rec.kind == 56) cam = &rec;
    ASSERT_NE(cam, nullptr);
    auto has = [](const auto& tbl, const std::string& k) { for (const auto& kv : tbl) if (kv.first == k) return true; return false; };
    EXPECT_TRUE(has(cam->strings, "sq0")); EXPECT_TRUE(has(cam->strings, "sq1")); EXPECT_FALSE(has(cam->strings, "sq2"));
    EXPECT_TRUE(has(cam->floats, "sqtime0")); EXPECT_TRUE(has(cam->floats, "sqtime1")); EXPECT_TRUE(has(cam->floats, "prosperity"));
    for (const char* sfx : {"TYPE", "C", "I", "S", "CS"}) EXPECT_TRUE(has(cam->ints, std::string("0:sq list1") + sfx)) << sfx;
    EXPECT_FALSE(has(cam->ints, "0:sq list2I")); EXPECT_FALSE(has(cam->ints, "1:sq list0I"));
}

TEST(KenshiSave, SquadListEmptyAndNoProsperity) {
    const fs::path d = Tmp("sqlist_empty"); std::string err;
    KenshiSave s = Sample(); s.squad_list = KSquadList{};
    ASSERT_TRUE(WriteKenshiSave(d.string(), s, err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_TRUE(r.squad_list.entries.empty()); EXPECT_FALSE(r.squad_list.has_prosperity);
}

// per-part wounds (exe writer 0x14064d390, loader 0x14064d600)
TEST(KenshiSave, PartWoundsRoundTripAndKeys) {
    const fs::path d = Tmp("parts"); std::string err;
    ASSERT_TRUE(WriteKenshiSave(d.string(), Sample(), err)) << err;
    KenshiSave r; ASSERT_TRUE(ReadKenshiSave(d.string(), r, err)) << err;
    EXPECT_TRUE(r.squads[0].characters[0].parts.empty());
    const auto& p = r.squads[0].characters[1].parts; ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(p[0].sid, "head"); EXPECT_FLOAT_EQ(p[0].hit, 1.f); EXPECT_FLOAT_EQ(p[0].hitmult, 2.f); EXPECT_FLOAT_EQ(p[0].flesh, 3.f); EXPECT_FLOAT_EQ(p[0].stun, 4.f);
    EXPECT_FLOAT_EQ(p[0].bandage, 5.f); EXPECT_FLOAT_EQ(p[0].rig, 6.f); EXPECT_FLOAT_EQ(p[0].wear, 7.f);
    EXPECT_EQ(p[1].sid, "arm"); EXPECT_FLOAT_EQ(p[1].wear, 9.f);
    md::fcs::File f; md::fcs::Error e; ASSERT_TRUE(md::fcs::ReadFile((d / "platoon" / "Nameless_0.platoon").string(), f, e)) << e.Message();
    const md::fcs::Record* med = nullptr; int seen = 0; for (const auto& rec : f.records) if (rec.kind == 57 && ++seen == 2) med = &rec;
    ASSERT_NE(med, nullptr);
    auto has = [](const auto& tbl, const std::string& k) { for (const auto& kv : tbl) if (kv.first == k) return true; return false; };
    EXPECT_TRUE(has(med->strings, "sid0")); EXPECT_TRUE(has(med->strings, "sid1")); EXPECT_FALSE(has(med->strings, "sid2"));
    for (const char* k : {"hit", "hitmult", "flesh", "stun", "bandage", "rig", "wear"}) { EXPECT_TRUE(has(med->floats, std::string(k) + "0")) << k; EXPECT_TRUE(has(med->floats, std::string(k) + "1")) << k; }
}
