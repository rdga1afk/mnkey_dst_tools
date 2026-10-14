// Minimal XML reader for Kenshi's editor_data_*.xml (elements, attributes, comments, a BOM, <?xml ?>); C++17 + stdlib only.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace md::character {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    const std::string* Attr(const std::string& key) const {
        for (const auto& a : attrs) if (a.first == key) return &a.second;
        return nullptr;
    }
};

// Parses a document into its root element. False + a message on malformed input.
bool ParseXml(const std::string& text, XmlNode& root, std::string& error);

} // namespace md::character
