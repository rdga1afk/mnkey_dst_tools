// Ogre .mesh / .skeleton readers and the Kenshi resource lookup (engine/src/io/ogre_*.cpp, kenshi_resources.cpp): synthetic files built here.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_resources.h>
#include <monkey_dust/io/ogre_mesh.h>
#include <monkey_dust/io/ogre_skeleton.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
void P8(Bytes& b, uint8_t v) { b.push_back(v); }
void P16(Bytes& b, uint16_t v) { P8(b, v & 0xff); P8(b, v >> 8); }
void P32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) P8(b, (v >> (8 * i)) & 0xff); }
void PF(Bytes& b, float f) { uint32_t u; std::memcpy(&u, &f, 4); P32(b, u); }
void PS(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); P8(b, '\n'); }
void Append(Bytes& b, const Bytes& o) { b.insert(b.end(), o.begin(), o.end()); }
// chunk with the right length (header 6 bytes + body); `wrong_len` != 0 writes that length instead (what v1.8 / v1.41 do for parents)
Bytes Chunk(uint16_t id, const Bytes& body, uint32_t wrong_len = 0) { Bytes b; P16(b, id); P32(b, wrong_len ? wrong_len : uint32_t(6 + body.size())); Append(b, body); return b; }

Bytes Geometry(uint32_t nverts, uint16_t colour_type, const std::vector<uint8_t>& colours /* 4 bytes per vertex */, bool wrong) {
    Bytes decl;   // position FLOAT3 at 0, diffuse at 12, buffer 0, stride 16
    Bytes e1; P16(e1, 0); P16(e1, 2); P16(e1, 1); P16(e1, 0); P16(e1, 0); Append(decl, Chunk(0x5110, e1));
    Bytes e2; P16(e2, 0); P16(e2, colour_type); P16(e2, 5); P16(e2, 12); P16(e2, 0); Append(decl, Chunk(0x5110, e2));
    Bytes data;
    for (uint32_t v = 0; v < nverts; ++v) { PF(data, float(v)); PF(data, 2.f * v); PF(data, 3.f * v); for (int k = 0; k < 4; ++k) P8(data, colours[v * 4 + k]); }
    Bytes buf; P16(buf, 0); P16(buf, 16); Append(buf, Chunk(0x5210, data));
    Bytes g; P32(g, nverts); Append(g, Chunk(0x5100, decl)); Append(g, Chunk(0x5200, buf));
    return Chunk(0x5000, g, wrong ? 7 : 0);
}

Bytes Submesh(bool idx32, const std::vector<uint32_t>& idx, bool wrong) {
    Bytes s; PS(s, "mat"); P8(s, 1); P32(s, uint32_t(idx.size())); P8(s, idx32);
    for (uint32_t i : idx) { if (idx32) P32(s, i); else P16(s, uint16_t(i)); }
    return Chunk(0x4000, s, wrong ? 9 : 0);
}

Bytes MeshFile(const std::string& version, bool wrong_parents, uint16_t colour_type, bool idx32, bool with_unknown, bool with_pose) {
    Bytes f; P16(f, 0x1000); PS(f, version);
    Bytes m; P8(m, 0);
    Append(m, Geometry(3, colour_type, {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 1, 2, 3, 4}, wrong_parents));
    Append(m, Submesh(idx32, {0, 1, 2}, wrong_parents));
    if (with_unknown) Append(m, Chunk(0xB000, Bytes{1, 2, 3, 4, 5}));        // edge lists: skipped whole
    if (with_unknown) {                                                       // LOD with a wrong length and one usage child
        Bytes lod; PS(lod, "distance"); P16(lod, 2); P8(lod, 0); Append(lod, Chunk(0x8100, Bytes{9, 9, 9, 9}));
        Append(m, Chunk(0x8000, lod, 3));
    }
    Bytes bounds; for (float x : {0.f, 0.f, 0.f, 2.f, 4.f, 6.f, 7.f}) PF(bounds, x);
    Append(m, Chunk(0x9000, bounds));
    if (with_pose) {
        Bytes poses, pose, v1; PS(pose, "wide_jaw"); P16(pose, 0);
        if (version.find("v1.41") == std::string::npos) P8(pose, 0);          // v1.8+: "has normals" flag
        P32(v1, 1); PF(v1, 0.5f); PF(v1, 0.f); PF(v1, -1.f);
        Append(pose, Chunk(0xC111, v1)); Append(poses, Chunk(0xC100, pose));
        Append(m, Chunk(0xC000, poses));
    }
    Append(f, Chunk(0x3000, m, wrong_parents ? 5 : 0));
    return f;
}

