// `.bod2`: a saved character appearance (Kenshi data/character/bodies/*.bod2). An FCS file (file versions 13, 15, 16 occur) with one record of
// type 66 (CHARACTER_APPEARANCE); the slider values are the record's float fields (name = slider name), `sex female` a bool, `race` a reference.
// Layout as in engine/include/monkey_dust/io/fcs.h plus the older v13 record (a legacy flag list instead of the datatype word). Read, write; Write(Read(f)) reproduces
// the bytes of every file of the install (tests/gates/gate_data_editor_limits.sh). C++17 + stdlib only.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace md::character {

struct Bod2Ref { std::string target; int32_t v0 = 0, v1 = 0, v2 = 0; };      // v1 / v2 only stored from file version 10

struct Bod2Record {
    int32_t unknown = 0, item_type = 66, item_id = 0;
    std::string name, identifier;
    uint32_t datatype = 0;                                                   // file version >= 15
    std::vector<std::pair<std::string, uint8_t>> legacy_flags;               // file versions 11 .. 14
    std::vector<std::pair<std::string, uint8_t>> bools;
    std::vector<std::pair<std::string, float>> floats;
    std::vector<std::pair<std::string, int32_t>> ints;
    std::vector<std::pair<std::string, std::array<float, 3>>> vec3s;         // file version > 8
    std::vector<std::pair<std::string, std::array<float, 4>>> quats;
    std::vector<std::pair<std::string, std::string>> strings, filenames;
    std::vector<std::pair<std::string, std::vector<Bod2Ref>>> refs;
};

struct Bod2 {
    int32_t file_version = 0;
    // file version > 15 only
    int32_t version = 0;
    std::string author, description, dependencies, references;
    int32_t last_id = 0;
    std::vector<Bod2Record> records;
};

// False + `error` on a short / unsupported file (version other than 13, 15, 16; a record with objects).
bool ReadBod2(const std::string& path, Bod2& out, std::string& error);
bool ParseBod2(const std::vector<uint8_t>& bytes, Bod2& out, std::string& error);
std::vector<uint8_t> WriteBod2(const Bod2& b);
// One line per field, for the oracle: `bod2|<file>|<kind>|<name>|<value>`; floats as %.9g, refs as the target id (the three reference values are checked by the byte round trip).
std::string DumpBod2(const std::string& file, const Bod2& b);

} // namespace md::character
