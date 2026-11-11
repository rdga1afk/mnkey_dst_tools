// Reads every .mesh / .skeleton under <dir> with md::ogre and prints the same lines as tests/gates/ogre_oracle.py. usage: md_ogre_sweep <KENSHI_DIR>/data
#include <monkey_dust/io/ogre_mesh.h>
#include <monkey_dust/io/ogre_skeleton.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(argv[1])) {
        if (!e.is_regular_file()) continue;
        std::string x = e.path().extension().string();
        for (char& c : x) c = char(tolower((unsigned char)c));
        if (x == ".mesh" || x == ".skeleton") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        std::string rel = fs::relative(p, argv[1]).generic_string(), x = p.extension().string();
        for (char& c : x) c = char(tolower((unsigned char)c));
        md::ogre::Error err;
        if (x == ".mesh") {
            md::ogre::Mesh m;
            if (!md::ogre::ReadMesh(p.string(), m, err)) { std::printf("error|%s|%s\n", rel.c_str(), err.Message().c_str()); continue; }
            size_t verts = m.has_shared ? m.shared.vertex_count : 0, ind = 0;
            for (const auto& s : m.submeshes) { if (s.has_geometry) verts += s.geometry.vertex_count; ind += s.indices.size(); }
            size_t ba = m.bone_assignments.size();
            char bb[256] = "-|-|-|-|-|-";
            if (m.has_bounds) std::snprintf(bb, sizeof bb, "%.9g|%.9g|%.9g|%.9g|%.9g|%.9g", double(m.bounds_min[0]), double(m.bounds_min[1]), double(m.bounds_min[2]), double(m.bounds_max[0]), double(m.bounds_max[1]), double(m.bounds_max[2]));
            std::printf("mesh|%s|%s|submeshes|%zu|vertices|%zu|indices|%zu|poses|%zu|bone_assign|%zu|skeleton|%s|aabb|%s\n", rel.c_str(), m.version.c_str(), m.submeshes.size(), verts, ind, m.poses.size(), ba, m.skeleton.c_str(), bb);
        } else {
            md::ogre::Skeleton s;
            if (!md::ogre::ReadSkeleton(p.string(), s, err)) { std::printf("error|%s|%s\n", rel.c_str(), err.Message().c_str()); continue; }
            size_t tracks = 0, keys = 0;
            for (const auto& a : s.animations) { tracks += a.tracks.size(); for (const auto& t : a.tracks) keys += t.keyframes.size(); }
            std::printf("skeleton|%s|%s|bones|%zu|animations|%zu|tracks|%zu|keys|%zu|links|%zu\n", rel.c_str(), s.version.c_str(), s.bones.size(), s.animations.size(), tracks, keys, s.links.size());
        }
    }
    return 0;
}
