// CI test of md::character::Session on a SYNTHETIC install: the records (two race groups, three races, a hair, a head, an idle stance) and the slider limits are built here in memory,
// no Kenshi files (the public CI has none). It checks the logic of the editor buttons, not Kenshi's data: RACE / SUBRACE / GENDER cycling, the slider set following the selection,
// RESET to the mids, RANDOMISE (seeded, in range), IDLE STANCE / HEAD / HAIR indices, EXPORT -> RESET -> IMPORT. usage: md_session_synthetic_test
#include "appearance_io.h"
#include "session.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace md::character;
using md::fcs::GameData;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL session_synthetic: " __VA_ARGS__); std::printf("\n"); return 1; } } while (0)

static GameData& Add(md::fcs::GameDataContainer& c, int kind, const char* id, const char* name) {
    GameData g; g.kind = kind; g.string_id = id; g.name = name;
    c.order.push_back(id);
    return c.records[id] = g;
}
static void Ref(GameData& g, const char* cat, std::initializer_list<const char*> targets) {
    std::vector<std::pair<std::string, std::array<int32_t, 3>>> v;
    for (const char* t : targets) v.push_back({t, {0, 0, 0}});
    g.references.push_back({cat, v});
}
static Range R(const char* name, int lo, int hi, int mid) { Range r; r.name = name; r.min = lo; r.max = hi; r.has_mid = true; r.mid = mid; r.has_random_variation = true; r.random_variation = 30; return r; }
static Config Cfg(Gender g, int face_sliders, int body_sliders) {
    Config c; c.gender = g;
    Category face; face.name = "Face"; for (int i = 0; i < face_sliders; ++i) face.ranges.push_back(R(("Face" + std::to_string(i)).c_str(), 0, 200, 100));
    Category body; body.name = "Body"; for (int i = 0; i < body_sliders; ++i) body.ranges.push_back(R(("Body" + std::to_string(i)).c_str(), 50, 150, 100));
    c.categories = {face, body};
    return c;
}

