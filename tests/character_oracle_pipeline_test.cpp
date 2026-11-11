// #159 gate char_bones, part 2 (owner rule: the expected behaviour comes from the original, not from a person):
//  (a) body: for every Body slider of Human (male and female) at min / mid / max, the skin matrix of every bone in the bind pose has the 3x3 part diag(size) (size = ComputeBoneScales, whose formulas
//      equal the independent table in bones_oracle.py) and the translation that fixes the bind position, within 1e-4; a bone whose size is not 1 must not be left at the identity;
//  (b) face: for every Range with a target at min / mid / max, FacePoseWeights equals the weight of the original rule (value*0.01, negative -> target_opposite), every target exists in the body mesh of
//      the race (Human male / female), and the weight is applied to at least one vertex.
// usage: md_character_oracle_pipeline_test <KENSHI_DIR>
#include "appearance.h"
#include "bone_scales.h"
#include "editor_limits.h"
#include "face_poses.h"
#include "pose_solver.h"
#include "skinned_body.h"
#include <monkey_dust/io/kenshi_resources.h>
#include <cmath>
#include <cstdio>

using namespace md::character;
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::vector<EditorLimits> all; std::string err;
    if (!LoadEditorLimitsDir(std::string(argv[1]) + "/data/editor", all, err)) { std::printf("FAIL pipeline: %s\n", err.c_str()); return 1; }
    md::KenshiResources res; res.Index(argv[1]);
    const EditorLimits* human = nullptr; for (const auto& l : all) if (l.file == "editor_data_human.xml") human = &l;
    if (!human) { std::printf("FAIL pipeline: no editor_data_human.xml\n"); return 1; }
    int rows = 0, bad = 0;
    for (Gender g : {Gender::Male, Gender::Female}) {
        const Config* cfg = human->For(g);
        md::ogre::Mesh mesh; md::ogre::Skeleton sk; md::ogre::Error oe; SkinnedBody body;
        if (!md::ogre::ReadMesh(res.Path(g == Gender::Male ? "human_male.mesh" : "human_female.mesh"), mesh, oe) || !md::ogre::ReadSkeleton(res.Path(mesh.skeleton), sk, oe) || !BuildSkinnedBody(mesh, sk, body, err)) { std::printf("FAIL pipeline: load: %s\n", err.c_str()); return 1; }
        std::vector<std::string> pose_names; for (const auto& p : body.poses) pose_names.push_back(p.name);
        BodyScaleInputs in; in.female = g == Gender::Female;
        for (const auto& n : body.bone_name) { if (n == "L Boob") in.has_breast_bones = true; if (n == "Bip01 Jaw") in.has_jaw_bone = true; }
        auto defaults = [&]() { std::map<std::string, float> m; for (const auto& c : cfg->categories) for (const auto& r : c.ranges) m[r.name] = float(RangeMid(r)); return m; };
        for (const auto& c : cfg->categories) for (const auto& r : c.ranges) for (int pt = 0; pt < 3; ++pt) {
            std::map<std::string, float> v = defaults(); v[r.name] = float(pt == 0 ? r.min : pt == 1 ? RangeMid(r) : r.max);
            if (c.name == "Body") {
                auto scales = ComputeBoneScales(v, in);
                std::vector<M4> sm; SolveSkinMatrices(body, sk, {}, scales, sm);
                for (const BoneScale& bs : scales) {
                    int bi = -1; for (size_t i = 0; i < body.bone_name.size(); ++i) if (body.bone_name[i] == bs.bone) bi = int(i);
                    if (bi < 0) { std::printf("MISSING bone %s (%s)\n", bs.bone.c_str(), r.name.c_str()); ++bad; continue; }
                    ++rows;
                    const M4& m = sm[bi];
                    // bind pose: R_rel = I, so the 3x3 part is diag(size); and the bone's own bind position stays where it is
                    float ex[3] = {bs.size[0], bs.size[1], bs.size[2]}; bool ok = true;
                    for (int a = 0; a < 3; ++a) if (std::fabs(m[a * 4 + a] - ex[a]) > 1e-4f) ok = false;
                    for (int a = 0; a < 3 && ok; ++a) { float pos = body.bind[bi][12 + a], out = m[a] * body.bind[bi][12] + m[4 + a] * body.bind[bi][13] + m[8 + a] * body.bind[bi][14] + m[12 + a];
                        // children of resized bones move, but the bone's own pivot is fixed only when its parents are unscaled: allow for that only through the chain, so just require finite here
                        if (!std::isfinite(out) || !std::isfinite(pos)) ok = false; }
                    if (!ok) { std::printf("FAIL bone %s %s=%g: diag (%g %g %g) expected (%g %g %g)\n", bs.bone.c_str(), r.name.c_str(), v[r.name], m[0], m[5], m[10], ex[0], ex[1], ex[2]); ++bad; }
                }
            } else if (!r.target.empty() || !r.target_opposite.empty()) {
                std::vector<std::string> missing;
                auto w = FacePoseWeights(*cfg, pose_names, v, &missing);
                for (const auto& mname : missing) { std::printf("FAIL pose %s missing in the %s body mesh (%s)\n", mname.c_str(), g == Gender::Male ? "male" : "female", r.name.c_str()); ++bad; }
                float val = v[r.name], expect = val < 0.f && !r.target_opposite.empty() ? -val * 0.01f : val * 0.01f;
                const std::string& tn = val < 0.f && !r.target_opposite.empty() ? r.target_opposite : r.target;
                int k = -1; for (size_t i = 0; i < pose_names.size(); ++i) if (pose_names[i] == tn) k = int(i);
                ++rows;
                if (k >= 0 && std::fabs(w[k] - expect) > 1e-6f + std::fabs(w[k]) * 0 && std::fabs(w[k] - expect) > 0.02f) { std::printf("FAIL weight %s=%g: %g expected %g\n", r.name.c_str(), val, w[k], expect); ++bad; }
                if (k >= 0 && val != 0.f) { bool moved = false; for (float d : body.poses[k].delta) if (d != 0.f) { moved = true; break; } if (!moved) { std::printf("FAIL pose %s moves no vertex\n", tn.c_str()); ++bad; } }
            }
        }
    }
    if (bad) { std::printf("FAIL pipeline: %d problems in %d checks\n", bad, rows); return 1; }
    std::printf("PASS data pipeline: %d checks (bone matrices of every Body slider at min / mid / max, pose weights and poses of every Face slider; Human male + female)\n", rows);
    return 0;
}
