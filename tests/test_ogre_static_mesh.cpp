// LoadOgreStaticMesh reads a Kenshi .mesh straight from the install and gives the geometry of the converted .glb (tools/md_convert_static.py --scale 0.1): every glb in game/data/props/features
// (named by tools/md_convert_features.py sanitize()) against its .mesh named in game/data/features_scatter.txt. Needs KENSHI_DIR and the repo as the working folder.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/render/prop_mesh.h>
#include "../../engine/src/render/cgltf.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>

TEST(OgreStaticMesh, FeatureGlbsEqualDirectMesh) {
    if (md::KenshiDir().empty() || !std::filesystem::exists("game/data/features_scatter.txt")) GTEST_SKIP() << "needs KENSHI_DIR and the repo as working folder";
    std::map<std::string, std::string> by_glb;   // sanitized name -> game-data path
    std::ifstream f("game/data/features_scatter.txt"); std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::string mp = line.substr(0, line.find('|'));
        std::string n = mp.substr(mp.find_last_of("\\/") + 1);
        n = std::regex_replace(n, std::regex("\\.mesh$", std::regex::icase), "");
        n = std::regex_replace(n, std::regex("[^A-Za-z0-9_.-]"), "_");
        by_glb[n] = mp;
    }
    int checked = 0, bad = 0;
    for (const auto& kv : by_glb) {
        const std::string glb = "game/data/props/features/" + kv.first + ".glb";
        if (!std::filesystem::exists(glb)) continue;
        cgltf_options o = {}; cgltf_data* d = nullptr;
        ASSERT_EQ(cgltf_parse_file(&o, glb.c_str(), &d), cgltf_result_success) << glb;
        ASSERT_EQ(cgltf_load_buffers(&o, d, glb.c_str()), cgltf_result_success);
        const cgltf_primitive& pr = d->meshes[0].primitives[0];
        cgltf_accessor *pa = nullptr, *na = nullptr, *ua = nullptr;
        for (cgltf_size a = 0; a < pr.attributes_count; ++a) { if (pr.attributes[a].type == cgltf_attribute_type_position) pa = pr.attributes[a].data; if (pr.attributes[a].type == cgltf_attribute_type_normal) na = pr.attributes[a].data; if (pr.attributes[a].type == cgltf_attribute_type_texcoord) ua = pr.attributes[a].data; }
        std::vector<PropVertex> v; std::vector<uint32_t> idx;
        const bool ok = LoadOgreStaticMesh(kv.second, 0.1f, 0.f, v, idx);
        bool same = ok && v.size() == pa->count && idx.size() == pr.indices->count;
        double worst = 0;
        for (cgltf_size i = 0; same && i < pa->count; ++i) {
            float p[3], n[3], uv[2] = {0, 0}; cgltf_accessor_read_float(pa, i, p, 3); cgltf_accessor_read_float(na, i, n, 3); if (ua) cgltf_accessor_read_float(ua, i, uv, 2);
            const float mine[8] = { v[i].x, v[i].y, v[i].z, v[i].nx, v[i].ny, v[i].nz, v[i].u, v[i].v }, ref[8] = { p[0], p[1], p[2], n[0], n[1], n[2], uv[0], uv[1] };
            for (int k = 0; k < 8; ++k) worst = std::fmax(worst, std::fabs(mine[k] - ref[k]) / std::fmax(1.0f, std::fabs(ref[k])));   // the glb holds the 6-digit text of OgreXMLConverter
        }
        for (cgltf_size i = 0; same && i < pr.indices->count; ++i) if (idx[i] != cgltf_accessor_read_index(pr.indices, i)) same = false;
        cgltf_free(d);
        ++checked;
        if (!same || worst > 1e-5) { ++bad; if (bad <= 5) ADD_FAILURE() << kv.first << " ok=" << ok << " worst=" << worst; }
    }
    EXPECT_GT(checked, 50);
    EXPECT_EQ(bad, 0);
}

