// Edit layer over the loaded game data (#196 F3): the fields the user changed, per record, as the typed values a .mod would hold, with undo / redo.
// The base data (md::fcs::GameDataContainer) is never touched. A field is ORIGINAL (not in the layer) or MODIFIED (in the layer). Values come in as text and are
// parsed by the kind the schema gives the field (bool / int / float / string / file / enum / bitset / colour); vector fields are not in the schema grammar and are not edited here.
#pragma once
#include "fcs_def.h"
#include <monkey_dust/io/fcs_gamedata.h>
#include <array>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace md::fcsdef {

enum class FieldState { Original, Modified };

using EditValue = std::variant<bool, float, int32_t, std::string>;

// A record's state in the layer. OWNED: another record lists it in an `OWNED:` category of its schema section (it is deleted / copied together with that owner).
enum class RecordState { Original, Modified, New, Removed };

class EditSession {
public:
    // `mod_name` ends the string_id of new records ("<id>-<mod_name>", e.g. "18003-md_test.mod").
    EditSession(const Schema& schema, const md::fcs::GameDataContainer& base, std::string mod_name = "md_test.mod") : schema_(schema), base_(base), mod_name_(std::move(mod_name)) { layer_.last_id = base.id_counter; }

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

    // ---- F4: references, instances, new / copied / removed records ----
    using Triple = std::array<int32_t, 3>;
    static constexpr int32_t kRemoved = INT32_MAX;                                  // v2 of a reference the mod removes
    std::string NewRecord(int32_t kind, const std::string& name);                    // string_id of a new empty record (id from the last-id counter)
    std::string Duplicate(const std::string& string_id, const std::string& name);    // copy with its fields, references, instances and OWNED records (those are copied too)
    bool Remove(const std::string& string_id);                                       // REMOVED = true on the record and, recursively, on its OWNED records
    bool Restore(const std::string& string_id);
    RecordState StateOfRecord(const std::string& string_id) const;
    bool Exists(const std::string& string_id) const;                                 // loaded or new, and not removed
    int32_t KindOf(const std::string& string_id) const;                              // -1 unknown
    std::string NameOf(const std::string& string_id) const;
    bool SetName(const std::string& string_id, const std::string& name);
    bool SetRef(const std::string& string_id, const std::string& category, const std::string& target, Triple v);
    bool RemoveRef(const std::string& string_id, const std::string& category, const std::string& target);   // the mod removes the link (written as v2 = INT32_MAX)
    std::vector<std::pair<std::string, Triple>> Refs(const std::string& string_id, const std::string& category) const;   // base + layer, removed links gone
    bool SetInstance(const std::string& string_id, const md::fcs::Instance& inst);   // add (new id) or change (existing id)
    bool RemoveInstance(const std::string& string_id, const std::string& inst_id);   // written as an instance with an empty target
    std::vector<md::fcs::Instance> Instances(const std::string& string_id) const;
    std::vector<std::string> OwnedOf(const std::string& string_id) const;            // records the OWNED categories of the section point to
    std::string OwnerOf(const std::string& string_id) const;                         // "" when no record owns it
    std::vector<std::string> NewRecords() const;
    std::vector<std::string> RemovedRecords() const;
    std::vector<std::string> TouchedRecords() const;                                 // every record with any change, for the writer
    // Raw layer content for the writer (F5).
    const std::map<std::string, std::map<std::string, std::map<std::string, Triple>>>& RefLayer() const { return layer_.refs; }
    const std::map<std::string, std::map<std::string, md::fcs::Instance>>& InstanceLayer() const { return layer_.insts; }
    const std::map<std::string, EditValue>& FieldLayer(const std::string& string_id) const { static const std::map<std::string, EditValue> none; auto it = layer_.fields.find(string_id); return it == layer_.fields.end() ? none : it->second; }
    bool IsRenamed(const std::string& string_id) const { return layer_.names.count(string_id) != 0; }
    int32_t LastId() const { return layer_.last_id; }

private:
    struct NewRec { int32_t kind = 0, id = 0; std::string name; };
    struct Layer {
        std::map<std::string, std::map<std::string, EditValue>> fields;
        std::map<std::string, std::map<std::string, std::map<std::string, Triple>>> refs;
        std::map<std::string, std::map<std::string, md::fcs::Instance>> insts;
        std::map<std::string, NewRec> created;
        std::map<std::string, std::string> names;
        std::set<std::string> removed;
        int32_t last_id = 0;
    };
    void Push();   // snapshot for undo
    const std::vector<std::string>* OwnedCategories(int32_t kind) const;
    std::string DuplicateInner(const std::string& sid, const std::string& name, std::map<std::string, std::string>& copied);
    bool Parse(const Field& f, const std::string& text, EditValue& out, std::string& err) const;
    std::string Format(const Field& f, const EditValue& v) const;
    bool BaseValue(const Field& f, const md::fcs::GameData& rec, EditValue& out) const;

    const Schema& schema_;
    const md::fcs::GameDataContainer& base_;
    std::string mod_name_;
    Layer layer_;
    std::vector<Layer> undo_, redo_;
};

}  // namespace md::fcsdef
