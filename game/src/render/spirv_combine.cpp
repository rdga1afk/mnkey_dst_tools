#include "spirv_combine.h"
#include <map>
#include <set>

namespace md::hlsl {
namespace {
enum Op : uint32_t { OpName = 5, OpMemberName = 6, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeFunction = 33,
    OpTypePointer = 32, OpVariable = 59, OpLoad = 61, OpDecorate = 71, OpImage = 100, OpSampledImage = 86, OpCopyObject = 83, OpFunctionParameter = 55 };
constexpr uint32_t kDecBinding = 33, kDecDescriptorSet = 34, kScUniformConstant = 0, kScUniform = 2, kScStorageBuffer = 12;
using Inst = std::vector<uint32_t>;
uint32_t OpOf(const Inst& i) { return i[0] & 0xffffu; }
Inst Make(uint32_t op, std::initializer_list<uint32_t> w) { Inst i; i.push_back(((uint32_t)(w.size() + 1) << 16) | op); i.insert(i.end(), w.begin(), w.end()); return i; }
}

bool CombineSamplers(std::vector<uint32_t>& spv, char stage, std::string& err) {
    if (spv.size() < 5 || spv[0] != 0x07230203u) { err = "not a SPIR-V module"; return false; }
    std::vector<Inst> in;
    for (size_t p = 5; p < spv.size();) {
        const uint32_t wc = spv[p] >> 16;
        if (wc == 0 || p + wc > spv.size()) { err = "broken instruction stream"; return false; }
        in.emplace_back(spv.begin() + p, spv.begin() + p + wc); p += wc;
    }
    uint32_t bound = spv[3];
    std::set<uint32_t> imageTypes, samplerTypes; std::map<uint32_t, uint32_t> sampledOf;   // image type -> OpTypeSampledImage id
    std::map<uint32_t, uint32_t> ptrPointee, ptrStorage;
    for (const Inst& i : in) {
        switch (OpOf(i)) {
        case OpTypeImage: if (i.size() > 7 && i[7] == 1 && i[3] != 5 && i[3] != 6) imageTypes.insert(i[1]); break;
        case OpTypeSampler: samplerTypes.insert(i[1]); break;
        case OpTypeSampledImage: sampledOf[i[2]] = i[1]; break;
        case OpTypePointer: ptrStorage[i[1]] = i[2]; ptrPointee[i[1]] = i[3]; break;
        default: break;
        }
    }
    if (samplerTypes.empty()) {   // DX9 `sampler2D` / `samplerCUBE` are combined images already: only the sets move
        std::set<uint32_t> images, ubos;
        for (const Inst& i : in) if (OpOf(i) == OpVariable) { if (i[3] == kScUniformConstant) images.insert(i[2]); else if (i[3] == kScUniform) ubos.insert(i[2]); }
        const uint32_t setImg0 = stage == 'f' ? 2 : 0, setUbo0 = stage == 'f' ? 3 : 1;
        std::vector<uint32_t> res(spv.begin(), spv.begin() + 5);
        for (Inst i : in) {
            if (OpOf(i) == OpDecorate && i.size() >= 4 && i[2] == kDecDescriptorSet) { if (images.count(i[1])) i[3] = setImg0; else if (ubos.count(i[1])) i[3] = setUbo0; }
            res.insert(res.end(), i.begin(), i.end());
        }
        spv.swap(res);
        return true;
    }
    // images behind arrays are not handled
    for (const Inst& i : in) if ((OpOf(i) == OpTypeArray || OpOf(i) == OpTypeRuntimeArray) && (imageTypes.count(i[2]) || samplerTypes.count(i[2]))) { err = "array of textures or samplers"; return false; }
    for (uint32_t t : imageTypes) if (!sampledOf.count(t)) sampledOf[t] = bound++;
    std::set<uint32_t> imagePtrs, samplerPtrs;   // pointer types to the images / samplers
    for (const auto& kv : ptrPointee) {
        if (imageTypes.count(kv.second)) imagePtrs.insert(kv.first);
        if (samplerTypes.count(kv.second)) samplerPtrs.insert(kv.first);
    }
    std::set<uint32_t> dead;   // sampler variables, sampler loads, sampler pointer types, the sampler type
    std::set<uint32_t> imageVars, uboVars, samplerVals;
    std::map<uint32_t, uint32_t> combinedOf;   // image value id -> combined value id
    std::vector<Inst> out; out.reserve(in.size() + 64);
    for (const Inst& i : in) {
        const uint32_t op = OpOf(i);
        if (op == OpTypeSampler) { dead.insert(i[1]); continue; }
        if (op == OpTypeSampledImage && imageTypes.count(i[2])) continue;   // re-emitted right behind its image type (a pointer type may come before the original)
        if (op == OpTypePointer && samplerPtrs.count(i[1])) { dead.insert(i[1]); continue; }
        if (op == OpFunctionParameter && samplerPtrs.count(i[1])) { err = "sampler as a function parameter"; return false; }
        if (op == OpTypeFunction) for (size_t k = 3; k < i.size(); ++k) if (samplerPtrs.count(i[k])) { err = "sampler as a function parameter"; return false; }
        if (op == OpVariable) {
            if (samplerPtrs.count(i[1])) { dead.insert(i[2]); samplerVals.insert(i[2]); continue; }
            if (imagePtrs.count(i[1]) || i[3] == kScUniformConstant) imageVars.insert(i[2]);   // a DX9 `sampler2D` is already a combined image: no split to undo, only the set to move
            else if (i[3] == kScUniform) uboVars.insert(i[2]);
            else if (i[3] == kScStorageBuffer) { err = "storage buffer in a stage with shifted sets"; return false; }
        }
        if (op == OpTypePointer && imagePtrs.count(i[1])) { out.push_back(Make(OpTypePointer, { i[1], i[2], sampledOf[i[3]] })); continue; }
        if (op == 62 && (dead.count(i[1]) || samplerVals.count(i[2]))) continue;   // a store into a sampler copy, or of a sampler value
        if (op == 62 && combinedOf.count(i[2])) { Inst st = i; st[2] = combinedOf[i[2]]; out.push_back(st); continue; }   // a texture handed to a function: the combined value
        if (op == OpLoad && samplerPtrs.empty() == false) {
            // a load through a sampler variable
            if (dead.count(i[3])) { dead.insert(i[2]); samplerVals.insert(i[2]); continue; }
        }
        if (op == OpLoad && imageTypes.count(i[1])) {
            const uint32_t comb = bound++;
            Inst l = i; l[1] = sampledOf[i[1]]; l[2] = comb;
            out.push_back(l);
            out.push_back(Make(OpImage, { i[1], i[2], comb }));
            combinedOf[i[2]] = comb;
            continue;
        }
        if (op == OpSampledImage) {
            auto it = combinedOf.find(i[3]);
            if (it == combinedOf.end()) { err = "a sampled image built from a texture that was not loaded in the same module"; return false; }
            out.push_back(Make(OpCopyObject, { i[1], i[2], it->second }));
            continue;
        }
        out.push_back(i);
        if (op == OpTypeImage && imageTypes.count(i[1])) out.push_back(Make(OpTypeSampledImage, { sampledOf[i[1]], i[1] }));
    }
    // drop names / decorations of removed ids and move the sets
    std::vector<Inst> fin; fin.reserve(out.size());
    const uint32_t setImg = stage == 'f' ? 2 : 0, setUbo = stage == 'f' ? 3 : 1;
    for (Inst& i : out) {
        const uint32_t op = OpOf(i);
        if ((op == OpName || op == OpMemberName || op == OpDecorate) && dead.count(i[1])) continue;
        if (op == 15 && i.size() > 4) {   // OpEntryPoint: SPIR-V 1.4+ lists every global variable in the interface, the removed sampler variables go
            size_t p = 3; while (p < i.size()) { const uint32_t w = i[p++]; if (!((w & 0xff) && (w & 0xff00) && (w & 0xff0000) && (w & 0xff000000))) break; }   // the name string ends at the word that holds the 0 byte
            Inst e(i.begin(), i.begin() + (long)p);
            for (size_t k = p; k < i.size(); ++k) if (!dead.count(i[k])) e.push_back(i[k]);
            e[0] = ((uint32_t)e.size() << 16) | 15u; i = e;
        }
        if (op == OpDecorate && i.size() >= 4 && i[2] == kDecDescriptorSet) {
            if (imageVars.count(i[1])) i[3] = setImg; else if (uboVars.count(i[1])) i[3] = setUbo;
        }
        fin.push_back(i);
    }
    // nothing may still mention a removed id
    for (const Inst& i : fin) {
        const uint32_t op = OpOf(i);
        if (op != 61 && op != 62 && op != 65 && op != 57 && op != OpCopyObject && op != OpSampledImage && op != 15) continue;   // load, store, access chain, call, copy, sampled image, entry point
        for (size_t k = 1; k < i.size(); ++k) if (dead.count(i[k]) && op != 0) { err = "a removed sampler id is still used (op " + std::to_string(op) + ", id " + std::to_string(i[k]) + ")"; return false; }
    }
    std::vector<uint32_t> res(spv.begin(), spv.begin() + 5); res[3] = bound;
    for (const Inst& i : fin) res.insert(res.end(), i.begin(), i.end());
    spv.swap(res);
    return true;
}

}   // namespace md::hlsl