md::ogre::Mesh Parse(const Bytes& b, md::ogre::Error* e = nullptr) {
    md::ogre::Mesh m; md::ogre::Error err;
    bool ok = md::ogre::ParseMesh(b.data(), b.size(), m, err);
    if (e) *e = err;
    EXPECT_EQ(ok, err.kind == md::ogre::ErrorKind::None);
    return m;
}
} // namespace

TEST(OgreMesh, ReadsGeometryIndicesAndBounds) {
    md::ogre::Error e;
    auto m = Parse(MeshFile("[MeshSerializer_v1.100]", false, md::ogre::vtype::COLOUR_ARGB, false, false, false), &e);
    ASSERT_EQ(e.kind, md::ogre::ErrorKind::None) << e.Message();
    ASSERT_EQ(m.submeshes.size(), 1u);
    EXPECT_EQ(m.submeshes[0].indices, (std::vector<uint32_t>{0, 1, 2}));
    ASSERT_TRUE(m.has_bounds);
    EXPECT_FLOAT_EQ(m.bounds_max[2], 6.f); EXPECT_FLOAT_EQ(m.bounds_radius, 7.f);
    EXPECT_EQ(m.submeshes[0].operation, md::ogre::OP_TRIANGLE_LIST);
}

TEST(OgreMesh, OldVersionsIgnoreParentLengths) {   // v1.8 / v1.41 write wrong lengths for MESH, SUBMESH, GEOMETRY
    for (const char* v : {"[MeshSerializer_v1.8]", "[MeshSerializer_v1.41]"}) {
        md::ogre::Error e;
        auto m = Parse(MeshFile(v, true, md::ogre::vtype::COLOUR_ARGB, false, false, false), &e);
        ASSERT_EQ(e.kind, md::ogre::ErrorKind::None) << v << ": " << e.Message();
        ASSERT_EQ(m.submeshes.size(), 1u);
        ASSERT_TRUE(m.has_shared);
        EXPECT_EQ(m.shared.vertex_count, 3u);
        EXPECT_EQ(m.submeshes[0].indices.size(), 3u);
    }
}

TEST(OgreMesh, SkipsUnknownChunksAndWrongLod) {
    md::ogre::Error e;
    auto m = Parse(MeshFile("[MeshSerializer_v1.8]", true, md::ogre::vtype::COLOUR_ARGB, false, true, false), &e);
    ASSERT_EQ(e.kind, md::ogre::ErrorKind::None) << e.Message();
    EXPECT_TRUE(m.has_bounds);
}

TEST(OgreMesh, ColourOrderArgbVersusAbgr) {
    std::vector<float> c;
    auto a = Parse(MeshFile("[MeshSerializer_v1.100]", false, md::ogre::vtype::COLOUR_ARGB, false, false, false));
    ASSERT_TRUE(a.shared.Colours(c));   // bytes 10 20 30 40 are B G R A
    EXPECT_NEAR(c[0], 0x30 / 255.f, 1e-6); EXPECT_NEAR(c[1], 0x20 / 255.f, 1e-6); EXPECT_NEAR(c[2], 0x10 / 255.f, 1e-6); EXPECT_NEAR(c[3], 0x40 / 255.f, 1e-6);
    auto b = Parse(MeshFile("[MeshSerializer_v1.100]", false, md::ogre::vtype::COLOUR_ABGR, false, false, false));
    ASSERT_TRUE(b.shared.Colours(c));   // bytes 10 20 30 40 are R G B A
    EXPECT_NEAR(c[0], 0x10 / 255.f, 1e-6); EXPECT_NEAR(c[2], 0x30 / 255.f, 1e-6);
}

