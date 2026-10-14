#include "xml_mini.h"
#include <cctype>

namespace md::character {
namespace {

struct Cursor {
    const std::string& s;
    size_t i = 0;
    bool Eof() const { return i >= s.size(); }
    void SkipWs() { while (!Eof() && std::isspace((unsigned char)s[i])) ++i; }
    bool StartsWith(const char* p) const { return s.compare(i, std::char_traits<char>::length(p), p) == 0; }
};

bool SkipMisc(Cursor& c) {
    for (;;) {
        c.SkipWs();
        if (c.StartsWith("<?")) { size_t e = c.s.find("?>", c.i); if (e == std::string::npos) return false; c.i = e + 2; }
        else if (c.StartsWith("<!--")) { size_t e = c.s.find("-->", c.i); if (e == std::string::npos) return false; c.i = e + 3; }
        else if (c.StartsWith("<!")) { size_t e = c.s.find('>', c.i); if (e == std::string::npos) return false; c.i = e + 1; }
        else return true;
    }
}

bool IsNameChar(char ch) { return std::isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == ':' || ch == '.'; }

bool ParseElement(Cursor& c, XmlNode& out, std::string& err, int depth) {
    if (depth > 32) { err = "nesting too deep"; return false; }
    if (c.Eof() || c.s[c.i] != '<') { err = "expected '<' at " + std::to_string(c.i); return false; }
    ++c.i;
    size_t b = c.i;
    while (!c.Eof() && IsNameChar(c.s[c.i])) ++c.i;
    out.name = c.s.substr(b, c.i - b);
    if (out.name.empty()) { err = "empty element name at " + std::to_string(c.i); return false; }
    for (;;) {
        c.SkipWs();
        if (c.Eof()) { err = "unterminated tag <" + out.name; return false; }
        if (c.s[c.i] == '/') { // <a ... />
            if (c.i + 1 >= c.s.size() || c.s[c.i + 1] != '>') { err = "bad '/' in <" + out.name; return false; }
            c.i += 2; return true;
        }
        if (c.s[c.i] == '>') { ++c.i; break; }
        size_t k = c.i;
        while (!c.Eof() && IsNameChar(c.s[c.i])) ++c.i;
        if (k == c.i) { err = "bad attribute in <" + out.name + "> at " + std::to_string(c.i); return false; }
        std::string key = c.s.substr(k, c.i - k);
        c.SkipWs();
        if (c.Eof() || c.s[c.i] != '=') { err = "attribute '" + key + "' without '='"; return false; }
        ++c.i; c.SkipWs();
        if (c.Eof() || (c.s[c.i] != '"' && c.s[c.i] != '\'')) { err = "attribute '" + key + "' without a quote"; return false; }
        char q = c.s[c.i++];
        size_t e = c.s.find(q, c.i);
        if (e == std::string::npos) { err = "unterminated value of '" + key + "'"; return false; }
        out.attrs.emplace_back(key, c.s.substr(c.i, e - c.i));
        c.i = e + 1;
    }
    for (;;) { // children until </name>
        if (!SkipMisc(c)) { err = "malformed comment / declaration"; return false; }
        if (c.Eof()) { err = "missing </" + out.name + ">"; return false; }
        if (c.StartsWith("</")) {
            c.i += 2; size_t b2 = c.i;
            while (!c.Eof() && IsNameChar(c.s[c.i])) ++c.i;
            if (c.s.substr(b2, c.i - b2) != out.name) { err = "mismatched </" + c.s.substr(b2, c.i - b2) + "> for <" + out.name + ">"; return false; }
            c.SkipWs();
            if (c.Eof() || c.s[c.i] != '>') { err = "bad closing tag"; return false; }
            ++c.i; return true;
        }
        if (c.s[c.i] == '<') { XmlNode child; if (!ParseElement(c, child, err, depth + 1)) return false; out.children.push_back(std::move(child)); }
        else { while (!c.Eof() && c.s[c.i] != '<') ++c.i; } // character data between elements is ignored
    }
}

} // namespace

bool ParseXml(const std::string& text, XmlNode& root, std::string& error) {
    Cursor c{text};
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) c.i = 3;
    if (!SkipMisc(c)) { error = "malformed prolog"; return false; }
    if (!ParseElement(c, root, error, 0)) return false;
    if (!SkipMisc(c)) { error = "malformed epilog"; return false; }
    return true;
}

} // namespace md::character
