// Translation of the game data (#196 F7): which strings of a record the FCS hands to translators (the schema's `TRANSLATE:` lines), the .pot extraction of them and the
// reading of a translated .po (gettext; Kenshi_translations.txt of the install describes the flow). Lines of the .po file are the only format used here.
#pragma once
#include "data_browser.h"
#include <map>
#include <string>
#include <vector>

namespace md::fcsdef {

struct TrEntry { std::string type, name, string_id, field, msgid; };   // field "" = the record name

// Strings to translate: records of types that have a `TRANSLATE:` rule (NAME = the name, ALL = name + string fields; `if ...` conditions evaluated on the card;
// FIELDS is not understood and gives nothing). Empty strings are skipped.
std::vector<TrEntry> ExtractStrings(const Schema& schema, const md::fcs::GameDataContainer& data, const DataBrowser& browser);
// gettext .pot text for the entries (one block per distinct msgid, with `#. Type` / `#. Name` comments).
std::string MakePot(const std::vector<TrEntry>& entries);
// msgid -> msgstr of a .po file (header and fuzzy-less, untranslated entries with an empty msgstr are kept as empty). False when the file cannot be read.
bool ReadPo(const std::string& path, std::map<std::string, std::string>& out);
// Escapes \n \" \\ \t and the other way.
std::string PoEscape(const std::string& s);
std::string PoUnescape(const std::string& s);

}  // namespace md::fcsdef
