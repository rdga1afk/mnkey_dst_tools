#include "kenshi_material_cull.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace {
struct Mat { std::string parent; int own = -1; };   // own: -1 no declaration, 0 culled, 1 none
std::unordered_map<std::string, Mat>& Table() {
    static std::unordered_map<std::string, Mat> t;
    static std::once_flag once;
    std::call_once(once, [] {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = fs::path(md::KenshiDir()) / "data";
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".material") continue;
            std::ifstream f(it->path());
            std::string line, cur; int depth = 0;
            while (std::getline(f, line)) {
                const size_t c = line.find("//"); if (c != std::string::npos) line.resize(c);
                std::istringstream ss(line); std::string w0; ss >> w0;
                if (depth == 0 && w0 == "material") {
                    std::string name, w; ss >> name >> w;
                    cur = name; Mat& m = t[cur]; m = Mat{};
                    if (w == ":") { std::string par; ss >> par; if (!par.empty() && par.back() == '{') par.pop_back(); m.parent = par; }
                } else if (depth > 0 && !cur.empty() && (w0 == "cull_hardware")) {
                    std::string v; ss >> v; Mat& m = t[cur]; m.own = (v == "none") ? 1 : 0;
                }
                for (char ch : line) { if (ch == '{') ++depth; else if (ch == '}') { if (--depth <= 0) { depth = 0; cur.clear(); } } }
            }
        }
        fprintf(stdout, "[KenshiMaterial] %zu materials\n", t.size());
    });
    return t;
}
}

static bool Resolve(const std::string& name);
bool KenshiMaterialTwoSided(const std::string& name) {
    const bool r = Resolve(name);
    static std::mutex mu; static std::unordered_map<std::string, bool> logged;
    std::lock_guard<std::mutex> lk(mu);
    if (logged.emplace(name, r).second) fprintf(stdout, "[KenshiMaterial] %s -> %s\n", name.c_str(), r ? "two-sided" : "culled");
    return r;
}

static bool Resolve(const std::string& name) {
    auto& t = Table();
    std::string n = name;
    for (int hop = 0; hop < 16; ++hop) {
        auto it = t.find(n);
        if (it == t.end()) return true;   // unknown: keep both sides
        if (it->second.own >= 0) return it->second.own == 1;
        if (it->second.parent.empty()) return false;   // no declaration in the chain: Ogre default (culled)
        n = it->second.parent;
    }
    return true;
}