int main() {
    md::fcs::GameDataContainer c;
    // idle stances (ANIMATION records, type 24): `idle_stand_normal` is the default and not the first
    for (const char* n : {"idle_stand_a", "idle_stand_normal", "idle_stand_c"}) Add(c, 24, n, n).strings["anim name"] = n;
    // a head with a texture map, a hair (attach slot 0) and one hair colour
    Add(c, 3, "head1", "Head 1").files["texture map"] = "head1.dds";
    { GameData& h = Add(c, 6, "hair1", "Short"); h.files["mesh"] = "hair1.mesh"; h.ints["attach slot"] = 0; }
    { GameData& h = Add(c, 6, "hair2", "Long"); h.files["mesh"] = "hair2.mesh"; h.ints["attach slot"] = 0; }
    Add(c, 8, "col1", "Brown").ints["color 1"] = (120 << 16) | (80 << 8) | 40;
    for (const char* id : {"raceA", "raceB"}) {
        GameData& r = Add(c, kItemRace, id, id[4] == 'A' ? "Greenlander" : "Scorchlander");
        r.bools["playable"] = true; r.files["editor limits"] = "editor_data_human.xml";
        Ref(r, "heads male", {"head1"}); Ref(r, "heads female", {"head1"}); Ref(r, "hairs", {"hair1", "hair2"}); Ref(r, "hair colors", {"col1"});
    }
    { GameData& r = Add(c, kItemRace, "raceS", "Bones"); r.bools["playable"] = true; r.bools["single gender"] = true; r.files["editor limits"] = "editor_data_skeleton.xml"; }
    { GameData& g = Add(c, kItemRaceGroup, "groupH", "Human"); Ref(g, "races", {"raceA", "raceB"}); }
    { GameData& g = Add(c, kItemRaceGroup, "groupS", "Skeleton"); Ref(g, "races", {"raceS"}); }

    EditorLimits human; human.file = "editor_data_human.xml"; human.configs = {Cfg(Gender::Male, 3, 2), Cfg(Gender::Female, 4, 3)};
    EditorLimits skel; skel.file = "editor_data_skeleton.xml"; skel.configs = {Cfg(Gender::Single, 1, 1)};

    Session s; std::string err;
    CHECK(s.LoadFromData(std::move(c), {human, skel}, err), "load: %s", err.c_str());
    CHECK(s.RaceCount() == 2 && s.Race(0).name == "Skeleton" && s.Race(1).name == "Human", "RACE order: Skeleton first, Human second");
    CHECK(s.Race(s.race_index()).name == "Human", "the session starts on Human");
    CHECK(s.SubraceCount() == 2 && s.Subrace(0).name == "Greenlander", "Human has 2 subraces");
    CHECK(s.gender() == Gender::Male && !s.SingleGender(), "male by default");

    // GENDER: the slider set follows (male 3 + 2, female 4 + 3); RESET puts every slider at its mid
    auto count = [&](const char* cat) { const Category* k = s.Limits() ? s.Limits()->Find(cat) : nullptr; return k ? int(k->ranges.size()) : -1; };
    CHECK(count("Face") == 3 && count("Body") == 2, "male sliders %d + %d", count("Face"), count("Body"));
    uint64_t rev = s.revision();
    s.SetGender(Gender::Female);
    CHECK(s.gender() == Gender::Female && count("Face") == 4 && count("Body") == 3 && s.revision() > rev, "female sliders %d + %d", count("Face"), count("Body"));
    CHECK(int(s.Value(s.Limits()->Find("Face")->ranges[0])) == 100, "a fresh slider is at its mid");

    // SUBRACE
    s.SelectSubrace(1);
    CHECK(s.subrace_index() == 1 && s.Current().name == "Scorchlander", "SUBRACE 2");
    s.SelectSubrace(9);
    CHECK(s.subrace_index() == 1, "an out-of-range SUBRACE is ignored");

    // RANDOMISE: seeded (same seed, same values), values inside the range, RESET undoes it
    s.Reset(); s.Seed(77); s.RandomiseCategory("Face");
    std::map<std::string, float> first = s.Values().values;
    bool changed = false;
    for (const Range& r : s.Limits()->Find("Face")->ranges) { float v = s.Value(r); CHECK(v >= r.min && v <= r.max, "%s = %.1f out of [%d, %d]", r.name.c_str(), v, r.min, r.max); if (int(v) != r.mid) changed = true; }
    CHECK(changed, "RANDOMISE (Face) changes at least one slider");
    s.Reset(); s.Seed(77); s.RandomiseCategory("Face");
    CHECK(s.Values().values == first, "the same seed gives the same values");
    s.Reset();
    for (const Range& r : s.Limits()->Find("Face")->ranges) CHECK(int(s.Value(r)) == r.mid, "RESET puts %s at its mid", r.name.c_str());

    // IDLE STANCE: default idle_stand_normal (index 1), the arrows wrap around
    CHECK(s.StanceCount() == 3 && s.StanceName(s.stance()) == "idle_stand_normal", "default stance idle_stand_normal");
    s.SelectStance(int(s.stance()) + 2); CHECK(s.stance() == 0, "stance wraps forward");
    s.SelectStance(-1); CHECK(s.stance() == 2, "stance wraps backward");
    // HEAD / HAIR
    CHECK(s.HeadCount() == 1 && s.HairCount() == 2 && s.BeardCount() == 1, "head %zu hair %zu beard %zu", s.HeadCount(), s.HairCount(), s.BeardCount());
    s.SelectHair(1); CHECK(s.hair_index() == 1, "hair 2"); s.SelectHair(7); CHECK(s.hair_index() == 1, "out-of-range hair ignored");

    // single-gender race: GENDER is a no-op, the sliders come from its own file
    s.SelectRace(0);
    CHECK(s.Race(s.race_index()).name == "Skeleton" && s.SingleGender() && s.gender() == Gender::Single, "Skeleton is single-gender");
    s.SetGender(Gender::Female); CHECK(s.gender() == Gender::Single, "GENDER does nothing for a single-gender race");
    CHECK(count("Face") == 1 && count("Body") == 1, "Skeleton sliders");

    // EXPORT -> change -> IMPORT restores the exported state
    s.SelectRace(1); s.SetGender(Gender::Male); s.Reset(); s.Seed(5); s.RandomiseCategory("Body"); s.SelectStance(1); s.SelectHair(1);
    std::map<std::string, float> want = s.Values().values; size_t want_stance = s.stance(), want_hair = s.hair_index();
    const std::string path = (std::filesystem::temp_directory_path() / "md_session_synthetic.bod2").string();
    CHECK(ExportAppearance(s, path, err), "export: %s", err.c_str());
    s.Reset(); s.SelectStance(1); s.SelectHair(0);
    CHECK(s.Values().values != want, "RESET differs from the exported state");
    CHECK(ImportAppearance(s, path, err), "import: %s", err.c_str());
    CHECK(s.Values().values == want && s.stance() == want_stance && s.hair_index() == want_hair, "IMPORT restores values, stance and hair");
    std::filesystem::remove(path);
    CHECK(!ImportAppearance(s, path, err), "IMPORT of a missing file fails");
    std::printf("PASS session_synthetic: races, gender, subrace, sliders, reset, randomise, stance, head / hair, export / import\n");
    return 0;
}
