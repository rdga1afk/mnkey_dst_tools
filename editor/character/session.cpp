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
    if (stances_.empty()) {
        for (const md::fcs::GameData* a : data_.OfType(24)) {
            auto it = a->strings.find("anim name");
            if (it != a->strings.end() && it->second.rfind("idle_stand_", 0) == 0) stances_.push_back(it->second);
        }
        stance_ = DefaultStance();
    }
    values_ = Appearance();
    if (config_) Reset(*config_, values_);
    // hair and beards of this race (female mesh falls back to the male one in AttachmentInfo)
    hairs_.clear(); beards_.clear(); hair_ = beard_ = head_ = 0; heads_.clear();
    for (const std::string& id : gender_ == Gender::Female ? Current().heads_female : Current().heads_male) {
        const md::fcs::GameData* h = data_.Get(id);
        if (h && h->files.count("texture map") && !h->files.at("texture map").empty()) heads_.push_back(id);
    }
    for (size_t i = 0; i < Current().hairs.size(); ++i) {
        AttachmentInfo a;
        if (!LoadAttachment(data_, Current().hairs[i], a)) continue;
        a.weight = std::max(1, std::min(100, i < Current().hair_weights.size() ? Current().hair_weights[i] : 1));
        (a.slot == kSlotBeard ? beards_ : hairs_).push_back(a);
    }
    { AttachmentInfo none; none.name = "None"; beards_.insert(beards_.begin(), none); }   // `beard` may be empty (M:docs/characters.md "Body file")
    // the default hair colour: the first of the race's hair colours (COLOR_DATA `color 1`) as HSB x 100
    for (const std::string& id : Current().hair_colors) {
        std::array<int, 3> rgb;
        if (!LoadColour(data_, id, rgb)) continue;
        auto hsb = RgbToHsb(rgb[0] / 255.f, rgb[1] / 255.f, rgb[2] / 255.f);
        values_.values["Hair Colour"] = hsb[0] * 100.f; values_.values["Hair Saturation"] = hsb[1] * 100.f; values_.values["Hair Brightness"] = hsb[2] * 100.f;
        break;
    }
    ++revision_;
}

void Session::RandomiseHair() {
    // weighted pick; the placeholder "None" at the front of the beards is not a record of the race
    auto pick = [&](const std::vector<AttachmentInfo>& v, size_t first) {
        float total = 0.f; for (size_t i = first; i < v.size(); ++i) total += float(v[i].weight);
        float r = rng_.Next() * total;
        for (size_t i = first; i < v.size(); ++i) { r -= float(v[i].weight); if (r < 0.f) return i; }
        return v.size() > first ? v.size() - 1 : size_t(0);
    };
    if (gender_ != Gender::Female && beards_.size() > 1) beard_ = pick(beards_, 1);
    if (!hairs_.empty()) hair_ = pick(hairs_, 0);
    float total = 0.f; std::vector<float> w;
    for (size_t i = 0; i < Current().hair_colors.size(); ++i) { int v = i < Current().hair_color_weights.size() ? Current().hair_color_weights[i] : 0; w.push_back(float(v == 0 ? 100 : v)); total += w.back(); }
    std::array<int, 3> rgb{};
    bool have = false;
    if (total > 0.f) {
        float r = rng_.Next() * total; size_t k = w.size() - 1;
        for (size_t i = 0; i < w.size(); ++i) { r -= w[i]; if (r < 0.f) { k = i; break; } }
        have = LoadColour(data_, Current().hair_colors[k], rgb);
    }
    if (have) {
        auto hsb = RgbToHsb(rgb[0] / 255.f, rgb[1] / 255.f, rgb[2] / 255.f);
        values_.values["Hair Colour"] = hsb[0] * 100.f; values_.values["Hair Saturation"] = hsb[1] * 100.f; values_.values["Hair Brightness"] = hsb[2] * 100.f;
    } else { values_.values["Hair Brightness"] = 100.f; values_.values["Hair Saturation"] = 0.f; }
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

std::array<float, 3> Session::HairRgb() const {
    auto v = [&](const char* k, float d) { auto it = values_.values.find(k); return (it == values_.values.end() ? d : it->second) * 0.01f; };
    return HsbToRgb(v("Hair Colour", 0.f), v("Hair Saturation", 0.f), v("Hair Brightness", 0.5f));
}

float Session::Value(const Range& r) const {
    auto it = values_.values.find(r.name);
    return it == values_.values.end() ? float(RangeMid(r)) : it->second;
}

} // namespace md::character
