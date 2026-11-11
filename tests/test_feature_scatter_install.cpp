// The placement text built from the install's features.dat holds every line of the committed game/data/features_scatter.txt (private/md_features_parse.py --export) byte for byte, plus 29 placements the old export could not resolve (Tarsands_* meshes: the merged game data names them, the single mod file did not). Needs KENSHI_DIR and the repo as the working folder.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/feature_scatter.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

TEST(FeatureScatterInstall, CoversCommittedFile) {
    if (md::KenshiDir().empty() || !std::filesystem::exists("game/data/features_scatter.txt")) GTEST_SKIP() << "needs KENSHI_DIR and the repo as working folder";
    const std::string built = BuildFeatureScatterText(md::KenshiDir());
    ASSERT_FALSE(built.empty());
    auto lines = [](const std::string& t) { std::vector<std::string> v; std::istringstream s(t); std::string l; while (std::getline(s, l)) if (!l.empty() && l[0] != '#') v.push_back(l); return v; };
    std::ifstream f("game/data/features_scatter.txt"); std::stringstream ss; ss << f.rdbuf();
    const auto a = lines(built), b = lines(ss.str());
    const std::set<std::string> in_built(a.begin(), a.end());
    int missing = 0;
    for (const std::string& l : b) if (!in_built.count(l) && missing++ < 5) ADD_FAILURE() << "committed line not built: " << l;
    EXPECT_EQ(missing, 0);
    EXPECT_EQ(a.size() - b.size(), 29u);
}
