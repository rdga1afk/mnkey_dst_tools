// State of one character-editor session, without any UI: the race / subrace / gender selection, the slider values of the current character and the
// operations of the editor's buttons. Everything comes from the Kenshi install (FCS records, editor_data_*.xml); nothing is listed in code.
// RACE of the editor = a RACE_GROUP record (FCS type 100), SUBRACE = the group's races with `playable` set (spec §3, races.h).
#pragma once
#include "appearance.h"
#include "editor_limits.h"
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
    void Reset() { if (config_) Reset(*config_, values_); ++revision_; }
    void RandomiseCategory(const std::string& category) { if (config_) md::character::RandomiseCategory(*config_, category, values_, rng_); ++revision_; }
    void RandomiseAll() { if (config_) md::character::RandomiseAll(*config_, values_, rng_); ++revision_; }
    void Seed(uint32_t s) { rng_ = Rng(s); }

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
    Gender gender_ = Gender::Male;
    const Config* config_ = nullptr;
    Appearance values_;
    Rng rng_{20261014u};
    uint64_t revision_ = 1;
};

} // namespace md::character
