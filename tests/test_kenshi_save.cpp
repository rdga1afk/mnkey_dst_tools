// Kenshi save folder reader / writer (engine/src/world/kenshi_save.cpp). Synthetic files only: the parser is checked on files our own writer makes
// (write -> read -> same values, write -> read -> write -> same bytes) and on damaged files; the round trip on the owner's real saves is pending (never committed).
#include <gtest/gtest.h>
#include <monkey_dust/io/fcs.h>
#include <monkey_dust/world/kenshi_save.h>
#include <filesystem>
#include <fstream>
#include <iterator>

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
        c.dead = i == 2; c.unconscious = i == 1; c.blood = 100.f - 10 * i; c.hunger = 2.f + i; c.fed = 0.5f * i;
        sq.characters.push_back(c);
    }
    s.squads.push_back(sq);
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
