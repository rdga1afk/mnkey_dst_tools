#include "session.h"
#include <algorithm>


namespace md::character {

bool Session::Load(const std::string& kenshi_dir, std::string& error) {
    loaded_ = false; dir_ = kenshi_dir; data_ = md::fcs::GameDataContainer(); limits_.clear(); group_ids_.clear();
    md::fcs::Install inst(kenshi_dir); md::fcs::LoadOrder order; md::fcs::Error fe;
    if (!inst.LoadOrderFor(order, fe) || !data_.Load(order, fe)) { error = "FCS: " + fe.Message(); return false; }
    races_ = LoadRaces(data_);
    if (!LoadEditorLimitsDir(kenshi_dir + "/data/editor", limits_, error)) return false;
    res_.Index(kenshi_dir);
    for (size_t i = 0; i < races_.groups.size(); ++i) if (!races_.Subraces(races_.groups[i]).empty()) group_ids_.push_back(i);
    // RACE list order: the cycle of the ▶ button in the original editor's video frames (Skeleton, Hive, Shek, Human); other groups follow in file order.
    // НЕВІДОМО (#159): the data has no order field; the exe's list order is not found. The editor starts on Human.
    static const char* kOrderFromFrames[] = {"Skeleton", "Hive", "Shek", "Human"};
    auto rank = [&](size_t g) { for (size_t i = 0; i < 4; ++i) if (races_.groups[g].name == kOrderFromFrames[i]) return int(i); return 4; };
    std::stable_sort(group_ids_.begin(), group_ids_.end(), [&](size_t a, size_t b) { return rank(a) < rank(b); });
    if (group_ids_.empty()) { error = "no playable race in the FCS data"; return false; }
    race_ = 0; subrace_ = 0; gender_ = Gender::Male;
    for (size_t i = 0; i < group_ids_.size(); ++i) if (races_.groups[group_ids_[i]].name == "Human") race_ = i;
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
