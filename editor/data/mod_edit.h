// Edit layer over the loaded game data (#196 F3): the fields the user changed, per record, as the typed values a .mod would hold, with undo / redo.
// The base data (md::fcs::GameDataContainer) is never touched. A field is ORIGINAL (not in the layer) or MODIFIED (in the layer). Values come in as text and are
// parsed by the kind the schema gives the field (bool / int / float / string / file / enum / bitset / colour); vector fields are not in the schema grammar and are not edited here.
#pragma once
#include "fcs_def.h"
#include <monkey_dust/io/fcs_gamedata.h>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace md::fcsdef {

enum class FieldState { Original, Modified };

using EditValue = std::variant<bool, float, int32_t, std::string>;

class EditSession {
public:
    EditSession(const Schema& schema, const md::fcs::GameDataContainer& base) : schema_(schema), base_(base) {}

    // Set `field` of the record from text. False + `err` when the record / field is unknown or the text does not parse for the field's kind.
    // Setting the value the base already has removes the field from the layer (back to ORIGINAL).
    bool Set(const std::string& string_id, const std::string& field, const std::string& text, std::string& err);
    bool Revert(const std::string& string_id, const std::string& field);   // back to ORIGINAL; false when it was not modified
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }

    FieldState StateOf(const std::string& string_id, const std::string& field) const;
    // The edited value as text (the form the card shows); false when the field is ORIGINAL.
    bool EditedText(const std::string& string_id, const std::string& field, std::string& out) const;
    const EditValue* Edited(const std::string& string_id, const std::string& field) const;
    size_t ModifiedCount() const;
    // Every edited (record, field) in a stable order, for the writer (F5).
    std::vector<std::pair<std::string, std::string>> ModifiedFields() const;

private:
    struct Change { std::string sid, field; bool had_old = false, has_new = false; EditValue old_v, new_v; };
    bool Parse(const Field& f, const std::string& text, EditValue& out, std::string& err) const;
    void Apply(const std::string& sid, const std::string& field, bool has, const EditValue& v);
    std::string Format(const Field& f, const EditValue& v) const;
    const Field* FindField(const std::string& sid, const std::string& field) const;
    bool BaseValue(const Field& f, const md::fcs::GameData& rec, EditValue& out) const;

    const Schema& schema_;
    const md::fcs::GameDataContainer& base_;
    std::map<std::string, std::map<std::string, EditValue>> edits_;
    std::vector<Change> undo_, redo_;
};

}  // namespace md::fcsdef
