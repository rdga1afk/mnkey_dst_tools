#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/particles/pu_script.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace md::pu;

TEST(PuScript, BlocksArgsAndNestedValues) {
    const std::string t = "system S\n{\n  technique T // comment\n  {\n    visual_particle_quota 8\n    emitter Point E\n    {\n      emission_rate dyn_random\n      {\n        min 2\n        max 9\n      }\n      direction 0 3 0\n    }\n  }\n}\n";
    std::vector<Node> n; std::string err;
    ASSERT_TRUE(Parse(t, n, err)) << err;
    ASSERT_EQ(n.size(), 1u);
    EXPECT_EQ(n[0].key, "system"); EXPECT_EQ(n[0].args[0], "S");
    const Node* tech = Find(n[0], "technique"); ASSERT_TRUE(tech);
    const Node* em = Find(*tech, "emitter"); ASSERT_TRUE(em);
    EXPECT_EQ(em->args.size(), 2u);
    const Node* rate = Find(*em, "emission_rate"); ASSERT_TRUE(rate);
    EXPECT_EQ(rate->args[0], "dyn_random");
    ASSERT_EQ(rate->children.size(), 2u);
    EXPECT_EQ(Find(*em, "direction")->args.size(), 3u);
}

TEST(PuScript, UnbalancedBracesFail) {
    std::vector<Node> n; std::string err;
    EXPECT_FALSE(Parse("system S\n{\n technique T\n {\n }\n", n, err));
    EXPECT_FALSE(Parse("system S\n{\n}\n}\n", n, err));
}

// Every install particle script parses (QUEUE 9b-6 step "parser"); prints the keyword census. Needs KENSHI_DIR.
TEST(PuScript, EveryInstallScriptParses) {
    const char* kd = std::getenv("KENSHI_DIR");
    const std::filesystem::path dir = kd ? std::filesystem::path(kd) / "data/particles/scripts" : std::filesystem::path();
    std::error_code ec;
    if (!kd || !std::filesystem::exists(dir, ec)) GTEST_SKIP() << "needs a Kenshi install (KENSHI_DIR)";
    int files = 0, systems = 0, techniques = 0, renderers = 0;
    std::map<std::string, int> kinds;   // emitter / affector / observer / handler / behaviour class names
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        std::string ext = e.path().extension().string(); for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".pu") continue;
        std::ifstream f(e.path()); std::stringstream ss; ss << f.rdbuf();
        std::vector<Node> n; std::string err;
        ASSERT_TRUE(Parse(ss.str(), n, err)) << e.path().filename() << ": " << err;
        ++files;
        std::vector<const Node*> stack;
        for (const Node& s : n) if (s.key == "system") { ++systems; stack.push_back(&s); }
        while (!stack.empty()) {
            const Node* x = stack.back(); stack.pop_back();
            if (x->key == "technique") ++techniques;
            if (x->key == "renderer") ++renderers;
            if ((x->key == "emitter" || x->key == "affector" || x->key == "observer" || x->key == "handler" || x->key == "behaviour") && !x->args.empty()) ++kinds[x->key + " " + x->args[0]];
            for (const Node& c : x->children) stack.push_back(&c);
        }
    }
    printf("[PuScript] %d scripts, %d systems, %d techniques, %d renderers, %zu distinct emitter/affector/observer/handler/behaviour classes\n", files, systems, techniques, renderers, kinds.size());
    EXPECT_GE(files, 90);
    EXPECT_EQ(techniques, renderers);   // inventory: 200 techniques, 200 renderers
}
