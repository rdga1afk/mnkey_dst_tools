// FCS reader / load order / merge tests (engine/src/io/fcs*.cpp). Synthetic buffers need no game data; the data test runs only when
// KENSHI_DIR points at a Kenshi folder (the one with data/gamedata.base) -- no game data lives in the repo.
#include <gtest/gtest.h>
#include <monkey_dust/io/fcs.h>
#include <monkey_dust/io/fcs_gamedata.h>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace md::fcs;
namespace fs = std::filesystem;

namespace {
struct W {
    std::vector<uint8_t> b;
    W& i32(int32_t v) { for (int k = 0; k < 4; ++k) b.push_back((uint8_t)((uint32_t)v >> (8 * k))); return *this; }
    W& f32(float v) { uint8_t t[4]; memcpy(t, &v, 4); b.insert(b.end(), t, t + 4); return *this; }
    W& str(const std::string& s) { i32((int32_t)s.size()); b.insert(b.end(), s.begin(), s.end()); return *this; }
    W& u8(uint8_t v) { b.push_back(v); return *this; }
    W& raw(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); return *this; }
};

// one record with: bool "REMOVED" optional, float "weight", one reference category, one instance
struct R {
    std::string sid, name; int32_t kind = 4; uint32_t datatype = 0;
    float weight = -1; bool removed = false;
    std::vector<std::tuple<std::string, std::string, int32_t>> refs; // category, target, v0 (v1=v2=0; v0==INT_MAX -> remove)
    std::vector<std::pair<std::string, std::string>> insts;          // id, target
};

std::vector<uint8_t> RecBytes(const R& r, int filetype) {
    W w;
    w.i32(0).i32(r.kind).i32(7).str(r.name).str(r.sid).i32((int32_t)r.datatype);
    if (r.removed) w.i32(1).str("REMOVED").u8(1); else w.i32(0);
    if (r.weight >= 0) w.i32(1).str("weight").f32(r.weight); else w.i32(0);
    w.i32(0).i32(0).i32(0).i32(0).i32(0); // ints, vec3s, vec4s, strings, files
    w.i32((int32_t)r.refs.size());
    for (auto& [cat, target, v0] : r.refs) {
        w.str(cat).i32(1).str(target).i32(v0).i32(v0 == INT_MAX ? INT_MAX : 0).i32(v0 == INT_MAX ? INT_MAX : 0);
    }
    w.i32((int32_t)r.insts.size());
    for (auto& [id, target] : r.insts) w.str(id).str(target).f32(1).f32(2).f32(3).f32(1).f32(0).f32(0).f32(0).i32(0);
    if (filetype == 17) { // lead = byte length including the lead field
        int32_t n = (int32_t)w.b.size();
        for (int k = 0; k < 4; ++k) w.b[k] = (uint8_t)((uint32_t)n >> (8 * k));
    }
    return w.b;
}

std::vector<uint8_t> FileBytes(int filetype, const std::string& deps, int id_counter, const std::vector<R>& recs, const std::vector<uint8_t>& tail = {}) {
    W w;
    w.i32(filetype);
    if (filetype != 15) {
        W body; body.i32(1).str("a").str("d").str(deps).str("");
        body.raw(tail);
        if (filetype == 17) w.i32((int32_t)body.b.size());
        w.raw(body.b);
    }
    w.i32(id_counter).i32((int32_t)recs.size());
    for (auto& r : recs) w.raw(RecBytes(r, filetype));
    return w.b;
}

File Parsed(const std::vector<uint8_t>& b) { File f; Error e; EXPECT_TRUE(Parse(b.data(), b.size(), f, e)) << e.Message(); return f; }
} // namespace