TEST(OgreMesh, SixteenAndThirtyTwoBitIndices) {
    for (bool idx32 : {false, true}) {
        auto m = Parse(MeshFile("[MeshSerializer_v1.100]", false, md::ogre::vtype::COLOUR_ARGB, idx32, false, false));
        ASSERT_EQ(m.submeshes.size(), 1u);
        EXPECT_EQ(m.submeshes[0].indices, (std::vector<uint32_t>{0, 1, 2})) << idx32;
    }
}

TEST(OgreMesh, PosesMovePositionsByWeight) {
    for (const char* v : {"[MeshSerializer_v1.41]", "[MeshSerializer_v1.8]"}) {
        auto m = Parse(MeshFile(v, true, md::ogre::vtype::COLOUR_ARGB, false, false, true));
        ASSERT_EQ(m.poses.size(), 1u) << v;
        EXPECT_EQ(m.poses[0].name, "wide_jaw");
        std::vector<float> p;
        ASSERT_TRUE(m.shared.Positions(p));
        EXPECT_FLOAT_EQ(p[3], 1.f);                         // vertex 1 x
        EXPECT_TRUE(m.ApplyPose("wide_jaw", 2.f));
        ASSERT_TRUE(m.shared.Positions(p));
        EXPECT_FLOAT_EQ(p[3], 2.f);                         // 1 + 2 * 0.5
        EXPECT_FLOAT_EQ(p[5], 3.f * 1 - 2.f);               // z 3 + 2 * -1
        EXPECT_FALSE(m.ApplyPose("missing", 1.f));
    }
}

TEST(OgreMesh, RefusesBadFiles) {
    md::ogre::Error e;
    Bytes bad = {0x34, 0x12, 'x', '\n'};
    Parse(bad, &e);
    EXPECT_EQ(e.kind, md::ogre::ErrorKind::BadHeader);
    Bytes trunc = MeshFile("[MeshSerializer_v1.100]", false, md::ogre::vtype::COLOUR_ARGB, false, false, false);
    trunc.resize(trunc.size() - 20);
    Parse(trunc, &e);
    EXPECT_NE(e.kind, md::ogre::ErrorKind::None);
    Bytes idx = MeshFile("[MeshSerializer_v1.100]", false, md::ogre::vtype::COLOUR_ARGB, false, false, false);
    // index 2 -> 9 (the indices 0, 1, 2 as u16): addresses a vertex that is not there
    for (size_t i = 0; i + 6 <= idx.size(); ++i) if (idx[i] == 0 && idx[i + 1] == 0 && idx[i + 2] == 1 && idx[i + 3] == 0 && idx[i + 4] == 2 && idx[i + 5] == 0) { idx[i + 4] = 9; break; }
    Parse(idx, &e);
    EXPECT_EQ(e.kind, md::ogre::ErrorKind::Invalid);
}

