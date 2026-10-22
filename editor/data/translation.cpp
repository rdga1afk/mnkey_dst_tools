#include "translation.h"
#include <fstream>
#include <set>
#include <sstream>

namespace md::fcsdef {

namespace {
std::string Trim(const std::string& s) { size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r"); return a == std::string::npos ? std::string() : s.substr(a, b - a + 1); }

// `named is false`, `"is sign" is False and "building category" not "DOORS"` -> true when every clause holds on the card
bool ConditionHolds(const std::string& cond, const std::vector<CardField>& card) {
    size_t pos = 0;
    while (pos <= cond.size()) {
        size_t e = cond.find(" and ", pos); if (e == std::string::npos) e = cond.size();
        std::string cl = Trim(cond.substr(pos, e - pos)), name, op, val;
        if (!cl.empty()) {
            size_t i = 0;
            if (cl[0] == '"') { const size_t q = cl.find('"', 1); if (q == std::string::npos) return true; name = cl.substr(1, q - 1); i = q + 1; }
            else { const size_t sp = cl.find(' '); if (sp == std::string::npos) return true; name = cl.substr(0, sp); i = sp; }
            const std::string rest = Trim(cl.substr(i));
            const size_t sp = rest.find(' '); if (sp == std::string::npos) return true;
            op = rest.substr(0, sp); val = Trim(rest.substr(sp + 1));
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
            if (!EvalCondition("\"" + name + "\" " + op + " " + val, card)) return false;
        }
        pos = e + 5;
    }
    return true;
}
}  // namespace

std::string PoEscape(const std::string& s) {
    std::string o; for (char c : s) { if (c == '\n') o += "\\n"; else if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c == '\t') o += "\\t"; else if (c != '\r') o += c; }
    return o;
}
std::string PoUnescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        const char n = s[++i];
        o += n == 'n' ? '\n' : n == 't' ? '\t' : n;
    }
    return o;
}

std::vector<TrEntry> ExtractStrings(const Schema& schema, const md::fcs::GameDataContainer& data, const DataBrowser& browser) {
    std::map<int, std::string> rule;   // kind -> TRANSLATE text
    for (int k = 0; k < md::fcs::ITEM_TYPE_COUNT; ++k)
        for (const Section& s : schema.sections) if (!s.translate.empty()) for (const std::string& t : s.types) if (t == md::fcs::ItemTypeName(k)) rule[k] = s.translate;
    std::vector<TrEntry> out;
    for (const std::string& id : data.order) {
        const md::fcs::GameData* r = data.Get(id); if (!r) continue;
        auto it = rule.find(r->kind); if (it == rule.end()) continue;
        std::string mode = it->second, cond;
        const size_t iff = mode.find(" if "); if (iff != std::string::npos) { cond = Trim(mode.substr(iff + 4)); mode = Trim(mode.substr(0, iff)); }
        if (mode != "NAME" && mode != "ALL") continue;
        if (!cond.empty() && !ConditionHolds(cond, browser.Card(id))) continue;
        const char* type = md::fcs::ItemTypeName(r->kind);
        if (!r->name.empty()) out.push_back({type, r->name, id, "", r->name});
        if (mode == "ALL") for (const auto& kv : r->strings) if (!kv.second.empty()) out.push_back({type, r->name, id, kv.first, kv.second});
    }
    return out;
}

std::string MakePot(const std::vector<TrEntry>& entries) {
    std::ostringstream o;
    o << "msgid \"\"\nmsgstr \"\"\n\"Project-Id-Version: Kenshi\\n\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\"Content-Transfer-Encoding: 8bit\\n\"\n";
    std::set<std::string> seen;
    for (const TrEntry& e : entries) {
        if (!seen.insert(e.msgid).second) continue;
        o << "\n#. Type: " << e.type << "\n#. Name: " << PoEscape(e.name) << "\n#: " << e.string_id << (e.field.empty() ? "" : ":" + e.field) << "\nmsgid \"" << PoEscape(e.msgid) << "\"\nmsgstr \"\"\n";
    }
    return o.str();
}

bool ReadPo(const std::string& path, std::map<std::string, std::string>& out) {
    std::ifstream in(path); if (!in) return false;
    std::string line, id, str; int mode = 0;   // 1 = in msgid, 2 = in msgstr
    bool have = false;
    auto flush = [&] { if (have && !id.empty()) out[id] = str; id.clear(); str.clear(); have = false; mode = 0; };
    auto quoted = [](const std::string& l) { const size_t a = l.find('"'), b = l.rfind('"'); return a == std::string::npos || b <= a ? std::string() : PoUnescape(l.substr(a + 1, b - a - 1)); };
    while (std::getline(in, line)) {
        if (line.compare(0, 6, "msgid ") == 0) { flush(); id = quoted(line); have = true; mode = 1; }
        else if (line.compare(0, 7, "msgstr ") == 0) { str = quoted(line); mode = 2; }
        else if (!line.empty() && line[0] == '"') { (mode == 1 ? id : str) += quoted(line); }
        else if (Trim(line).empty()) flush();
    }
    flush();
    return true;
}

}  // namespace md::fcsdef