namespace md::hlsl {
bool GlobalBlockMembers(const std::vector<uint32_t>& spv, std::vector<UniformMember>& out, uint32_t& size_bytes) {
    out.clear(); size_bytes = 0;
    std::map<uint32_t, std::string> names; std::map<uint64_t, std::string> memberNames; std::map<uint64_t, uint32_t> offsets, strides;
    std::map<uint32_t, std::vector<uint32_t>> structMembers; std::map<uint32_t, uint32_t> varType, ptrPointee, arrElem, arrLenId, constVal, scalarBits, vecLen, matCols;
    std::map<uint32_t, uint32_t> typeKind;   // 1 scalar, 2 vector, 3 matrix, 4 array, 5 struct
    uint32_t globalStruct = 0;
    auto str = [](const Inst& i, size_t from) { std::string s; for (size_t k = from; k < i.size(); ++k) for (int b = 0; b < 4; ++b) { char c = (char)((i[k] >> (8 * b)) & 255); if (!c) return s; s += c; } return s; };
    std::vector<Inst> in;
    for (size_t p = 5; p < spv.size();) { const uint32_t wc = spv[p] >> 16; if (!wc || p + wc > spv.size()) return false; in.emplace_back(spv.begin() + p, spv.begin() + p + wc); p += wc; }
    for (const Inst& i : in) {
        switch (OpOf(i)) {
        case OpName: names[i[1]] = str(i, 2); break;
        case OpMemberName: memberNames[((uint64_t)i[1] << 32) | i[2]] = str(i, 3); break;
        case 72: if (i[3] == 35) offsets[((uint64_t)i[1] << 32) | i[2]] = i[4]; break;   // OpMemberDecorate Offset
        case OpDecorate: if (i[2] == 6) strides[i[1]] = i[3]; break;                     // ArrayStride
        case 30: { std::vector<uint32_t> m(i.begin() + 2, i.end()); structMembers[i[1]] = m; typeKind[i[1]] = 5; break; }   // OpTypeStruct
        case OpTypePointer: ptrPointee[i[1]] = i[3]; break;
        case OpVariable: varType[i[2]] = i[1]; break;
        case OpTypeArray: arrElem[i[1]] = i[2]; arrLenId[i[1]] = i[3]; typeKind[i[1]] = 4; break;
        case 43: constVal[i[2]] = i.size() > 3 ? i[3] : 0; break;                         // OpConstant
        default: break;
        }
    }
    for (const auto& kv : varType) { auto n = names.find(kv.first); if (n != names.end() && n->second == "") {} }
    for (const auto& kv : names) {
        if (kv.second != "$Global") continue;
        if (varType.count(kv.first)) { auto pp = ptrPointee.find(varType[kv.first]); if (pp != ptrPointee.end()) globalStruct = pp->second; }
        else if (structMembers.count(kv.first)) globalStruct = kv.first;   // the struct type carries the name too
    }
    if (!globalStruct) { for (const auto& kv : structMembers) { auto n = names.find(kv.first); if (n != names.end() && n->second == "$Global") globalStruct = kv.first; } }
    if (!globalStruct) return false;
    const auto& mem = structMembers[globalStruct];
    uint32_t end = 0;
    for (size_t k = 0; k < mem.size(); ++k) {
        UniformMember u; const uint64_t key = ((uint64_t)globalStruct << 32) | (uint32_t)k;
        auto nm = memberNames.find(key); u.name = nm == memberNames.end() ? "" : nm->second;
        auto of = offsets.find(key); if (of == offsets.end()) return false; u.offset = of->second;
        auto st = strides.find(mem[k]); u.array_stride = st == strides.end() ? 0 : st->second;
        out.push_back(u);
        uint32_t bytes = 16;   // a vec4 / float slot; arrays: stride * length
        if (u.array_stride) { auto ln = constVal.find(arrLenId[mem[k]]); bytes = u.array_stride * (ln == constVal.end() ? 1 : ln->second); }
        if (u.offset + bytes > end) end = u.offset + bytes;
    }
    size_bytes = (end + 15) & ~15u;
    return true;
}
}   // namespace md::hlsl
