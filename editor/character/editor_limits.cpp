#include "editor_limits.h"
#include "xml_mini.h"
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <sstream>

namespace md::character {
namespace {

bool ToInt(const std::string& s, int& out) {
    if (s.empty()) return false;
    size_t i = 0; bool neg = false;
    if (s[0] == '-') { neg = true; i = 1; } else if (s[0] == '+') i = 1;
    if (i >= s.size()) return false;
    long v = 0;
    for (; i < s.size(); ++i) { if (s[i] < '0' || s[i] > '9') return false; v = v * 10 + (s[i] - '0'); if (v > 1000000) return false; }
    out = (int)(neg ? -v : v);
    return true;
}

bool ReadFileText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

bool IntAttr(const XmlNode& n, const char* key, bool required, bool& has, int& v, std::string& err) {
    const std::string* a = n.Attr(key);
    has = a != nullptr;
    if (!a) { if (required) { err = std::string("<") + n.name + "> without " + key; return false; } return true; }
    if (!ToInt(*a, v)) { err = std::string("<") + n.name + "> attribute " + key + "=\"" + *a + "\" is not an integer"; return false; }
    return true;
}

} // namespace

const Category* Config::Find(const std::string& category) const {
    for (const auto& c : categories) if (c.name == category) return &c;
    return nullptr;
}

const Config* EditorLimits::For(Gender g) const {
    for (const auto& c : configs) if (c.gender == g) return &c;
    for (const auto& c : configs) if (c.gender == Gender::Single) return &c;
    return nullptr;
}

bool LoadEditorLimits(const std::string& path, EditorLimits& out, std::string& error) {
    std::string text, err;
    if (!ReadFileText(path, text)) { error = "cannot read " + path; return false; }
    XmlNode root;
    if (!ParseXml(text, root, err)) { error = path + ": " + err; return false; }
    if (root.name != "Editor") { error = path + ": root element is <" + root.name + ">, expected <Editor>"; return false; }
    out = EditorLimits{};
    size_t slash = path.find_last_of('/');
    out.file = slash == std::string::npos ? path : path.substr(slash + 1);
    for (const XmlNode& cn : root.children) {
        if (cn.name != "Config") continue;
        Config cfg;
        if (const std::string* g = cn.Attr("gender")) {
            if (*g == "Male") cfg.gender = Gender::Male; else if (*g == "Female") cfg.gender = Gender::Female;
            else { error = path + ": unknown gender \"" + *g + "\""; return false; }
        }
        for (const XmlNode& kn : cn.children) {
            if (kn.name != "Category") continue;
            Category cat;
            if (const std::string* n = kn.Attr("name")) cat.name = *n;
            for (const XmlNode& rn : kn.children) {
                if (rn.name == "Range") {
                    Range r; bool has = false;
                    if (const std::string* n = rn.Attr("name")) r.name = *n;
                    if (!IntAttr(rn, "min", true, has, r.min, err) || !IntAttr(rn, "max", true, has, r.max, err) ||
                        !IntAttr(rn, "mid", false, r.has_mid, r.mid, err) || !IntAttr(rn, "random_group", false, r.has_random_group, r.random_group, err) ||
                        !IntAttr(rn, "random_variation", false, r.has_random_variation, r.random_variation, err)) { error = path + ": " + err; return false; }
                    if (const std::string* t = rn.Attr("target")) r.target = *t;
                    if (const std::string* t = rn.Attr("target_opposite")) r.target_opposite = *t;
                    cat.ranges.push_back(std::move(r));
                } else if (rn.name == "ColourRange") {
                    ColourRange cr;
                    if (const std::string* n = rn.Attr("name")) cr.name = *n;
                    if (!IntAttr(rn, "mid", false, cr.has_mid, cr.mid, err)) { error = path + ": " + err; return false; }
                    for (const XmlNode& col : rn.children) {
                        if (col.name != "Colour") continue;
                        std::array<int, 3> c{}; bool has = false;
                        if (!IntAttr(col, "r", true, has, c[0], err) || !IntAttr(col, "g", true, has, c[1], err) || !IntAttr(col, "b", true, has, c[2], err)) { error = path + ": " + err; return false; }
                        cr.colours.push_back(c);
                    }
                    cat.colour_ranges.push_back(std::move(cr));
                }
            }
            cfg.categories.push_back(std::move(cat));
        }
        out.configs.push_back(std::move(cfg));
    }
    return true;
}

bool LoadEditorLimitsDir(const std::string& dir, std::vector<EditorLimits>& out, std::string& error) {
    DIR* d = opendir(dir.c_str());
    if (!d) { error = "cannot open " + dir; return false; }
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 16 && n.compare(0, 12, "editor_data_") == 0 && n.compare(n.size() - 4, 4, ".xml") == 0) names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    out.clear();
    for (const auto& n : names) { EditorLimits el; if (!LoadEditorLimits(dir + "/" + n, el, error)) return false; out.push_back(std::move(el)); }
    return true;
}

std::string DumpEditorLimits(const std::vector<EditorLimits>& all) {
    std::ostringstream o;
    auto gname = [](Gender g) { return g == Gender::Male ? "Male" : g == Gender::Female ? "Female" : "-"; };
    for (const auto& el : all)
        for (const auto& cfg : el.configs)
            for (const auto& cat : cfg.categories) {
                for (const auto& r : cat.ranges) {
                    o << el.file << '|' << gname(cfg.gender) << '|' << cat.name << "|range|" << r.name << '|' << r.min << '|' << r.max << '|';
                    if (r.has_mid) o << r.mid;
                    o << '|' << r.target << '|' << r.target_opposite << '|';
                    if (r.has_random_group) o << r.random_group;
                    o << '|';
                    if (r.has_random_variation) o << r.random_variation;
                    o << '\n';
                }
                for (const auto& cr : cat.colour_ranges) {
                    o << el.file << '|' << gname(cfg.gender) << '|' << cat.name << "|colour_range|" << cr.name << '|';
                    if (cr.has_mid) o << cr.mid;
                    o << '|';
                    for (size_t i = 0; i < cr.colours.size(); ++i) o << (i ? ";" : "") << cr.colours[i][0] << ',' << cr.colours[i][1] << ',' << cr.colours[i][2];
                    o << '\n';
                }
            }
    return o.str();
}

} // namespace md::character
