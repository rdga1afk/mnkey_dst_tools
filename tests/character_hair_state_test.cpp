// Hair state test (coordinator 6804920270, steps 1-3): drives the Characters session through hair slider, RANDOMISE ALL x50, race / subrace / gender changes and RESET,
// and after EVERY step counts what the preview would attach (one hair record, one beard record, never a beard on a woman, indices inside the lists, records of this race and gender).
// The first step that breaks an invariant is printed. A state test: no GPU, no frame. usage: md_character_hair_state_test <KENSHI_DIR>
#include "session.h"
#include <cstdio>
#include <map>
#include <string>

using namespace md::character;
static int fails = 0; static long steps = 0; static std::string last;
static long overlay_with_mesh = 0, overlay_only = 0, mesh_only = 0, female_inherits_overlay = 0;   // by (record, gender) pair, counted once per step-visited pair
static std::map<std::string, std::string> both;   // records that define a mesh AND a head-skin overlay: id -> mesh | overlay
// the head-skin overlay the preview binds = AttachmentInfo::HeadTexture(female) of the CURRENT hair record; here it is re-read from the raw FCS record (an independent path) and compared
static std::string RawOverlay(const Session& s, const AttachmentInfo& a, bool fem) {
    const md::fcs::GameData* g = s.Data().Get(a.id); if (!g) return "<no record>";
    auto f = [&](const char* k) { auto it = g->files.find(k); return it == g->files.end() ? std::string() : it->second; };
    const std::string fe = f("head texture female"), ma = f("head texture");
    return fem && !fe.empty() ? fe : ma;
}
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
    if (s.HairCount()) {   // (b) the overlay in use is the current record's, by the raw FCS data; "none" is valid
        const AttachmentInfo& a = s.Hair(s.hair_index());
        const std::string want = RawOverlay(s, a, fem), got = a.HeadTexture(fem), style = a.id + (fem ? "|f" : "|m");
        if (want != got) bad(("overlay differs from the raw record " + a.id + ": want '" + want + "' got '" + got + "'").c_str());
        // a changed style with a different raw overlay must change what is bound: both are the record's own value, so a stale value would show as want != got above
                const bool mesh = a.HasMesh(fem), ov = !want.empty();
        if (mesh && ov) { ++overlay_with_mesh; both[a.id + (fem ? " F" : " M")] = (fem && !a.mesh_female.empty() ? a.mesh_female : a.mesh) + " | " + want + " ch" + std::to_string(a.HeadChannel(fem)) + "/a" + std::to_string(fem ? a.head_alpha_channel_female : a.head_alpha_channel); } else if (ov) ++overlay_only; else if (mesh) ++mesh_only;
        if (fem && a.head_texture_female.empty() && !a.head_texture.empty() && (a.HasMesh(true))) ++female_inherits_overlay;
    }
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
    for (const auto& kv : both) std::printf("BOTH %s: %s\n", kv.first.c_str(), kv.second.c_str());
    std::printf("OVERLAY: step-visits with mesh+overlay %ld, overlay only (scalp style) %ld, mesh only %ld, woman inherits the MAN's overlay %ld\n", overlay_with_mesh, overlay_only, mesh_only, female_inherits_overlay);
    std::printf("HAIR STATE: %ld steps, %d violations; RESET left a non-default hairstyle in %ld of %ld cases (Session::Reset resets sliders, stance and head only)\n", steps, fails, resets_keep_hair, resets);
    return fails ? 1 : 0;
}
