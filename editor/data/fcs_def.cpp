#include "fcs_def.h"
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace md::fcsdef {
namespace {

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// `//` starts a comment unless it is inside a quoted string.
std::string StripComment(const std::string& s) {
    bool q = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') q = !q;
        else if (!q && s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') return s.substr(0, i);
    }
    return s;
}

std::string Lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// Value tokens: "quoted" (kept with its quotes), (group), or a bare word.
std::vector<std::string> Tokens(const std::string& s) {
    std::vector<std::string> t;
    size_t i = 0;
    while (i < s.size()) {
        if (std::isspace((unsigned char)s[i])) { ++i; continue; }
        size_t a = i;
        if (s[i] == '"') { ++i; while (i < s.size() && s[i] != '"') { if (s[i] == '\\' && i + 1 < s.size()) ++i; ++i; } if (i < s.size()) ++i; }
        else if (s[i] == '(') { while (i < s.size() && s[i] != ')') ++i; if (i < s.size()) ++i; }
        else while (i < s.size() && !std::isspace((unsigned char)s[i])) ++i;
        t.push_back(s.substr(a, i - a));
    }
    return t;
}

std::string Unquote(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        std::string r;
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            if (s[i] == '\\' && i + 2 < s.size() && s[i + 1] == 'n') { r += '\n'; ++i; } else r += s[i];
        }
        return r;
    }
    return s;
}

bool IsNumber(const std::string& s, bool& is_float) {
    if (s.empty()) return false;
    char* e = nullptr;
    std::strtod(s.c_str(), &e);
    if (e == s.c_str() || *e) return false;
    is_float = s.find_first_of(".eE") != std::string::npos;
    return true;
}

bool IsUpperWord(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!(std::isupper((unsigned char)c) || std::isdigit((unsigned char)c) || c == '_')) return false;
    return true;
}

bool ParseEnumBody(const std::string& name, bool shared, const std::string& body, Schema& out, std::string& err) {
    EnumDef e; e.name = name; e.shared = shared;
    int next = 0;
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        std::string comment;
        const size_t c = line.find("//");
        if (c != std::string::npos) { comment = Trim(line.substr(c + 2)); line = line.substr(0, c); }
        std::istringstream items(line);
        std::string item;
        bool first = true;
        while (std::getline(items, item, ',')) {
            item = Trim(item);
            if (item.empty()) continue;
            EnumValue v;
            const size_t eq = item.find('=');
            if (eq != std::string::npos) { v.name = Trim(item.substr(0, eq)); next = std::atoi(Trim(item.substr(eq + 1)).c_str()); }
            else v.name = item;
            v.value = next++;
            if (first && !comment.empty()) v.description = comment;   // a comment after the value is its description
            first = false;
            e.values.push_back(std::move(v));
        }
        if (!comment.empty() && !e.values.empty() && e.values.back().description.empty()) e.values.back().description = comment;
    }
    for (const EnumDef& x : out.enums) if (x.name == name) { err = "duplicate enum " + name; return false; }
    out.enums.push_back(std::move(e));
    return true;
}

void ParseField(const std::string& name, const std::string& value, const std::string& category, Field& f) {
    f.name = name; f.category = category;
    std::vector<std::string> t = Tokens(value);
    if (t.size() >= 2 && t.back().size() >= 2 && t.back().front() == '"') {
        // the last quoted token is the description, but only when something precedes it (`"" "desc"`, `TEXTURE_DDS "desc"`)
        f.description = Unquote(t.back());
        t.pop_back();
    }
    if (t.empty()) return;
    const std::string& v = t[0];
    bool is_float = false;
    const std::string lv = Lower(v);
    if (lv == "true" || lv == "false") { f.kind = FieldKind::Bool; f.default_text = v; }
    else if (v == "null") { f.kind = FieldKind::Null; f.default_text = v; }
    else if (v == "multiline") { f.kind = FieldKind::String; f.multiline = true; }
    else if (v[0] == '"') {
        const std::string s = Unquote(v);
        if (s.find('|') != std::string::npos && s.find("*.") != std::string::npos) f.kind = FieldKind::File;
        else f.kind = FieldKind::String;
        f.default_text = s;
    }
    else if (v[0] == '#' && v.size() == 7) { f.kind = FieldKind::Color; f.default_text = v; }
    else if (IsNumber(v, is_float)) { f.kind = is_float ? FieldKind::Float : FieldKind::Int; f.default_text = v; }
    else if (v == "TEXTURE_DDS" || v == "TEXTURE_ANY") { f.kind = FieldKind::File; f.default_text = v; }
    else if (v == "REF_STRING") { f.kind = FieldKind::RefString; f.ref_type = v; }
    else if (v.find('.') != std::string::npos && !IsUpperWord(v)) { f.kind = FieldKind::Enum; f.default_text = v; f.enum_name = v.substr(0, v.find('.')); }
    else if (IsUpperWord(v)) { f.kind = FieldKind::RefList; f.ref_type = v; }
    else { f.kind = FieldKind::String; f.default_text = v; }
    for (size_t i = 1; i < t.size(); ++i) {
        const std::string& m = t[i];
        bool fl = false;
        if (m == "bitset") f.bitset = true;
        else if (m == "looped") f.looped = true;
        else if (m == "multiline") f.multiline = true;
        else if (m == "SINGLE") f.limit = 1;
        else if (m == "REQUIRED") f.required = true;
        else if (m[0] == '(') f.default_text += (f.default_text.empty() ? "" : " ") + m;
        else if (f.kind == FieldKind::RefList && IsNumber(m, fl) && !fl) f.limit = std::atoi(m.c_str());
        else if (m[0] == '"' && f.kind == FieldKind::File) f.default_text = Unquote(m);
    }
}

