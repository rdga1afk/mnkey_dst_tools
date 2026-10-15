// State of one character-editor session, without any UI: the race / subrace / gender selection, the slider values of the current character and the
// operations of the editor's buttons. Everything comes from the Kenshi install (FCS records, editor_data_*.xml); nothing is listed in code.
// RACE of the editor = a RACE_GROUP record (FCS type 100), SUBRACE = the group's races with `playable` set (spec §3, races.h).
#pragma once
#include "appearance.h"
#include "editor_limits.h"
#include "attachments.h"
#include "races.h"
#include <monkey_dust/io/fcs_gamedata.h>
#include <monkey_dust/io/kenshi_resources.h>
#include <string>
#include <vector>

namespace md::character {

class Session {
public:
    // Reads the install in `kenshi_dir`: merged FCS data, editor limits, resource index. False + `error` when something is missing.
    bool Load(const std::string& kenshi_dir, std::string& error);
    bool Loaded() const { return loaded_; }

    // RACE list: groups that have at least one playable race.
    size_t RaceCount() const { return group_ids_.size(); }
    const RaceGroup& Race(size_t i) const { return races_.groups[group_ids_[i]]; }
    size_t SubraceCount() const { return subraces_.size(); }
    const RaceInfo& Subrace(size_t i) const { return *subraces_[i]; }
    size_t race_index() const { return race_; }
    size_t subrace_index() const { return subrace_; }
    const RaceInfo& Current() const { return *subraces_[subrace_]; }
    bool SingleGender() const { return Current().single_gender; }
    Gender gender() const { return gender_; }
    const Config* Limits() const { return config_; }           // sliders of the current race and gender (null: the race has no editor limits)

    void SelectRace(size_t i);          // resets subrace and sliders
    void SelectSubrace(size_t i);       // resets sliders
    void SetGender(Gender g);           // no-op for a single-gender race; resets sliders

    const Appearance& Values() const { return values_; }
    // Incremented by every change of the selection or of a value: a viewer redraws when it differs from the one it saw.
    uint64_t revision() const { return revision_; }
    // Selects the subrace called `name` (its group becomes the RACE); false when there is none.
    bool SelectSubraceByName(const std::string& name, bool female);
    float Value(const Range& r) const;  // current value of a slider (mid when unset)
    void SetValue(const std::string& slider, float v) { values_.values[slider] = v; ++revision_; }
    void Reset() { if (config_) Reset(*config_, values_); stance_ = DefaultStance(); head_ = 0; ++revision_; }
    // Heads: the race's `heads male` / `heads female` HEAD records that have a `texture map` (M:docs/characters.md "Head"); index 0 is the default.
    size_t HeadCount() const { return heads_.size(); }
    const std::string& HeadId(size_t i) const { return heads_[i]; }
    size_t head() const { return head_; }
    void SelectHead(int i) { if (!heads_.empty()) { head_ = size_t((i + int(heads_.size())) % int(heads_.size())); ++revision_; } }
    // Idle stance: the ANIMATION records `idle_stand_*` of chareditor.mod, record order; default `idle_stand_normal` (exe 0x14007a490, M:docs/characters.md).
    size_t StanceCount() const { return stances_.size(); }
    const std::string& StanceName(size_t i) const { return stances_[i]; }
    size_t stance() const { return stance_; }
    void SelectStance(int i) { if (!stances_.empty()) { stance_ = size_t((i + int(stances_.size())) % int(stances_.size())); ++revision_; } }
    const std::string& StanceAnim() const { static const std::string d = "idle_stand_normal"; return stances_.empty() ? d : stances_[stance_]; }
    void RandomiseCategory(const std::string& category) { if (config_) md::character::RandomiseCategory(*config_, category, values_, rng_); ++revision_; }
    void RandomiseAll() { if (config_) md::character::RandomiseAll(*config_, values_, rng_); RandomiseHair(); }
    // Hair tab (exe 0x14007adc0): a weighted pick of the hair, of the beard (not for women) and of the hair colour; no colours in the race: brightness 100, saturation 0.
    void RandomiseHair();
    void Seed(uint32_t s) { rng_ = Rng(s); }

    // Hair and beard of the current race and gender (RACE `hairs`, by attach slot); index 0 is the first record (a "Bald" / no-mesh entry when the race has one).
    size_t HairCount() const { return hairs_.size(); }
    const AttachmentInfo& Hair(size_t i) const { return hairs_[i]; }
    size_t hair_index() const { return hair_; }
    void SelectHair(size_t i) { if (i < hairs_.size()) { hair_ = i; ++revision_; } }
    size_t BeardCount() const { return beards_.size(); }
    const AttachmentInfo& Beard(size_t i) const { return beards_[i]; }
    size_t beard_index() const { return beard_; }
    void SelectBeard(size_t i) { if (i < beards_.size()) { beard_ = i; ++revision_; } }
    // Hair colour of the appearance values (Hair Colour / Hair Saturation / Hair Brightness, x 0.01 as HSB), RGB 0..1.
    std::array<float, 3> HairRgb() const;
    const md::fcs::GameDataContainer& Data() const { return data_; }
    const std::vector<EditorLimits>& AllLimits() const { return limits_; }
    const md::KenshiResources& Resources() const { return res_; }
    const std::string& KenshiDir() const { return dir_; }

private:
    static void Reset(const Config& c, Appearance& a) { md::character::Reset(c, a); }
    void Refresh();                     // subraces, limits config and default values after a selection change

    bool loaded_ = false;
    std::string dir_;
    md::fcs::GameDataContainer data_;
    RaceData races_;
    std::vector<EditorLimits> limits_;
    md::KenshiResources res_;
    std::vector<size_t> group_ids_;
    size_t race_ = 0, subrace_ = 0;
    std::vector<const RaceInfo*> subraces_;
    std::vector<AttachmentInfo> hairs_, beards_;
    size_t hair_ = 0, beard_ = 0, head_ = 0;
    std::vector<std::string> heads_;
    std::vector<std::string> stances_;
    size_t stance_ = 0;
    size_t DefaultStance() const { for (size_t i = 0; i < stances_.size(); ++i) if (stances_[i] == "idle_stand_normal") return i; return 0; }
    Gender gender_ = Gender::Male;
    const Config* config_ = nullptr;
    Appearance values_;
    Rng rng_{20261014u};
    uint64_t revision_ = 1;
};

} // namespace md::character