TEST(Fcs, ParsesV16AndV17) {
    R a; a.sid = "1-x"; a.name = "Sword"; a.weight = 2.0f; a.refs = {{"parts", "t", 5}}; a.insts = {{"i1", "t"}};
    File f16 = Parsed(FileBytes(16, "gamedata.base, other.mod", 9, {a}));
    ASSERT_EQ(f16.records.size(), 1u);
    EXPECT_EQ(f16.header.filetype, 16);
    EXPECT_EQ(f16.header.id_counter, 9);
    EXPECT_EQ(FileList(f16.header.dependencies), (std::vector<std::string>{"gamedata.base", "other.mod"}));
    EXPECT_EQ(f16.records[0].string_id, "1-x");
    EXPECT_FLOAT_EQ(f16.records[0].floats[0].second, 2.0f);
    EXPECT_EQ(f16.records[0].references[0].second[0].values[0], 5);
    EXPECT_EQ(f16.records[0].instances[0].position[2], 3.0f);

    File f17 = Parsed(FileBytes(17, "", 3, {a}, {1, 2, 3, 4, 5}));
    EXPECT_EQ(f17.header.tail, (std::vector<uint8_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(f17.records[0].lead != 0, true);

    File f15 = Parsed(FileBytes(15, "", 2, {a}));
    EXPECT_EQ(f15.header.filetype, 15);
    EXPECT_FALSE(f15.has_trailer);
}

TEST(Fcs, HeaderTailDecodes) {
    HeaderExtra x;
    EXPECT_TRUE(ParseHeaderTail({}, x));
    EXPECT_EQ(x.save_counter, 0u);
    // shipped-mod shapes: counter, last merge, no merges, with and without the removal block
    EXPECT_TRUE(ParseHeaderTail({1, 0, 0, 0, 0, 0, 0, 0, 0}, x));
    EXPECT_EQ(x.save_counter, 1u);
    EXPECT_TRUE(x.removals.empty());
    EXPECT_TRUE(ParseHeaderTail({9, 0, 0, 0, 0, 0, 0, 0, 0, 0}, x));
    EXPECT_EQ(x.save_counter, 9u);
    W w; w.i32(5).i32(2).u8(1).str("m.mod").i32(3).i32(4).u8(1).str("r.mod").i32(7).str("1:2");
    EXPECT_TRUE(ParseHeaderTail(w.b, x));
    ASSERT_EQ(x.merges.size(), 1u);
    EXPECT_EQ(x.merges[0].mod, "m.mod");
    EXPECT_EQ(x.merges[0].b, 4u);
    ASSERT_EQ(x.removals.size(), 1u);
    EXPECT_EQ(x.removals[0].version, 7u);
    EXPECT_EQ(x.removals[0].ids, "1:2");
    EXPECT_FALSE(ParseHeaderTail({1, 0, 0}, x));   // truncated
}

TEST(Fcs, SerializeRoundTripsSynthetic) {
    R a; a.sid = "1-x"; a.name = "Sword"; a.weight = 2.0f; a.refs = {{"parts", "t", 5}}; a.insts = {{"i1", "t"}};
    for (int ft : {16, 17}) {
        const std::vector<uint8_t> in = FileBytes(ft, "a.mod", 9, {a, a}, ft == 17 ? std::vector<uint8_t>{1, 0, 0, 0, 0, 0, 0, 0, 0} : std::vector<uint8_t>{});
        File f = Parsed(in);
        std::vector<uint8_t> out; Error e;
        ASSERT_TRUE(Serialize(f, out, e)) << e.Message();
        EXPECT_EQ(in, out) << "filetype " << ft;
    }
    File f15; f15.header.filetype = 15; std::vector<uint8_t> out; Error e;
    EXPECT_FALSE(Serialize(f15, out, e));
}

TEST(FcsData, InstalledFilesRoundTripByteForByte) {
    const char* kd = std::getenv("KENSHI_DIR");
    if (!kd || !*kd) return;
    size_t n = 0;
    for (const auto& ent : fs::directory_iterator(fs::path(kd) / "data")) {
        const std::string ext = ent.path().extension().string();
        if (ext != ".mod" && ent.path().filename() != "gamedata.base") continue;
        File f; Error e;
        if (!ReadFile(ent.path().string(), f, e)) continue;   // v15 zone-type files are covered by the parse test
        if (f.header.filetype == FILETYPE_V15) continue;
        std::ifstream in(ent.path(), std::ios::binary); std::vector<uint8_t> orig((std::istreambuf_iterator<char>(in)), {});
        std::vector<uint8_t> out; ASSERT_TRUE(Serialize(f, out, e)) << e.Message();
        EXPECT_TRUE(orig == out) << ent.path().filename() << ": " << orig.size() << " bytes read, " << out.size() << " written";
        ++n;
    }
    EXPECT_GT(n, 0u);
}

TEST(Fcs, RejectsDamagedFiles) {
    R a; a.sid = "s"; a.name = "n";
    auto ok = FileBytes(16, "", 1, {a});
    File f; Error e;
    auto cut = ok; cut.pop_back();
    EXPECT_FALSE(Parse(cut.data(), cut.size(), f, e));
    EXPECT_TRUE(e.kind == ErrorKind::Truncated || e.kind == ErrorKind::BadLength) << e.Message();
    auto extra = ok; extra.push_back(0);
    EXPECT_FALSE(Parse(extra.data(), extra.size(), f, e));
    EXPECT_EQ(e.kind, ErrorKind::TrailingBytes);
    auto bad = ok; bad[0] = 99;
    EXPECT_FALSE(Parse(bad.data(), bad.size(), f, e));
    EXPECT_EQ(e.kind, ErrorKind::UnsupportedFiletype);
    auto v17 = FileBytes(17, "", 1, {a});
    // corrupt the record's declared length (the lead): find it right after the header counts
    size_t lead_at = v17.size() - (RecBytes(a, 17).size());
    v17[lead_at] ^= 0x7f;
    EXPECT_FALSE(Parse(v17.data(), v17.size(), f, e));
    EXPECT_EQ(e.kind, ErrorKind::LengthMismatch) << e.Message();
    // a count the file cannot hold
    W w; w.i32(16).i32(1).str("").str("").str("").str("").i32(0).i32(1000000);
    EXPECT_FALSE(Parse(w.b.data(), w.b.size(), f, e));
}

TEST(Fcs, ModsCfgRules) {
    std::string cfg = "A.mod\r\n# comment.mod\n/x.mod\n;y.mod\n\nnot a mod.txt\nB Mod (2).MOD\nA.mod\n  C.mod  \n";
    EXPECT_EQ(ParseModsCfg(cfg), (std::vector<std::string>{"A", "B Mod (2)", "C"}));
}

TEST(Fcs, ItemTypeNamesComplete) {
    for (int i = 0; i < ITEM_TYPE_COUNT; ++i) { ASSERT_NE(ItemTypeName(i), nullptr); EXPECT_STRNE(ItemTypeName(i), "?") << i; }
    EXPECT_STREQ(ItemTypeName(ITEM_BIOMES), "BIOMES");
    EXPECT_STREQ(ItemTypeName(ITEM_TYPE_COUNT), "?");
}

TEST(FcsMerge, Rules) {
    R base; base.sid = "1-base"; base.name = "Sword"; base.datatype = 0x80000002u; base.weight = 2.0f;
    base.refs = {{"parts", "a", 1}, {"parts", "b", 2}}; base.insts = {{"i1", "x"}, {"i2", "y"}};
    GameDataContainer c;
    c.ApplyFile("base", Parsed(FileBytes(16, "", 5, {base})));
    ASSERT_NE(c.Get("1-base"), nullptr);
    EXPECT_EQ(c.Get("1-base")->name, "Sword");

    R mod; mod.sid = "1-base"; mod.name = "Blade"; mod.datatype = 0x80000003u; mod.weight = 3.5f;
    mod.refs = {{"parts", "a", INT_MAX}, {"parts", "c", 9}}; mod.insts = {{"i1", ""}, {"i3", "z"}};
    R fresh; fresh.sid = "2-new"; fresh.name = "New"; fresh.datatype = 0x1; // modifies, but does not exist: warning
    c.ApplyFile("mod", Parsed(FileBytes(16, "base", 11, {mod, fresh})));
    const GameData* g = c.Get("1-base");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->name, "Blade");                       // datatype bit 1 renames
    EXPECT_FLOAT_EQ(g->floats.at("weight"), 3.5f);    // field overwritten
    const auto* parts = g->Refs("parts");
    ASSERT_NE(parts, nullptr);
    ASSERT_EQ(parts->size(), 2u);                     // a removed, b kept, c added
    EXPECT_EQ((*parts)[0].first, "b");
    EXPECT_EQ((*parts)[1].first, "c");
    ASSERT_EQ(g->instances.size(), 2u);               // i1 removed (empty target), i2 kept, i3 added
    EXPECT_EQ(g->instances[0].id, "i2");
    EXPECT_EQ(g->instances[1].id, "i3");
    EXPECT_EQ(c.id_counter, 11);
    ASSERT_EQ(c.warnings.size(), 1u);
    EXPECT_NE(c.warnings[0].find("does not exist"), std::string::npos);
    EXPECT_EQ(c.Get("2-new")->source, 1u);

    R rm; rm.sid = "1-base"; rm.name = "x"; rm.removed = true;
    c.ApplyFile("rm", Parsed(FileBytes(16, "", 1, {rm})));
    EXPECT_EQ(c.Get("1-base"), nullptr);              // REMOVED deletes
    EXPECT_EQ(c.OfType(4).size(), 1u);                // "2-new" is of kind 4
}

namespace {
void Write(const fs::path& p, const std::vector<uint8_t>& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)b.data(), (std::streamsize)b.size());
}
void WriteText(const fs::path& p, const std::string& t) { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary); f << t; }
} // namespace