void AddLayout(std::vector<LayoutNode>& top, int depth, LayoutNode n) {
    std::vector<LayoutNode>* level = &top;
    for (int d = 0; d < depth && !level->empty(); ++d) level = &level->back().children;
    level->push_back(std::move(n));
}

}  // namespace

const EnumDef* Schema::FindEnum(const std::string& name) const {
    for (const EnumDef& e : enums) if (e.name == name) return &e;
    return nullptr;
}

std::vector<const Field*> Schema::FieldsOf(const std::string& item_type) const {
    std::vector<const Field*> r;
    for (const Section& s : sections)
        for (const std::string& t : s.types)
            if (t == item_type) { for (const Field& f : s.fields) r.push_back(&f); break; }
    return r;
}

bool ParseText(const std::string& text, Schema& out, std::string& err) {
    std::istringstream in(text);
    std::string raw;
    enum { None, Items, Layout, Settings, Theme } mode = None;
    std::string category = "General";
    int lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = Trim(StripComment(raw));
        if (line.empty()) continue;
        // enum [shared] Name { ... } -- may span lines
        if (line.compare(0, 5, "enum ") == 0 && line.find(':') == std::string::npos) {
            std::string body = line;
            while (body.find('}') == std::string::npos && std::getline(in, raw)) { ++lineno; body += "\n" + raw; }
            const size_t ob = body.find('{'), cb = body.rfind('}');
            if (ob == std::string::npos || cb == std::string::npos) { err = "line " + std::to_string(lineno) + ": enum without braces"; return false; }
            std::string head = Trim(body.substr(4, ob - 4));
            bool shared = false;
            if (head.compare(0, 7, "shared ") == 0) { shared = true; head = Trim(head.substr(7)); }
            if (!ParseEnumBody(head, shared, body.substr(ob + 1, cb - ob - 1), out, err)) { err = "line " + std::to_string(lineno) + ": " + err; return false; }
            continue;
        }
        if (line[0] == '[') {
            const size_t e = line.find(']');
            if (e == std::string::npos) { err = "line " + std::to_string(lineno) + ": unterminated section"; return false; }
            const std::string name = Trim(line.substr(1, e - 1));
            category = "General";
            if (name == "FCS_LAYOUT") mode = Layout;
            else if (name == "FCS_SETTINGS") mode = Settings;
            else if (name == "FCS_THEME") mode = Theme;
            else {
                mode = Items;
                Section s;
                std::istringstream ts(name); std::string t;
                while (std::getline(ts, t, ',')) { t = Trim(t); if (!t.empty()) s.types.push_back(t); }
                out.sections.push_back(std::move(s));
            }
            continue;
        }
        if (mode == None) continue;
        const size_t colon = line.find(':');
        if (mode == Layout) {
            if (colon == std::string::npos) continue;
            int depth = 0;
            for (char c : raw) { if (c == '\t') ++depth; else if (c == ' ') continue; else break; }
            LayoutNode n; n.name = Trim(line.substr(0, colon));
            const std::vector<std::string> t = Tokens(Trim(line.substr(colon + 1)));
            if (!t.empty()) n.type = t[0];
            if (t.size() > 1) n.filter = Unquote(t[1]);
            AddLayout(out.layout, depth, std::move(n));
            continue;
        }
        if (mode == Settings || mode == Theme) {
            if (colon == std::string::npos) continue;
            (mode == Settings ? out.settings : out.theme)[Trim(line.substr(0, colon))] = Trim(line.substr(colon + 1));
            continue;
        }
        Section& s = out.sections.back();
        if (line.compare(0, 10, "condition ") == 0) {   // condition "field" if "other" is A or B ...
            const size_t q1 = line.find('"'), q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
            if (q2 == std::string::npos) { err = "line " + std::to_string(lineno) + ": bad condition"; return false; }
            const std::string target = line.substr(q1 + 1, q2 - q1 - 1);
            for (Field& f : s.fields) if (f.name == target) { f.condition = Trim(line.substr(q2 + 1)); if (f.condition.compare(0, 3, "if ") == 0) f.condition = Trim(f.condition.substr(3)); }
            continue;
        }
        if (colon == std::string::npos) { err = "line " + std::to_string(lineno) + ": expected `name: value`"; return false; }
        const std::string key = Trim(line.substr(0, colon)), val = Trim(line.substr(colon + 1));
        if (key == "OWNED") { std::istringstream os(val); std::string o; while (std::getline(os, o, ',')) { o = Trim(o); if (!o.empty()) s.owned.push_back(o); } continue; }
        if (key == "TRANSLATE") { s.translate = val; continue; }
        if (val.empty()) { category = key; continue; }
        Field f;
        ParseField(key, val, category, f);
        s.fields.push_back(std::move(f));
    }
    return true;
}

bool LoadDir(const std::string& dir, Schema& out, std::string& err) {
    static const char* kFiles[] = { "fcs_theme.def", "fcs_settings.def", "fcs_layout.def", "fcs_enums.def", "fcs.def" };
    int found = 0;
    for (const char* f : kFiles) {
        std::ifstream in(dir + "/" + f, std::ios::binary);
        if (!in) continue;
        ++found;
        std::stringstream ss; ss << in.rdbuf();
        std::string e;
        if (!ParseText(ss.str(), out, e)) { err = std::string(f) + ": " + e; return false; }
    }
    if (!found) { err = "no fcs*.def in " + dir; return false; }
    return true;
}

}  // namespace md::fcsdef