namespace {
Bytes BoneChunk(const std::string& name, uint16_t handle, bool scale, bool include_name_in_len) {
    Bytes body; PS(body, name); P16(body, handle); for (float x : {1.f, 2.f, 3.f}) PF(body, x); for (float x : {0.f, 0.f, 0.f, 1.f}) PF(body, x);
    if (scale) for (float x : {2.f, 2.f, 2.f}) PF(body, x);
    // the writer of the shipped files leaves the name out of the stored length: 6 + 2 + 12 + 16 = 36 (no scale), 48 (scale)
    uint32_t len = 6 + 2 + 12 + 16 + (scale ? 12 : 0) + (include_name_in_len ? uint32_t(name.size() + 1) : 0);
    Bytes c; P16(c, 0x2000); P32(c, len); Append(c, body);
    return c;
}
Bytes SkeletonFile(bool bone_scale, bool key_scale, bool unknown_track_bone) {
    Bytes f; P16(f, 0x1000); PS(f, "[Serializer_v1.10]");
    Append(f, BoneChunk("Root", 0, bone_scale, false)); Append(f, BoneChunk("Child", 1, false, false));
    Bytes par; P16(par, 1); P16(par, 0); Append(f, Chunk(0x3000, par));
    Bytes anim; PS(anim, "walk"); PF(anim, 1.5f);
    Bytes track; P16(track, unknown_track_bone ? 7 : 1);
    Bytes key; PF(key, 0.25f); for (float x : {0.f, 0.f, 0.f, 1.f}) PF(key, x); for (float x : {1.f, 0.f, 0.f}) PF(key, x);
    if (key_scale) for (float x : {3.f, 3.f, 3.f}) PF(key, x);
    Append(track, Chunk(0x4110, key));
    Append(anim, Chunk(0x4100, track, 3));          // parent length is not trusted
    Append(f, Chunk(0x4000, anim, 4));
    Bytes link; PS(link, "other.skeleton"); PF(link, 0.5f); Append(f, Chunk(0x5000, link));
    return f;
}
} // namespace

TEST(OgreSkeleton, BonesParentsScaleAndAnimations) {
    for (bool bs : {false, true}) for (bool ks : {false, true}) {
        Bytes b = SkeletonFile(bs, ks, false);
        md::ogre::Skeleton s; md::ogre::Error e;
        ASSERT_TRUE(md::ogre::ParseSkeleton(b.data(), b.size(), s, e)) << e.Message();
        ASSERT_EQ(s.bones.size(), 2u);
        EXPECT_EQ(s.bones[0].has_scale, bs); EXPECT_EQ(s.bones[1].has_scale, false);
        EXPECT_TRUE(s.bones[1].has_parent); EXPECT_EQ(s.bones[1].parent, 0);
        EXPECT_FLOAT_EQ(s.bones[0].orientation[3], 1.f);   // x y z w as stored
        ASSERT_EQ(s.animations.size(), 1u);
        ASSERT_EQ(s.animations[0].tracks.size(), 1u);
        ASSERT_EQ(s.animations[0].tracks[0].keyframes.size(), 1u);
        EXPECT_EQ(s.animations[0].tracks[0].keyframes[0].has_scale, ks);
        ASSERT_EQ(s.links.size(), 1u);
        EXPECT_EQ(s.links[0].first, "other.skeleton");
    }
}

TEST(OgreSkeleton, TrackOfUnknownBoneIsAnError) {
    Bytes b = SkeletonFile(false, false, true);
    md::ogre::Skeleton s; md::ogre::Error e;
    EXPECT_FALSE(md::ogre::ParseSkeleton(b.data(), b.size(), s, e));
    EXPECT_EQ(e.kind, md::ogre::ErrorKind::Invalid);
}

TEST(KenshiResources, StalePathsFallBackToFileName) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("md-res-" + std::to_string(::getpid()));
    fs::path real = dir / "data/character/skins/human_male";
    fs::create_directories(real);
    { std::ofstream(real / "Head_HI.dds") << "x"; std::ofstream(real / "Body.dds") << "x"; }
    md::KenshiResources r; r.Index(dir.string());
    const std::string want = "data/character/skins/human_male/Head_HI.dds";
    EXPECT_EQ(r.Find(".\\data\\character\\skins\\hu\\heads\\head_hi.dds"), want);
    EXPECT_EQ(r.Find("data/character/skins/human_male/Head_HI.dds"), want);
    EXPECT_EQ(r.Find("Body_HI.dds"), "");
    EXPECT_EQ(r.FindTexture("Body_HI.dds"), "data/character/skins/human_male/Body.dds");   // a _HI texture the install lacks is its plain one
    EXPECT_EQ(r.Find("missing.dds"), "");
    fs::remove_all(dir);
}
