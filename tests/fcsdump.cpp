// md_fcsdump <file.base|file.mod>  -- dumps every record of an FCS file as one JSON object per line, strings as hex of their raw
// bytes, floats as the shortest decimal that round-trips float32. Used by tests/verify_fcs_vs_db.py to compare the C++ reader with
// the Python extraction (re/re_/kenshi/kenshi_fcs.db). Development tool, not part of the game.
#include <monkey_dust/io/fcs.h>
#include <cstdio>
#include <string>

using namespace md::fcs;

static std::string Hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string o; o.reserve(s.size() * 2 + 2); o += '"';
    for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; }
    o += '"'; return o;
}
static std::string Flt(float f) { char b[40]; snprintf(b, sizeof b, "%.9g", f); return b; }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: md_fcsdump <file>\n"); return 2; }
    File f; Error e;
    if (!ReadFile(argv[1], f, e)) { fprintf(stderr, "ERROR %s: %s\n", argv[1], e.Message().c_str()); return 1; }
    const Header& h = f.header;
    printf("{\"header\":{\"filetype\":%d,\"version\":%d,\"author\":%s,\"description\":%s,\"dependencies\":%s,\"references\":%s,\"tail\":%zu,\"id_counter\":%d,\"records\":%d}}\n",
           h.filetype, h.version, Hex(h.author).c_str(), Hex(h.description).c_str(), Hex(h.dependencies).c_str(), Hex(h.references).c_str(),
           h.tail.size(), h.id_counter, h.record_count);
    for (const Record& r : f.records) {
        std::string o = "{\"kind\":" + std::to_string(r.kind) + ",\"id\":" + std::to_string(r.id) + ",\"name\":" + Hex(r.name) +
                        ",\"sid\":" + Hex(r.string_id) + ",\"datatype\":" + std::to_string(r.datatype) + ",\"lead\":" + std::to_string(r.lead);
        o += ",\"bools\":["; for (size_t i = 0; i < r.bools.size(); ++i) { if (i) o += ','; o += "[" + Hex(r.bools[i].first) + "," + (r.bools[i].second ? "1" : "0") + "]"; }
        o += "],\"floats\":["; for (size_t i = 0; i < r.floats.size(); ++i) { if (i) o += ','; o += "[" + Hex(r.floats[i].first) + "," + Flt(r.floats[i].second) + "]"; }
        o += "],\"ints\":["; for (size_t i = 0; i < r.ints.size(); ++i) { if (i) o += ','; o += "[" + Hex(r.ints[i].first) + "," + std::to_string(r.ints[i].second) + "]"; }
        o += "],\"vec3s\":["; for (size_t i = 0; i < r.vec3s.size(); ++i) { if (i) o += ','; const auto& v = r.vec3s[i].second; o += "[" + Hex(r.vec3s[i].first) + ",[" + Flt(v[0]) + "," + Flt(v[1]) + "," + Flt(v[2]) + "]]"; }
        o += "],\"vec4s\":["; for (size_t i = 0; i < r.vec4s.size(); ++i) { if (i) o += ','; const auto& v = r.vec4s[i].second; o += "[" + Hex(r.vec4s[i].first) + ",[" + Flt(v[0]) + "," + Flt(v[1]) + "," + Flt(v[2]) + "," + Flt(v[3]) + "]]"; }
        o += "],\"strings\":["; for (size_t i = 0; i < r.strings.size(); ++i) { if (i) o += ','; o += "[" + Hex(r.strings[i].first) + "," + Hex(r.strings[i].second) + "]"; }
        o += "],\"files\":["; for (size_t i = 0; i < r.files.size(); ++i) { if (i) o += ','; o += "[" + Hex(r.files[i].first) + "," + Hex(r.files[i].second) + "]"; }
        o += "],\"refs\":["; for (size_t i = 0; i < r.references.size(); ++i) { if (i) o += ','; o += "[" + Hex(r.references[i].first) + ",[";
            for (size_t j = 0; j < r.references[i].second.size(); ++j) { const Reference& rf = r.references[i].second[j]; if (j) o += ','; o += "[" + Hex(rf.target) + "," + std::to_string(rf.values[0]) + "," + std::to_string(rf.values[1]) + "," + std::to_string(rf.values[2]) + "]"; }
            o += "]]"; }
        o += "],\"objects\":["; for (size_t i = 0; i < r.instances.size(); ++i) { if (i) o += ','; const Instance& n = r.instances[i];
            o += "[" + Hex(n.id) + "," + Hex(n.target) + ",[" + Flt(n.position[0]) + "," + Flt(n.position[1]) + "," + Flt(n.position[2]) + "],[" +
                 Flt(n.rotation[0]) + "," + Flt(n.rotation[1]) + "," + Flt(n.rotation[2]) + "," + Flt(n.rotation[3]) + "],[";
            for (size_t j = 0; j < n.states.size(); ++j) { if (j) o += ','; o += Hex(n.states[j]); }
            o += "]]"; }
        o += "]}";
        puts(o.c_str());
    }
    return 0;
}
