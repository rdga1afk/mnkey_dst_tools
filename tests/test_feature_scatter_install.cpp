// The placement text built from the game folder's features.dat (BuildFeatureScatterText) against its hash reference (tests/data/install_hashes.tsv; DE hash references, coordinator 18.11).
// No copy of the output is committed (#201: Kenshi-derived files are never committed); the hash is the guard.
#include "install_hash.h"
#include <monkey_dust/world/feature_scatter.h>
#include <sstream>

TEST(FeatureScatterInstall, OutputMatchesHashReference) {
    ASSERT_FALSE(md::KenshiDir().empty()) << "needs KENSHI_DIR";
    const std::string built = BuildFeatureScatterText(md::KenshiDir());
    ASSERT_FALSE(built.empty());
    size_t lines = 0; std::istringstream s(built); std::string l;
    while (std::getline(s, l)) if (!l.empty() && l[0] != '#') ++lines;
    EXPECT_GT(lines, 500u);   // a structural floor next to the hash
    MD_EXPECT_INSTALL_HASH("feature_scatter_text", built.data(), built.size());
}
