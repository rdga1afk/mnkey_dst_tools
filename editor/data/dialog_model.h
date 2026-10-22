// Dialogues of the data (#196 F6): packages -> dialogues -> the tree of lines, their conditions and effects, and the edits on them. Everything goes through the
// EditSession (and so through undo / redo and the .mod writer). Structure as the shipped data has it (found by reading it, FCS_SCHEMA section 6):
//   DIALOGUE_PACKAGE.dialogs -> DIALOGUE, v0 of the link = the EventTriggerEnum value that starts it;
//   DIALOGUE.lines / DIALOGUE_LINE.lines -> DIALOGUE_LINE (v0 = 100 in shipped data; the replies below a line);
//   DIALOGUE / DIALOGUE_LINE.conditions -> DIALOG_ACTION (condition name: DialogConditionEnum, who: TalkerEnum, compare by: ComparisonEnum, tag / stringvar: the value);
//   DIALOGUE_LINE.effects -> DIALOG_ACTION (action name: DialogActionEnum, action value);
//   DIALOGUE_LINE text0, text1, ...: the variants of the text, speaker: TalkerEnum.
#pragma once
#include "mod_edit.h"
#include <string>
#include <vector>

namespace md::fcsdef {

constexpr int kItemDialogue = 18, kItemDialogueLine = 19, kItemDialogAction = 31, kItemDialoguePackage = 73;

// Fields the .def files do not describe for DIALOG_ACTION (they live in code): added to the schema so the edit layer can set them. No-op when the schema has them.
void AddDialogActionFields(Schema& s);
// The enums that live in the FCS code (not in fcs_enums.def): values from a JSON map {"Enum": [["NAME", value], ...]}, found by MD_FCS_ENUMS, else ../docs/research/kenshi_specs/fcs_code_enums.json (tools built inside the private main repo); they also complete `enum shared` ones. Returns the number of enums added / completed (0 when no file).
int AddCodeEnums(Schema& s, const std::string& path = "");

struct DialogAction { std::string sid; std::string what; std::string detail; };   // `what`: the condition / action enum name; detail: who, compare, value

struct DialogNode {
    std::string sid, speaker;
    std::vector<std::string> texts;
    std::vector<DialogAction> conditions, effects;
    std::vector<DialogNode> children;
    bool cycle = false;   // the line is already on the path above it
};

struct DialogRef { std::string sid, name; int32_t trigger = 0; std::string trigger_name; };

class DialogModel {
public:
    DialogModel(EditSession& ed, const Schema& schema, const md::fcs::GameDataContainer& base) : ed_(ed), schema_(schema), base_(base) {}
    std::vector<std::string> Packages() const;                         // DIALOGUE_PACKAGE ids, by name
    std::vector<DialogRef> DialoguesOf(const std::string& package) const;
    std::vector<std::string> AllDialogues() const;                     // ids, by name
    DialogNode Tree(const std::string& dialogue_or_line, int max_depth = 64) const;
    size_t CountNodes(const DialogNode& n) const;
    std::vector<std::string> ChildLines(const std::string& dialogue_or_line) const;   // one level, for lazy trees: the shipped lines share replies and loop back, a full Tree() of one dialogue can be huge
    DialogNode Shallow(const std::string& line) const { return Tree(line, 0); }        // the line itself (text, speaker, conditions, effects), no children

    // ---- edits ----
    std::string AddLine(const std::string& parent, const std::string& text, const std::string& speaker_enum_name = "T_ME");   // new DIALOGUE_LINE below parent; its id
    bool RemoveLine(const std::string& parent, const std::string& line);   // unlink + remove the line (and what it owns)
    bool SetText(const std::string& line, int variant, const std::string& text);
    std::string AddCondition(const std::string& owner, const std::string& condition_enum_name, const std::string& who_enum_name, const std::string& compare_enum_name, int value);
    std::string AddEffect(const std::string& line, const std::string& action_enum_name, int value);
    bool RemoveAction(const std::string& owner, const std::string& category, const std::string& action);   // category "conditions" or "effects"
    bool SetTrigger(const std::string& package, const std::string& dialogue, const std::string& trigger_enum_name);   // v0 of the package's link
    std::string NewDialogue(const std::string& name, const std::string& package, const std::string& trigger_enum_name);   // empty dialogue linked from the package

private:
    DialogAction Describe(const std::string& action_sid) const;
    std::string EnumName(const char* enum_name, int value) const;
    bool EnumNumber(const char* enum_name, const std::string& name, int& out) const;
    EditSession& ed_;
    int32_t IntOf(const std::string& sid, const char* field, int32_t def = 0) const;
    bool StrOf(const std::string& sid, const std::string& field, std::string& out) const;
    const Schema& schema_;
    const md::fcs::GameDataContainer& base_;
};

}  // namespace md::fcsdef
