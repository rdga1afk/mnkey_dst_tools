#include <gtest/gtest.h>
#include <monkey_dust/io/data_files.h>
#include <fstream>
#include <sstream>
#include <filesystem>

static std::string Slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

TEST(DataFiles, NotEmbeddedIsReturnedUnchanged) { EXPECT_EQ(md::DataPath("game/data/not_ours.txt"), "game/data/not_ours.txt"); }

TEST(DataFiles, EmbeddedPathIsWrittenToCacheAndOpens) {
    setenv("XDG_CACHE_HOME", "/tmp/md_datafiles_test_cache", 1);
    const std::string p = md::DataPath("game/data/textures/_MD_Flat_Normal_NML.dds");
    ASSERT_FALSE(p.empty());
    EXPECT_NE(p.find("/tmp/md_datafiles_test_cache/monkeydust/"), std::string::npos);
    EXPECT_TRUE(Slurp(p) == md::FlatNormalDds());
}

// RepoPath: the private root (main) overrides the tools root; a file only in the tools root is found there; neither -> the relative path unchanged.
TEST(DataFiles, RepoPathLookupOrder) {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "md_repopath_test";
    fs::remove_all(base);
    fs::create_directories(base / "priv/game/data"); fs::create_directories(base / "tools/game/data");
    { std::ofstream(base / "priv/game/data/both.txt") << "priv"; std::ofstream(base / "tools/game/data/both.txt") << "tools"; std::ofstream(base / "tools/game/data/only_tools.txt") << "t"; }
    const std::string priv = (base / "priv").string(), tools = (base / "tools").string();
    EXPECT_EQ(Slurp(md::RepoPathIn("game/data/both.txt", priv, tools)), "priv");
    EXPECT_EQ(Slurp(md::RepoPathIn("game/data/only_tools.txt", priv, tools)), "t");
    EXPECT_EQ(md::RepoPathIn("game/data/only_tools.txt", "", tools), (base / "tools/game/data/only_tools.txt").string());   // no private root: tools only works
    EXPECT_EQ(md::RepoPathIn("game/data/missing.txt", priv, tools), "game/data/missing.txt");
    fs::remove_all(base);
}