// The hair glbs (game/data/hair, converted by tools/md_convert.py) hold the same geometry as the install's character/hair/meshes/<name>.mesh.
TEST(OgreStaticMesh, HairGlbsEqualDirectMesh) {
    ASSERT_FALSE(md::KenshiDir().empty() || !std::filesystem::exists("game/data/hair")) << "needs KENSHI_DIR and the repo as working folder";
    int checked = 0, bad = 0;
    for (const auto& e : std::filesystem::directory_iterator("game/data/hair")) {
        if (e.path().extension() != ".glb") continue;
        const std::string glb = e.path().string();
        cgltf_options o = {}; cgltf_data* d = nullptr;
        ASSERT_EQ(cgltf_parse_file(&o, glb.c_str(), &d), cgltf_result_success) << glb;
        ASSERT_EQ(cgltf_load_buffers(&o, d, glb.c_str()), cgltf_result_success);
        const cgltf_primitive& pr = d->meshes[0].primitives[0];
        cgltf_accessor *pa = nullptr, *na = nullptr, *ua = nullptr;
        for (cgltf_size a = 0; a < pr.attributes_count; ++a) { if (pr.attributes[a].type == cgltf_attribute_type_position) pa = pr.attributes[a].data; if (pr.attributes[a].type == cgltf_attribute_type_normal) na = pr.attributes[a].data; if (pr.attributes[a].type == cgltf_attribute_type_texcoord) ua = pr.attributes[a].data; }
        std::vector<PropVertex> v; std::vector<uint32_t> idx;
        const bool ok = LoadOgreStaticMesh(e.path().stem().string() + ".mesh", 0.1f, 0.f, v, idx, false);
        double worst = 1e9; bool same = ok && v.size() == pa->count;
        if (same) { worst = 0; for (cgltf_size i = 0; i < pa->count; ++i) { float p[3], n[3], uv[2] = {0, 0}; cgltf_accessor_read_float(pa, i, p, 3); cgltf_accessor_read_float(na, i, n, 3); if (ua) cgltf_accessor_read_float(ua, i, uv, 2);
            const float mine[8] = { v[i].x, v[i].y, v[i].z, v[i].nx, v[i].ny, v[i].nz, v[i].u, v[i].v }, ref[8] = { p[0], p[1], p[2], n[0], n[1], n[2], uv[0], uv[1] };
            for (int k = 0; k < 8; ++k) worst = std::fmax(worst, std::fabs(mine[k] - ref[k]) / std::fmax(1.0f, std::fabs(ref[k]))); } }
        if (getenv("MD_HAIR_DUMP") && glb.find(getenv("MD_HAIR_DUMP")) != std::string::npos) for (int i : {0, 1, 2, 100, 2000}) { float p[3]; cgltf_accessor_read_float(pa, i, p, 3); float n[3], uv[2] = {0,0}; cgltf_accessor_read_float(na, i, n, 3); if (ua) cgltf_accessor_read_float(ua, i, uv, 2); { float pp[3]; cgltf_accessor_read_float(pa, i, pp, 3); fprintf(stderr, "p glb %.3f %.3f %.3f mesh %.3f %.3f %.3f\n", pp[0], pp[1], pp[2], v[i].x, v[i].y, v[i].z); } fprintf(stderr, "v%d n glb %.3f %.3f %.3f mesh %.3f %.3f %.3f  uv glb %.3f %.3f mesh %.3f %.3f\n", i, n[0], n[1], n[2], v[i].nx, v[i].ny, v[i].nz, uv[0], uv[1], v[i].u, v[i].v); }
        cgltf_free(d); ++checked;
        if (!same || worst > 1e-5) { ++bad; if (bad <= 8) ADD_FAILURE() << glb << " mesh ok=" << ok << " verts " << v.size() << " vs " << pa->count << " worst=" << worst; }
    }
    EXPECT_GT(checked, 15);   // the 18 haircut_* glbs; the 11 legacy styles were removed (#201)
    EXPECT_EQ(bad, 0);
}

// Fact for #201 (afrohair): the raw bounding box of every hair mesh in the install (units as stored, no scale).
TEST(OgreStaticMesh, HairRawExtents) {
    ASSERT_FALSE(md::KenshiDir().empty()) << "needs KENSHI_DIR";
    for (const auto& e : std::filesystem::directory_iterator(md::KenshiDir() + "/data/character/hair/meshes")) {
        if (e.path().extension() != ".mesh") continue;
        std::vector<PropVertex> v; std::vector<uint32_t> idx;
        if (!LoadOgreStaticMesh(e.path().filename().string(), 1.0f, 0.f, v, idx, false)) continue;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (const PropVertex& p : v) { const float c[3] = {p.x, p.y, p.z}; for (int k = 0; k < 3; ++k) { lo[k] = std::fmin(lo[k], c[k]); hi[k] = std::fmax(hi[k], c[k]); } }
        std::printf("[HairExtent] %-28s %7.2f %7.2f %7.2f\n", e.path().filename().string().c_str(), hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    }
}
