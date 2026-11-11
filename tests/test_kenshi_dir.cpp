// Install detection (engine/src/io/kenshi_dir.cpp): Steam native, Flatpak Steam, extra library folders from libraryfolders.vdf, the saved picker path. A synthetic home folder, no Kenshi data.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
struct Home {
    fs::path root;
    Home() { root = fs::temp_directory_path() / ("md_kdir_" + std::to_string(::getpid())); fs::remove_all(root); fs::create_directories(root); }
    ~Home() { fs::remove_all(root); }
    void Install(const fs::path& dir) { fs::create_directories(dir / "data"); std::ofstream(dir / "data/gamedata.base") << "x"; }
    void Write(const fs::path& f, const std::string& text) { fs::create_directories(f.parent_path()); std::ofstream(f) << text; }
};
}

TEST(KenshiDir, NativeSteam) {
    Home h; h.Install(h.root / ".steam/steam/steamapps/common/Kenshi");
    const auto r = md::FindSteamKenshi(h.root.string());
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], (h.root / ".steam/steam/steamapps/common/Kenshi").string());
}

TEST(KenshiDir, FlatpakSteam) {
    Home h; h.Install(h.root / ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Kenshi");
    const auto r = md::FindSteamKenshi(h.root.string());
    ASSERT_EQ(r.size(), 1u);
    EXPECT_NE(r[0].find("com.valvesoftware.Steam"), std::string::npos);
}

TEST(KenshiDir, OtherLibraryFromVdf) {
    Home h; fs::path lib = h.root / "games/SteamLibrary";
    h.Install(lib / "steamapps/common/Kenshi");
    fs::create_directories(h.root / ".local/share/Steam/steamapps");
    h.Write(h.root / ".local/share/Steam/steamapps/libraryfolders.vdf", "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + (h.root / ".local/share/Steam").string() + "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + lib.string() + "\"\n\t}\n}\n");
    const auto r = md::FindSteamKenshi(h.root.string());
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], (lib / "steamapps/common/Kenshi").string());
}

TEST(KenshiDir, FolderWithoutGamedataIsNotAnInstall) {
    Home h; fs::create_directories(h.root / ".steam/steam/steamapps/common/Kenshi/data");
    EXPECT_TRUE(md::FindSteamKenshi(h.root.string()).empty());
}

TEST(KenshiDir, SavePickedFolder) {
    Home h; fs::path k = h.root / "elsewhere/Kenshi"; h.Install(k);
    EXPECT_TRUE(md::SaveKenshiDir(k.string(), h.root.string()));
    std::ifstream f(h.root / ".config/monkeydust/kenshi_dir"); std::string line; std::getline(f, line);
    EXPECT_EQ(line, k.string());
    EXPECT_FALSE(md::SaveKenshiDir((h.root / "nothing").string(), h.root.string()));
}

TEST(KenshiDir, SavedFolderWithSpacesAndCyrillicRoundTrips) {
    Home h; fs::path k = h.root / u8"Ігри/Steam Library/steamapps/common/Kenshi"; h.Install(k);
    ASSERT_TRUE(md::SaveKenshiDir(k.string(), h.root.string()));
    EXPECT_EQ(md::SavedKenshiDir(h.root.string()), k.string());
}

TEST(KenshiDir, FolderSavedUnderTheOldNameIsTakenOverOnce) {
    Home h; fs::path k = h.root / "Kenshi"; h.Install(k);
    fs::create_directories(h.root / ".config/monkey_dust");
    { std::ofstream f(h.root / ".config/monkey_dust/kenshi_dir"); f << k.string() << "\n"; }
    EXPECT_EQ(md::SavedKenshiDir(h.root.string()), k.string());
    EXPECT_TRUE(fs::exists(h.root / ".config/monkeydust/kenshi_dir"));
    fs::remove(h.root / ".config/monkey_dust/kenshi_dir");   // the old file is no longer needed
    EXPECT_EQ(md::SavedKenshiDir(h.root.string()), k.string());
}

TEST(KenshiDir, SupportedVersionOnly1_0_68) {
    Home h; const fs::path d = h.root / "k"; h.Install(d);
    EXPECT_FALSE(md::KenshiVersionWarning(d.string()).empty());                 // no currentVersion.txt: unknown, warns
    h.Write(d / "currentVersion.txt", "Kenshi 1.0.68 - x64 (Newland)\r\n");
    EXPECT_EQ(md::KenshiVersionWarning(d.string()), "");
    h.Write(d / "currentVersion.txt", "Kenshi 1.0.64 - x64\n");
    const std::string w = md::KenshiVersionWarning(d.string());
    EXPECT_NE(w.find("1.0.64"), std::string::npos);
    EXPECT_NE(w.find("1.0.68"), std::string::npos);
}