TEST(FcsLoadOrder, CoreDependenciesAndMods) {
    fs::path root = fs::temp_directory_path() / "md_fcs_test_install" / "steamapps" / "common" / "Kenshi";
    fs::remove_all(root.parent_path().parent_path());
    R r; r.sid = "1-r"; r.name = "n";
    Write(root / "data/gamedata.base", FileBytes(16, "", 1, {r}));
    Write(root / "data/A.mod", FileBytes(16, "Z.mod", 1, {r}));
    Write(root / "data/B.mod", FileBytes(16, "gamedata.base", 1, {r}));
    Write(root / "data/Z.mod", FileBytes(16, "gamedata.base", 1, {r}));
    Write(root / "mods/Extra/Extra.mod", FileBytes(17, "gamedata.base", 1, {r}));
    // a workshop item: steamapps/workshop/content/233860/123/WS.mod
    Write(root.parent_path().parent_path() / "workshop/content/233860/123/WS.mod", FileBytes(16, "", 1, {r}));
    WriteText(root / "data/mods.cfg", "Extra.mod\nMissing.mod\nA.mod\nWS.mod\n");
    Install inst(root.string());
    EXPECT_FALSE(inst.workshop.empty());
    LoadOrder lo; Error e;
    ASSERT_TRUE(inst.LoadOrderFor(lo, e)) << e.Message();
    std::vector<std::string> names; for (auto& f : lo.files) names.push_back(f.name);
    // core: base, then B (A waits for Z), Z, A; mods: Extra (local), WS (workshop); A is a core name, Missing is not installed
    EXPECT_EQ(names, (std::vector<std::string>{"gamedata.base", "B.mod", "Z.mod", "A.mod", "Extra.mod", "WS.mod"}));
    EXPECT_EQ(lo.missing, (std::vector<std::string>{"Missing"}));
    EXPECT_TRUE(lo.files[3].core);
    EXPECT_FALSE(lo.files[4].core);
    std::string why;
    EXPECT_TRUE(inst.Check(why, false)) << why;
    EXPECT_FALSE(inst.Check(why, true)); // no kenshi*.exe, no data/gui
    GameDataContainer c;
    ASSERT_TRUE(c.Load(lo, e)) << e.Message();
    EXPECT_EQ(c.files.size(), 6u);
    EXPECT_EQ(c.records.size(), 1u);
    // a failing mod is skipped with a warning, a failing core file is an error
    WriteText(root / "mods/Extra/Extra.mod", "garbage");
    GameDataContainer c2;
    ASSERT_TRUE(c2.Load(lo, e));
    EXPECT_FALSE(c2.warnings.empty());
    WriteText(root / "data/B.mod", "garbage");
    GameDataContainer c3;
    EXPECT_FALSE(c3.Load(lo, e));
    fs::remove_all(root.parent_path().parent_path());
}

