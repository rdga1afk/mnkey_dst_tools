// BuildHumanBody (the NPC body from the game folder: human_male.mesh + male_skeleton.skeleton) against its hash reference (tests/data/install_hashes.tsv): every vertex, weight, bone, inverse-bind matrix, clip key and face-pose delta.
// which it equalled
// (tools/md_convert.py + tools/morph_gen_b.py) in every vertex, weight, bone, inverse-bind matrix, clip key and face-pose delta (the old file's 10 own body morphs are intentionally absent). Needs KENSHI_DIR and the repo as the working folder.
#include <gtest/gtest.h>
#include "install_hash.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/render/human_body_build.h>
#include "../../engine/src/render/cgltf.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>

TEST(HumanBodyBuild, OutputMatchesHashReference) {
    ASSERT_FALSE(md::KenshiDir().empty()) << "needs KENSHI_DIR";
    HumanBodyData b; ASSERT_TRUE(BuildHumanBody(md::KenshiDir(), b));
    EXPECT_EQ(b.morph_names.size(), 23u);   // face poses only (the old file's 10 hand-made body morphs are not in the original)
    std::string all;
    auto put = [&](const void* p, size_t n) { all.append(static_cast<const char*>(p), n); };
    auto putv = [&](const auto& v) { put(v.data(), v.size() * sizeof(v[0])); };
    put(&b.nv, sizeof b.nv); putv(b.pos); putv(b.nrm); putv(b.uv); putv(b.weight); putv(b.joint); putv(b.idx);
    for (const std::string& n : b.bone_names) all += n + "|";
    putv(b.inv_bind); putv(b.bind_t); putv(b.bind_q); putv(b.parent);
    for (const auto& c : b.clips) { all += c.name + "|"; for (const auto& tr : c.tracks) { const uint64_t n = tr.size(); put(&n, 8); putv(tr); } }
    for (size_t i = 0; i < b.morph_names.size(); ++i) { all += b.morph_names[i] + "|"; putv(b.morph_delta[i]); }
    MD_EXPECT_INSTALL_HASH("human_body_build", all.data(), all.size());
}

TEST(HumanBodyBuild, FemaleBodyBuildsWithItsOwnSkeleton) {
    ASSERT_FALSE(md::KenshiDir().empty()) << "needs KENSHI_DIR";
    HumanBodyData m, f;
    ASSERT_TRUE(BuildHumanBody(md::KenshiDir(), m));
    ASSERT_TRUE(BuildHumanBody(md::KenshiDir(), "human_female.mesh", f));
    EXPECT_GT(f.nv, 0u); EXPECT_FALSE(f.idx.empty()); EXPECT_FALSE(f.bone_names.empty()); EXPECT_FALSE(f.clips.empty());
    EXPECT_NE(f.nv, m.nv);
    ASSERT_EQ(f.clips.size(), m.clips.size()); for (size_t i = 0; i < f.clips.size(); ++i) EXPECT_EQ(f.clips[i].name, m.clips[i].name) << i;   // the clip indices of the male body address the female clips too
    EXPECT_NE(f.bone_names, m.bone_names);   // the female skeleton has no jaw bones and two breast bones: its own animator is needed
    std::printf("female body: %u vertices, %zu bones, %zu clips (male: %u, %zu, %zu)\n", f.nv, f.bone_names.size(), f.clips.size(), m.nv, m.bone_names.size(), m.clips.size());
    EXPECT_FALSE(BuildHumanBody(md::KenshiDir(), "no_such_body.mesh", f));
}
