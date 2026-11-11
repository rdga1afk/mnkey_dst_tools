// md_character_dump limits <editor_data dir> | races <KENSHI_DIR> [skip <file.mod>] | bod2 <bodies dir> (also checks Write(Read(f)) == f bytes) -- text dumps of md::character for the oracle (tests/test_character.cpp, scripts/gen_char_editor_spec_tables.py)
#include "editor_limits.h"
#include "races.h"
#include "bod2.h"
#include "race_assets.h"
#include "bone_scales.h"
#include <algorithm>
#include <map>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || (argc < 3 && std::string(argv[1]) != "bones")) { std::fprintf(stderr, "usage: md_character_dump limits <dir> | races <KENSHI_DIR>\n"); return 2; }
    std::string mode = argv[1];
    if (mode == "limits") {
        std::vector<md::character::EditorLimits> all; std::string err;
        if (!md::character::LoadEditorLimitsDir(argv[2], all, err)) { std::fprintf(stderr, "ERROR %s\n", err.c_str()); return 1; }
        std::fputs(md::character::DumpEditorLimits(all).c_str(), stdout);
        return 0;
    }
    if (mode == "races") {
        md::fcs::Install inst(argv[2]); md::fcs::LoadOrder order; md::fcs::Error e;
        if (!inst.LoadOrderFor(order, e)) { std::fprintf(stderr, "ERROR load order: %s\n", e.Message().c_str()); return 1; }
        if (argc >= 5 && std::string(argv[3]) == "skip")   // the oracle database kenshi_fcs.db was built without Dialogue.mod
            for (size_t i = 0; i < order.files.size(); ++i) if (order.files[i].name == argv[4]) { order.files.erase(order.files.begin() + i); break; }
        md::fcs::GameDataContainer data;
        if (!data.Load(order, e)) { std::fprintf(stderr, "ERROR load: %s\n", e.Message().c_str()); return 1; }
        std::fputs(md::character::DumpRaces(md::character::LoadRaces(data)).c_str(), stdout);
        return 0;
    }
    if (mode == "records") {   // records <KENSHI_DIR> <itemType number>: every record of that type with all its fields (read-only look at the FCS data)
        if (argc < 4) return 2;
        md::fcs::Install inst(argv[2]); md::fcs::LoadOrder order; md::fcs::Error e;
        if (!inst.LoadOrderFor(order, e)) { std::fprintf(stderr, "ERROR load order: %s\n", e.Message().c_str()); return 1; }
        md::fcs::GameDataContainer data;
        if (!data.Load(order, e)) { std::fprintf(stderr, "ERROR load: %s\n", e.Message().c_str()); return 1; }
        for (const md::fcs::GameData* r : data.OfType(std::atoi(argv[3]))) {
            std::printf("## %s | %s | %s\n", r->string_id.c_str(), r->name.c_str(), data.files[r->source].c_str());
            for (auto& kv : r->bools) std::printf("  b %s=%d\n", kv.first.c_str(), (int)kv.second);
            for (auto& kv : r->ints) std::printf("  i %s=%d\n", kv.first.c_str(), kv.second);
            for (auto& kv : r->floats) std::printf("  f %s=%.6g\n", kv.first.c_str(), (double)kv.second);
            for (auto& kv : r->vec3s) std::printf("  v3 %s=%.6g,%.6g,%.6g\n", kv.first.c_str(), (double)kv.second[0], (double)kv.second[1], (double)kv.second[2]);
            for (auto& kv : r->vec4s) std::printf("  v4 %s=%.6g,%.6g,%.6g,%.6g\n", kv.first.c_str(), (double)kv.second[0], (double)kv.second[1], (double)kv.second[2], (double)kv.second[3]);
            for (auto& kv : r->strings) std::printf("  s %s=%s\n", kv.first.c_str(), kv.second.c_str());
            for (auto& kv : r->files) std::printf("  file %s=%s\n", kv.first.c_str(), kv.second.c_str());
            for (auto& c : r->references) for (auto& t : c.second) std::printf("  ref %s -> %s (%d,%d,%d)\n", c.first.c_str(), t.first.c_str(), t.second[0], t.second[1], t.second[2]);
        }
        return 0;
    }
    if (mode == "poses") {   // poses <KENSHI_DIR> <editor_data dir>
        if (argc < 4) return 2;
        md::fcs::Install inst(argv[2]); md::fcs::LoadOrder order; md::fcs::Error e;
        if (!inst.LoadOrderFor(order, e)) { std::fprintf(stderr, "ERROR load order: %s\n", e.Message().c_str()); return 1; }
        md::fcs::GameDataContainer data;
        if (!data.Load(order, e)) { std::fprintf(stderr, "ERROR load: %s\n", e.Message().c_str()); return 1; }
        std::vector<md::character::EditorLimits> limits; std::string err;
        if (!md::character::LoadEditorLimitsDir(argv[3], limits, err)) { std::fprintf(stderr, "ERROR %s\n", err.c_str()); return 1; }
        md::KenshiResources res; res.Index(argv[2]);
        std::fputs(md::character::DumpPoseCoverage(md::character::LoadRaces(data), limits, res).c_str(), stdout);
        return 0;
    }
    if (mode == "bones") {   // the cases of tests/gates/bones_oracle.py through md::character::ComputeBoneScales (starvation, muscle 0; the order of the oracle: Calf, Boob, ... differs: sorted below)
        struct Case { const char* name; bool female; std::map<std::string, float> v; };
        const std::vector<Case> cases = {
            {"default_m", false, {}},
            {"stork180", false, {{"Height", 119}, {"Frame", 98}, {"Posture", 69}, {"Shoulders", 110}, {"Arm bulk", 95}, {"Leg length", 85}, {"Hands", 102}, {"Chest", 180}, {"Stomach", 126}, {"Waist", 102}, {"Hips", 101}, {"Legs bulk", 104}, {"Legs shape", 95}}},
            {"beard_m", false, {{"Height", 101}, {"Frame", 103}, {"Arm bulk", 113}, {"Chest", 91}, {"Stomach", 112}, {"Waist", 82}, {"Hips", 89}, {"Legs bulk", 86}}},
            {"default_f", true, {{"Breast size", 85}}},
            {"scorch_f", true, {{"Height", 98}, {"Frame", 89}, {"Chest", 101}, {"Breast size", 73}, {"Breast height", 0}, {"Breast spacing", 86}, {"Stomach", 61}, {"Waist", 100}}},
            {"extreme_f", true, {{"Height", 120}, {"Frame", 130}, {"Leg length", 70}, {"Arm bulk", 150}, {"Breast size", 150}, {"Breast height", 140}, {"Breast spacing", 50}, {"Chest", 55}, {"Stomach", 200}, {"Waist", 15}, {"Hips", 140}, {"Shoulders", 140}, {"Hands", 120}, {"Feet", 120}}}};
        for (const Case& c : cases) {
            md::character::BodyScaleInputs in; in.female = c.female; in.has_breast_bones = c.female; in.has_jaw_bone = !c.female;
            for (const auto& b : md::character::ComputeBoneScales(c.v, in)) {
                auto f3 = [](const float* x) { char t[96]; std::snprintf(t, sizeof t, "%.6g,%.6g,%.6g", double(x[0]), double(x[1]), double(x[2])); return std::string(t); };
                std::printf("%s|%s|size|%s|pos|%s\n", c.name, b.bone.c_str(), f3(b.size).c_str(), b.has_positional ? f3(b.positional).c_str() : "-");
            }
        }
        return 0;
    }
    if (mode == "bod2") {
        namespace fs = std::filesystem; std::vector<fs::path> files;
        for (const auto& e : fs::recursive_directory_iterator(argv[2])) if (e.is_regular_file() && e.path().extension() == ".bod2") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& p : files) {
            md::character::Bod2 b; std::string err;
            if (!md::character::ReadBod2(p.string(), b, err)) { std::fprintf(stderr, "ERROR %s: %s\n", p.string().c_str(), err.c_str()); return 1; }
            std::ifstream f(p, std::ios::binary); std::vector<uint8_t> orig((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (md::character::WriteBod2(b) != orig) { std::fprintf(stderr, "ERROR %s: write(read) differs from the file\n", p.string().c_str()); return 1; }
            std::fputs(md::character::DumpBod2(fs::relative(p, argv[2]).generic_string(), b).c_str(), stdout);
        }
        return 0;
    }
    return 2;
}
