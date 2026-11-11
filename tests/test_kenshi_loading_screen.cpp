// The loading screen's picking rule and data (docs/research/kenshi_specs/loading_screen.md): the exe's rand() helper formula on the MSVC generator, the by-name group lookup, and the real install files when KENSHI_DIR is set.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/io/kenshi_loading_screen.h>
#include <filesystem>
#include <fstream>

using namespace md::kenshi;

TEST(KenshiLoadingScreen, MsvcRandSeed1) {
    MsvcRand r(1);
    const int expect[5] = { 41, 18467, 6334, 26500, 19169 };   // the C runtime's well-known first values for seed 1
    for (int v : expect) EXPECT_EQ(r.Next(), v);
}

TEST(KenshiLoadingScreen, PickIndexFormula) {
    MsvcRand r(1); int idx[5];
    for (int& i : idx) i = PickIndex(r.Next(), 8);
    EXPECT_EQ(idx[0], 0); EXPECT_EQ(idx[1], 4); EXPECT_EQ(idx[2], 1); EXPECT_EQ(idx[3], 6); EXPECT_EQ(idx[4], 4);
    EXPECT_EQ(PickIndex(32767, 8), 7);   // the largest rand() value stays inside the list
    EXPECT_EQ(PickIndex(0, 1), 0);
}

TEST(KenshiLoadingScreen, SyntheticFiles) {
    const auto root = std::filesystem::temp_directory_path() / "md_load_scr";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "data/gui/images");
    std::ofstream(root / "data/gui/images/kenshi_images.xml") <<
        "<Resource type=\"ResourceImageSet\" name=\"Other\"><Group name=\"X\" texture=\"x.dds\"/></Resource>\n"
        "<Resource type=\"ResourceImageSet\" name=\"Kenshi_LoadingScreen\">\n<Group name=\"LoadingScreen1\" texture=\"a.dds\" size=\"1 1\">\n<Group name=\"LoadingScreen6\" texture=\"f.dds\">\n<Group name=\"LoadingScreen6\" texture=\"g.dds\">\n</Resource>";
    std::ofstream(root / "data/tips.txt", std::ios::binary) << "one\r\ntwo\\nthree\r\n\r\n";
    LoadingScreenData d;
    ASSERT_TRUE(LoadLoadingScreenData(root.string(), d));
    ASSERT_EQ(d.group_names.size(), 3u);
    EXPECT_EQ(d.TextureFor(0), "a.dds");
    EXPECT_EQ(d.TextureFor(2), "f.dds");   // the repeated name resolves to the first group of that name
    ASSERT_EQ(d.tips.size(), 2u);
    EXPECT_EQ(d.tips[1], "two\nthree");
    std::filesystem::remove_all(root);
}

TEST(KenshiLoadingScreen, RealInstall) {
    if (md::KenshiDir().empty()) GTEST_SKIP() << "needs KENSHI_DIR";
    LoadingScreenData d;
    if (!LoadLoadingScreenData(md::KenshiDir(), d)) GTEST_SKIP() << "the install has no loading-screen data (partial copy)";
    ASSERT_EQ(d.group_names.size(), 8u);
    EXPECT_EQ(d.TextureFor(5), "loading_screen6.dds");
    EXPECT_EQ(d.TextureFor(6), "loading_screen6.dds");   // loading_screen9 / 10 are never shown
    EXPECT_EQ(d.TextureFor(7), "loading_screen6.dds");
    EXPECT_EQ(d.tips.size(), 62u);
}
