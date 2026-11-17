#include <gtest/gtest.h>
#include <monkey_dust/particles/pu_system.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace md::pu;

namespace {

const char* kScript = R"(system S
{
    technique T
    {
        visual_particle_quota 100
        material m
        default_particle_width 2
        renderer Billboard
        {
            billboard_rotation_type vertex
        }
        emitter SphereSurface E
        {
            emission_rate 100
            angle 0
            time_to_live 1
            velocity 10
            radius 5
            colour 1 0.5 0.25 1
        }
        affector Scale Sc
        {
            xyz_scale 4
        }
        affector LinearForce F
        {
            force_vector 0 10 0
        }
    }
}
)";

bool Load(const std::string& text, SystemDef& d, std::string& err) {
    std::vector<Node> n;
    if (!Parse(text, n, err)) return false;
    return LoadSystem(n[0], d, err);
}

}  // namespace

TEST(PuSystem, EmitsAtRateAndExpiresAtTtl) {
    SystemDef d; std::string err;
    ASSERT_TRUE(Load(kScript, d, err)) << err;
    uint32_t seed = 1;
    static Instance inst;
    inst.Init(d, 0, MsvcRng(&seed));
    inst.Update(0.1f);
    EXPECT_EQ(inst.Count(), 10);                       // 100/s x 0.1 s
    const Particle& p = inst.Get(0);
    EXPECT_NEAR(std::sqrt(p.pos.x * p.pos.x + p.pos.y * p.pos.y + p.pos.z * p.pos.z), 5.0f, 1e-3f);   // on the sphere: no motion in the emission frame (j=0)
    for (int i = 0; i < 9; ++i) inst.Update(0.1f);     // t = 1.0 s: the first particles (ttl 1) are gone
    EXPECT_LT(inst.Count(), 100);
    EXPECT_GT(inst.Count(), 80);
    for (int i = 0; i < inst.Count(); ++i) EXPECT_LE(inst.Get(i).ttl, 1.0f);
}

TEST(PuSystem, ScaleAndForceApplyPerSecond) {
    SystemDef d; std::string err;
    ASSERT_TRUE(Load(kScript, d, err)) << err;
    d.techniques[0].emitters[0].rate.value = 10;
    uint32_t seed = 7;
    static Instance inst;
    inst.Init(d, 0, MsvcRng(&seed));
    inst.Update(0.1f);                                  // one particle, born this frame
    ASSERT_EQ(inst.Count(), 1);
    EXPECT_NEAR(inst.Get(0).w, 2.0f + 4.0f * 0.1f, 1e-5f);   // default width 2 + 4/s x dt
    EXPECT_NEAR(inst.Get(0).dir.y, 10.0f + 10.0f * 0.1f, 1e-4f + 0.0f);   // no angle: direction (0,1,0) x velocity 10, + force x dt
}

TEST(PuSystem, UnsupportedPartsAreNamed) {
    SystemDef d; std::string err;
    EXPECT_FALSE(Load("system S\n{\n technique T\n {\n  visual_particle_quota 5\n  emitter Box B\n  {\n  }\n }\n}\n", d, err));
    EXPECT_NE(err.find("unsupported type Box"), std::string::npos) << err;
}

TEST(PuSystem, CurvedLinearMatchesNearestPointRule) {
    Dyn c; c.type = Dyn::CurvedLinear; c.n = 2; c.cx[0] = 0; c.cy[0] = 0.5f; c.cx[1] = 1; c.cy[1] = 60.5f;
    Rng r;
    EXPECT_NEAR(c.Eval(0.5f, r), 30.5f, 1e-4f);
    EXPECT_NEAR(c.Eval(1.0f, r), 60.5f, 1e-4f);        // at or past the last point: its value
    EXPECT_NEAR(c.Eval(-1.0f, r), 0.5f - 60.0f, 1e-3f);   // before the first point: the first segment extended
}

// The three weather scripts and the ground dust load; the rest of the install needs more emitters / affectors (counted, not required).
TEST(PuSystem, SandstormScriptLoads) {
    const char* kd = std::getenv("KENSHI_DIR");
    if (!kd) GTEST_SKIP() << "needs a Kenshi install (KENSHI_DIR)";
    const std::filesystem::path f = std::filesystem::path(kd) / "data/particles/scripts/kenshi_sandstorm_wall1.pu";
    std::ifstream in(f);
    if (!in) GTEST_SKIP() << "no " << f;
    std::stringstream ss; ss << in.rdbuf();
    SystemDef d; std::string err;
    ASSERT_TRUE(Load(ss.str(), d, err)) << err;
    ASSERT_EQ(d.n_techniques, 2);
    EXPECT_EQ(d.techniques[0].n_emitters, 2);
    EXPECT_EQ(d.techniques[0].n_affectors, 4);
    EXPECT_EQ(d.techniques[1].lod_index, 1);
    uint32_t seed = 3;
    static Instance inst;
    inst.Init(d, 0, MsvcRng(&seed));
    for (int i = 0; i < 100; ++i) inst.Update(0.05f);   // 5 s: emission 200/s, ttl 3..5 s, quota 1500
    EXPECT_GT(inst.Count(), 500);
    EXPECT_LE(inst.Count(), 1500);
}