// Runs only with KENSHI_DIR=<Kenshi folder>: every FCS file of the install (core, mods/, Workshop) must parse to the end of the file.
TEST(FcsData, EveryInstalledFileParsesToEof) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    Install inst(dir);
    std::string why;
    ASSERT_TRUE(inst.Check(why, false)) << why;
    Error e;
    std::vector<std::string> paths;
    for (auto& ent : fs::directory_iterator(inst.Data()))
        if (ent.is_regular_file()) { std::string x = ent.path().extension().string(); if (x == ".base" || x == ".mod") paths.push_back(ent.path().string()); }
    auto scan = [&](const fs::path& d) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) return;
        for (auto& sub : fs::directory_iterator(d, ec))
            if (sub.is_directory()) for (auto& f : fs::directory_iterator(sub.path())) if (f.is_regular_file() && f.path().extension() == ".mod") paths.push_back(f.path().string());
    };
    scan(fs::path(inst.root) / "mods");
    if (!inst.workshop.empty()) scan(inst.workshop);
    ASSERT_FALSE(paths.empty());
    size_t records = 0;
    for (const auto& p : paths) {
        File f;
        ASSERT_TRUE(ReadFile(p, f, e)) << p << ": " << e.Message();
        EXPECT_EQ((size_t)f.header.record_count, f.records.size()) << p;
        records += f.records.size();
    }
    printf("[FcsData] %zu files, %zu records\n", paths.size(), records);
    LoadOrder lo; GameDataContainer c;
    ASSERT_TRUE(inst.LoadOrderFor(lo, e)) << e.Message();
    ASSERT_TRUE(c.Load(lo, e)) << e.Message();
    EXPECT_GT(c.records.size(), 1000u);
    printf("[FcsData] load order %zu files (%zu missing), %zu merged records, %zu warnings\n", lo.files.size(), lo.missing.size(), c.records.size(), c.warnings.size());
}
