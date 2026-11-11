// #159 phase 3 logic: md::character::Session on the install: the RACE / SUBRACE / GENDER lists come from the FCS data, the slider set follows the selection (Human male 23 Face + 16 Body
// ranges, female 26 + 19, single-gender races have no gender), RESET gives the mids, a race change resets the values. usage: md_character_session_test <KENSHI_DIR>
#include "session.h"
#include <cstdio>
#include <set>

using namespace md::character;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL session: " __VA_ARGS__); std::printf("\n"); return 1; } } while (0)

static int Count(const Config* c, const char* cat) { const Category* k = c ? c->Find(cat) : nullptr; return k ? int(k->ranges.size()) : -1; }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    Session s; std::string err;
    CHECK(s.Load(argv[1], err), "load: %s", err.c_str());
    std::set<std::string> names;
    for (size_t i = 0; i < s.RaceCount(); ++i) names.insert(s.Race(i).name);
    for (const char* n : {"Human", "Hive", "Skeleton", "Shek"}) CHECK(names.count(n), "RACE list lacks %s", n);
    size_t human = 0; for (size_t i = 0; i < s.RaceCount(); ++i) if (s.Race(i).name == "Human") human = i;
    CHECK(s.Race(s.race_index()).name == "Human", "the editor starts on Human");
    CHECK(s.Race(0).name == "Skeleton" && s.Race(1).name == "Hive" && s.Race(2).name == "Shek" && s.Race(3).name == "Human", "RACE order from the video frames");
    s.SelectRace(human);
    std::set<std::string> subs; for (size_t i = 0; i < s.SubraceCount(); ++i) subs.insert(s.Subrace(i).name);
    CHECK(subs.count("Greenlander") && subs.count("Scorchlander"), "Human lacks Greenlander / Scorchlander");
    size_t green = 0; for (size_t i = 0; i < s.SubraceCount(); ++i) if (s.Subrace(i).name == "Greenlander") green = i;
    s.SelectSubrace(green);
    CHECK(!s.SingleGender() && s.gender() == Gender::Male, "Greenlander gender");
    CHECK(Count(s.Limits(), "Face") == 23 && Count(s.Limits(), "Body") == 16, "male Face %d Body %d", Count(s.Limits(), "Face"), Count(s.Limits(), "Body"));
    s.SetGender(Gender::Female);
    CHECK(s.gender() == Gender::Female && Count(s.Limits(), "Face") == 26 && Count(s.Limits(), "Body") == 19, "female Face %d Body %d", Count(s.Limits(), "Face"), Count(s.Limits(), "Body"));
    const Range& h = s.Limits()->Find("Body")->ranges[0];
    s.RandomiseAll(); s.SetValue(h.name, 1234.f);
    s.Reset();
    for (const Category& c : s.Limits()->categories) for (const Range& r : c.ranges) CHECK(s.Value(r) == float(RangeMid(r)), "RESET %s", r.name.c_str());
    s.SetValue(h.name, 77.f); s.SelectRace(human);
    CHECK(s.Value(h) == float(RangeMid(h)), "race change keeps values");
    size_t hive = 0; for (size_t i = 0; i < s.RaceCount(); ++i) if (s.Race(i).name == "Hive") hive = i;
    s.SelectRace(hive);
    CHECK(s.SingleGender() && s.gender() == Gender::Single, "Hive is single gender");
    CHECK(s.Limits() != nullptr, "Hive has editor limits");
    std::printf("PASS data session: %zu races, Human %zu subraces, Greenlander male 23+16 / female 26+19 ranges, Hive single gender\n", s.RaceCount(), size_t(subs.size()));
    return 0;
}
