#include "session.h"
#include <algorithm>
#include <cstdlib>

namespace md::character {

bool Session::Load(const std::string& kenshi_dir, std::string& error) {
    loaded_ = false; dir_ = kenshi_dir; data_ = md::fcs::GameDataContainer(); limits_.clear(); group_ids_.clear();
    md::fcs::Install inst(kenshi_dir); md::fcs::LoadOrder order; md::fcs::Error fe;
    if (!inst.LoadOrderFor(order, fe) || !data_.Load(order, fe)) { error = "FCS: " + fe.Message(); return false; }
    races_ = LoadRaces(data_);
    if (!LoadEditorLimitsDir(kenshi_dir + "/data/editor", limits_, error)) return false;
    res_.Index(kenshi_dir);
    for (size_t i = 0; i < races_.groups.size(); ++i) if (!races_.Subraces(races_.groups[i]).empty()) group_ids_.push_back(i);
    // RACE list order: by the numeric id of the RACE_GROUP records (Human, Hive, Shek, Skeleton in the shipped data). НЕВІДОМО (#159): the order the original shows (exe).
    std::stable_sort(group_ids_.begin(), group_ids_.end(), [&](size_t a, size_t b) { return std::atoi(races_.groups[a].id.c_str()) < std::atoi(races_.groups[b].id.c_str()); });
    if (group_ids_.empty()) { error = "no playable race in the FCS data"; return false; }
    race_ = 0; subrace_ = 0; gender_ = Gender::Male;
    Refresh();
    loaded_ = true;
    return true;
}

void Session::Refresh() {
    subraces_ = races_.Subraces(races_.groups[group_ids_[race_]]);
    if (subrace_ >= subraces_.size()) subrace_ = 0;
    if (Current().single_gender) gender_ = Gender::Single;
    else if (gender_ == Gender::Single) gender_ = Gender::Male;
    config_ = nullptr;
    for (const EditorLimits& l : limits_) if (l.file == Current().editor_limits) {
        config_ = l.For(gender_);
        // a single-gender race whose file has Male / Female configs (stick_prince, ...): the Male one. НЕВІДОМО (#159): which one the original takes (exe).
        if (!config_ && gender_ == Gender::Single) config_ = l.For(Gender::Male);
    }
    values_ = Appearance();
    if (config_) Reset(*config_, values_);
    ++revision_;
}

void Session::SelectRace(size_t i) { if (i < group_ids_.size()) { race_ = i; subrace_ = 0; Refresh(); } }
void Session::SelectSubrace(size_t i) { if (i < subraces_.size()) { subrace_ = i; Refresh(); } }
void Session::SetGender(Gender g) { if (!SingleGender() && g != Gender::Single && g != gender_) { gender_ = g; Refresh(); } }

bool Session::SelectSubraceByName(const std::string& name, bool female) {
    for (size_t g = 0; g < group_ids_.size(); ++g)
        for (const RaceInfo* r : races_.Subraces(races_.groups[group_ids_[g]]))
            if (r->name == name) {
                race_ = g; subraces_ = races_.Subraces(races_.groups[group_ids_[g]]);
                subrace_ = 0; for (size_t i = 0; i < subraces_.size(); ++i) if (subraces_[i] == r) subrace_ = i;
                gender_ = female ? Gender::Female : Gender::Male;
                Refresh();
                return true;
            }
    return false;
}

float Session::Value(const Range& r) const {
    auto it = values_.values.find(r.name);
    return it == values_.values.end() ? float(RangeMid(r)) : it->second;
}

} // namespace md::character
