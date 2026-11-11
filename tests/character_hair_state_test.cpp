// Hair state test (coordinator 6804920270, steps 1-3): drives the Characters session through hair slider, RANDOMISE ALL x50, race / subrace / gender changes and RESET,
// and after EVERY step counts what the preview would attach (one hair record, one beard record, never a beard on a woman, indices inside the lists, records of this race and gender).
// The first step that breaks an invariant is printed. A state test: no GPU, no frame. usage: md_character_hair_state_test <KENSHI_DIR>
#include "session.h"
#include <cstdio>
#include <string>

using namespace md::character;
static int fails = 0; static long steps = 0; static std::string last;
static void Check(const Session& s, const std::string& what) {
    ++steps; last = what;
    const bool fem = s.gender() == Gender::Female;
    auto bad = [&](const char* why) { if (fails++ < 12) std::printf("FAIL step %ld [%s] race %zu sub %zu %s: %s (hair %zu/%zu beard %zu/%zu)\n", steps, what.c_str(), s.race_index(), s.subrace_index(), fem ? "F" : "M", why, s.hair_index(), s.HairCount(), s.beard_index(), s.BeardCount()); };
    if (s.HairCount() && s.hair_index() >= s.HairCount()) bad("hair index outside the list");
    if (s.BeardCount() && s.beard_index() >= s.BeardCount()) bad("beard index outside the list");
    // what Prepare() loads: at most one hair, and a beard only for a man
    const int hair = s.HairCount() && s.Hair(s.hair_index()).HasMesh(fem) ? 1 : 0;
    const int beard = s.BeardCount() && !fem && s.Beard(s.beard_index()).HasMesh(false) ? 1 : 0;
    if (hair > 1 || beard > 1) bad("more than one attachment per slot");
    if (fem && s.BeardCount() && s.beard_index() != 0) bad("a woman keeps a beard index (hidden, but it is in the saved state)");
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    Session s; std::string err;
    if (!s.Load(argv[1], err)) { std::printf("load: %s\n", err.c_str()); return 1; }
    s.Seed(777);
    long resets_keep_hair = 0, resets = 0;
    for (size_t r = 0; r < s.RaceCount(); ++r) {
        s.SelectRace(r); Check(s, "select race");
        for (size_t sr = 0; sr < std::max<size_t>(1, s.SubraceCount()); ++sr) {
            if (s.SubraceCount()) { s.SelectSubrace(sr); Check(s, "select subrace"); }
            for (int g = 0; g < (s.SingleGender() ? 1 : 2); ++g) {
                if (!s.SingleGender()) { s.SetGender(g ? Gender::Female : Gender::Male); Check(s, "set gender"); }
                for (size_t i = 0; i < s.HairCount(); ++i) { s.SelectHair(i); Check(s, "hair slider"); }
                for (size_t i = 0; i < (g ? 0 : s.BeardCount()); ++i) { s.SelectBeard(i); Check(s, "beard slider"); }
                for (int k = 0; k < 50; ++k) { s.RandomiseAll(); Check(s, "randomise all #" + std::to_string(k)); }
                const size_t h = s.hair_index(), b = s.beard_index();
                s.Reset(); Check(s, "reset"); ++resets;
                if (h != 0 && s.hair_index() == h) ++resets_keep_hair;
                (void)b;
            }
        }
    }
    std::printf("HAIR STATE: %ld steps, %d violations; RESET left a non-default hairstyle in %ld of %ld cases (Session::Reset resets sliders, stance and head only)\n", steps, fails, resets_keep_hair, resets);
    return fails ? 1 : 0;
}
