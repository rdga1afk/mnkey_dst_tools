// View model of the read-only "Kenshi data" panel (#196): the category tree of `[FCS_LAYOUT]`, the items of a category (filter included) and the card of an
// item with its fields as the schema (`fcs.def`) describes them. The widgets (MyGUI) only draw what this returns. The merged records come from the engine
// reader (md::fcs::GameDataContainer); no second .mod reader.
#pragma once
#include "fcs_def.h"
#include "mod_edit.h"
#include <monkey_dust/io/fcs_gamedata.h>
#include <string>
#include <vector>

namespace md::fcsdef {

struct CardField {
    std::string category, name, type, value, def, description;   // type: bool / float / int / string / colour / enum / file / refs ...
    bool changed = false, edited = false;                                        // the record sets it (differs from the schema default)
};

struct CardInstance { std::string id, target_id, target_name; float pos[3] = {0, 0, 0}; std::vector<std::string> states; };

struct ItemRow { std::string string_id, name; };

class DataBrowser {
public:
    DataBrowser(const Schema& schema, const md::fcs::GameDataContainer& data) : schema_(schema), data_(data) {}

    void SetEdits(const EditSession* e) { edits_ = e; }   // the card then shows the edited values and their state
    // Items of one layout node: records of `node.type` that pass `node.filter` (`a==X;b!=Y;c=true`, all must hold), sorted by name.
    std::vector<ItemRow> Items(const LayoutNode& node) const;
    // The card of a record: every schema field of its item type, in file order; reference lists show their targets.
    std::vector<CardField> Card(const std::string& string_id) const;
    // The placed instances of a record (positions in the data's own units); an instance with an empty target is skipped (removed by a mod).
    std::vector<CardInstance> Instances(const std::string& string_id) const;
    // `field` of `rec` as text (value of the record when set, else the schema default); false when the type has no such field.
    bool FieldText(const md::fcs::GameData& rec, const std::string& field, std::string& out) const;

private:
    bool Passes(const md::fcs::GameData& rec, const std::string& filter) const;
    const Schema& schema_;
    const md::fcs::GameDataContainer& data_;
    const EditSession* edits_ = nullptr;
};

}  // namespace md::fcsdef
