// BuildHumanBody (the NPC body from the install: human_male.mesh + male_skeleton.skeleton) equals the committed game/data/props/md_human.glb
// (tools/md_convert.py + tools/morph_gen_b.py) in every vertex, weight, bone, inverse-bind matrix, clip key and face-pose delta (the old file's 10 own body morphs are intentionally absent). Needs KENSHI_DIR and the repo as the working folder.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/render/human_body_build.h>
#include "../../engine/src/render/cgltf.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>

namespace {
double Worst(const float* a, const std::vector<float>& b, size_t n) { double w = 0; for (size_t i = 0; i < n; ++i) w = std::fmax(w, std::fabs((double)a[i] - b[i]) / std::fmax(1.0, std::fabs((double)a[i]))); return w; }
}

TEST(HumanBodyBuild, EqualsCommittedGlb) {
    const char* glb = "game/data/props/md_human.glb";
    if (md::KenshiDir().empty() || !std::filesystem::exists(glb)) GTEST_SKIP() << "needs KENSHI_DIR and the repo as working folder";
    HumanBodyData b; ASSERT_TRUE(BuildHumanBody(md::KenshiDir(), b));
    cgltf_options o = {}; cgltf_data* d = nullptr;
    ASSERT_EQ(cgltf_parse_file(&o, glb, &d), cgltf_result_success); ASSERT_EQ(cgltf_load_buffers(&o, d, glb), cgltf_result_success);
    const cgltf_primitive& pr = d->meshes[0].primitives[0];
    cgltf_accessor *P = nullptr, *N = nullptr, *U = nullptr, *J = nullptr, *W = nullptr;
    for (cgltf_size a = 0; a < pr.attributes_count; ++a) { auto t = pr.attributes[a].type; auto* acc = pr.attributes[a].data; if (t == cgltf_attribute_type_position) P = acc; if (t == cgltf_attribute_type_normal) N = acc; if (t == cgltf_attribute_type_texcoord) U = acc; if (t == cgltf_attribute_type_joints) J = acc; if (t == cgltf_attribute_type_weights) W = acc; }
    ASSERT_EQ(P->count, (size_t)b.nv);
    double wp = 0, wn = 0, wu = 0, ww = 0; size_t jbad = 0;
    for (uint32_t i = 0; i < b.nv; ++i) {
        float p[3], n[3], u[2], w[4]; unsigned j[4];
        cgltf_accessor_read_float(P, i, p, 3); cgltf_accessor_read_float(N, i, n, 3); cgltf_accessor_read_float(U, i, u, 2); cgltf_accessor_read_float(W, i, w, 4); cgltf_accessor_read_uint(J, i, j, 4);
        for (int k = 0; k < 3; ++k) { wp = std::fmax(wp, std::fabs(p[k] - b.pos[i * 3 + k])); wn = std::fmax(wn, std::fabs(n[k] - b.nrm[i * 3 + k])); }
        for (int k = 0; k < 2; ++k) wu = std::fmax(wu, std::fabs(u[k] - b.uv[i * 2 + k]));
        for (int k = 0; k < 4; ++k) { ww = std::fmax(ww, std::fabs(w[k] - b.weight[i * 4 + k])); if (j[k] != b.joint[i * 4 + k] && w[k] > 0) ++jbad; }
    }
    std::printf("[HumanBody] max diff pos %.2e nrm %.2e uv %.2e weight %.2e, joint mismatches %zu\n", wp, wn, wu, ww, jbad);
    EXPECT_LT(wp, 1e-5); EXPECT_LT(wn, 2e-6); EXPECT_LT(wu, 2e-6); EXPECT_LT(ww, 2e-6); EXPECT_EQ(jbad, 0u);
    ASSERT_EQ(pr.indices->count, b.idx.size());
    for (cgltf_size i = 0; i < pr.indices->count; ++i) ASSERT_EQ(cgltf_accessor_read_index(pr.indices, i), b.idx[i]);
    // skeleton
    const cgltf_skin& sk = d->skins[0];
    ASSERT_EQ(sk.joints_count, b.bone_names.size());
    double wi = 0, wb = 0;
    for (cgltf_size i = 0; i < sk.joints_count; ++i) {
        EXPECT_STREQ(sk.joints[i]->name, b.bone_names[i].c_str());
        float m[16]; cgltf_accessor_read_float(sk.inverse_bind_matrices, i, m, 16);
        for (int k = 0; k < 16; ++k) wi = std::fmax(wi, std::fabs(m[k] - b.inv_bind[i * 16 + k]));
        for (int k = 0; k < 3; ++k) wb = std::fmax(wb, std::fabs(sk.joints[i]->translation[k] - b.bind_t[i * 3 + k]));
        for (int k = 0; k < 4; ++k) wb = std::fmax(wb, std::fabs(sk.joints[i]->rotation[k] - b.bind_q[i * 4 + k]));
        int par = -1; for (cgltf_size q = 0; q < sk.joints_count; ++q) if (sk.joints[q] == sk.joints[i]->parent) par = (int)q;
        EXPECT_EQ(par, b.parent[i]) << i;
    }
    std::printf("[HumanBody] max diff inverse bind %.2e, bind pose %.2e\n", wi, wb);
    EXPECT_LT(wi, 1e-4); EXPECT_LT(wb, 5e-5);
    // clips
    ASSERT_EQ(d->animations_count, b.clips.size());
    double wc = 0, wct = 0, wcq = 0;
    for (cgltf_size a = 0; a < d->animations_count; ++a) {
        const cgltf_animation& an = d->animations[a];
        const HumanBodyData::Clip* c = nullptr; for (const auto& cc : b.clips) if (cc.name == an.name) c = &cc;
        ASSERT_NE(c, nullptr) << an.name;
        for (cgltf_size ch = 0; ch < an.channels_count; ++ch) {
            int bi = -1; for (cgltf_size q = 0; q < sk.joints_count; ++q) if (sk.joints[q] == an.channels[ch].target_node) bi = (int)q;
            ASSERT_GE(bi, 0);
            const auto& keys = c->tracks[bi];
            ASSERT_EQ(an.channels[ch].sampler->input->count, keys.size()) << an.name << " bone " << bi;
            for (cgltf_size k = 0; k < keys.size(); ++k) {
                float t; cgltf_accessor_read_float(an.channels[ch].sampler->input, k, &t, 1); wc = std::fmax(wc, std::fabs(t - keys[k].t));
                if (an.channels[ch].target_path == cgltf_animation_path_type_translation) { float v[3]; cgltf_accessor_read_float(an.channels[ch].sampler->output, k, v, 3); wct = std::fmax(wct, std::fmax(std::fabs(v[0] - keys[k].tx), std::fmax(std::fabs(v[1] - keys[k].ty), std::fabs(v[2] - keys[k].tz)))); }
                else { float v[4]; cgltf_accessor_read_float(an.channels[ch].sampler->output, k, v, 4); { const double e = std::fmax(std::fmax(std::fabs(v[0] - keys[k].qx), std::fabs(v[1] - keys[k].qy)), std::fmax(std::fabs(v[2] - keys[k].qz), std::fabs(v[3] - keys[k].qw))); if (e > wcq) { wcq = e; if (getenv("MD_HB_DUMP")) std::printf("worst q: %s bone %d key %zu glb %.6f %.6f %.6f %.6f mine %.6f %.6f %.6f %.6f\n", an.name, bi, (size_t)k, v[0], v[1], v[2], v[3], keys[k].qx, keys[k].qy, keys[k].qz, keys[k].qw); } } }
            }
        }
    }
    std::printf("[HumanBody] %zu clips, max diff of key times %.2e, translations %.2e, rotations %.2e\n", b.clips.size(), wc, wct, wcq);
    // rotations: the .glb went through OgreXMLConverter's axis-angle (acos of w), which quantises angles below ~1e-3 rad (e.g. 0.000976562); the binary quaternion is exact, so a 5e-4 difference in a tiny rotation is the old file's error
    EXPECT_LT(wc, 1e-5); EXPECT_LT(wct, 1e-4); EXPECT_LT(wcq, 6e-4);
    // morphs by name
    cgltf_size ex_n = 0; cgltf_copy_extras_json(d, &d->meshes[0].extras, nullptr, &ex_n);
    std::string ex_s(ex_n + 1, 0); cgltf_copy_extras_json(d, &d->meshes[0].extras, ex_s.data(), &ex_n);
    const char* extras = ex_s.c_str();
    double wm = 0; size_t matched = 0;
    for (size_t t = 0; t < pr.targets_count; ++t) {
        // names: the committed file lists them in extras.targetNames in target order
        const char* key = std::strstr(extras, "targetNames"); ASSERT_NE(key, nullptr);
        size_t idx = 0; const char* s = std::strchr(key, '[') + 1; std::string name;
        for (; idx <= t; ++idx) { s = std::strchr(s, '"') + 1; const char* e = std::strchr(s, '"'); name.assign(s, e - s); s = e + 1; }
        size_t bm = 0; for (; bm < b.morph_names.size(); ++bm) if (b.morph_names[bm] == name) break;
        if (bm == b.morph_names.size()) continue;   // the old file's 10 hand-made body morphs: not in the original, not built
        cgltf_accessor* acc = nullptr; for (cgltf_size a = 0; a < pr.targets[t].attributes_count; ++a) if (pr.targets[t].attributes[a].type == cgltf_attribute_type_position) acc = pr.targets[t].attributes[a].data;
        double w = 0; for (uint32_t v = 0; v < b.nv; ++v) { float dv[3]; cgltf_accessor_read_float(acc, v, dv, 3); for (int k = 0; k < 3; ++k) w = std::fmax(w, std::fabs(dv[k] - b.morph_delta[bm][v * 3 + k])); }
        if (w > 1e-5) ADD_FAILURE() << "morph " << name << " differs by " << w;
        wm = std::fmax(wm, w); ++matched;
    }
    std::printf("[HumanBody] %zu morphs compared, max diff %.2e (built: %zu)\n", matched, wm, b.morph_names.size());
    EXPECT_EQ(matched, 23u); EXPECT_EQ(b.morph_names.size(), 23u);   // face poses only
    cgltf_free(d);
}
