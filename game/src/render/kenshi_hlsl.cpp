#include "kenshi_hlsl.h"
#include <glslang/Public/ShaderLang.h>
#include "spirv_combine.h"
#include <spirv-tools/optimizer.hpp>
#include <spirv-tools/libspirv.hpp>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <regex>
#include <sstream>
#include <sys/stat.h>

namespace md::hlsl {
namespace {

const char* kFlags = "hlsl-dx9-compatible;vulkan1.1;spv1.3;fmt10";

std::string ReadFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

uint64_t Fnv(uint64_t h, const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } return h; }

std::string CacheDir() {
    const char* x = getenv("XDG_CACHE_HOME"); const char* home = getenv("HOME");
    std::string base = x && *x ? x : (home ? std::string(home) + "/.cache" : "/tmp");
    std::string d = base + "/monkeydust"; mkdir(d.c_str(), 0755); d += "/shaders"; mkdir(d.c_str(), 0755);
    return d;
}


// DX9 lets a function change its `uniform` parameters (they are local copies); glslang makes them constant. Every uniform parameter that the function body assigns is
// renamed `<name>__w` and copied into a local of the original name at the top of the body, so the original source is used as it is.
// The preprocessor conditions that enclose `pos` (#if / #ifdef / #ifndef, #else as the negation), as text lines to open and the count of #endif lines to close: a parameter declared under
// `#ifdef DUST` is only in the signature when DUST is set, so the local that replaces it must be under the same condition.
static void EnclosingConditions(const std::string& src, size_t pos, std::string& open_lines, int& depth_out) {
    std::vector<std::string> stack;   // the condition of every open #if as a C expression
    size_t i = 0;
    while (i < pos) {
        size_t e = src.find('\n', i); if (e == std::string::npos) e = src.size();
        size_t k = i; while (k < e && (src[k] == ' ' || src[k] == '\t')) ++k;
        if (k < e && src[k] == '#') {
            ++k; while (k < e && (src[k] == ' ' || src[k] == '\t')) ++k;
            size_t w = k; while (w < e && isalpha((unsigned char)src[w])) ++w;
            const std::string dir = src.substr(k, w - k);
            std::string arg = src.substr(w, e - w); size_t c = arg.find("//"); if (c != std::string::npos) arg.resize(c);
            while (!arg.empty() && isspace((unsigned char)arg.back())) arg.pop_back();
            while (!arg.empty() && isspace((unsigned char)arg.front())) arg.erase(arg.begin());
            if (dir == "ifdef") stack.push_back("defined(" + arg + ")");
            else if (dir == "ifndef") stack.push_back("!defined(" + arg + ")");
            else if (dir == "if") stack.push_back("(" + arg + ")");
            else if (dir == "else" && !stack.empty()) stack.back() = "!(" + stack.back() + ")";
            else if (dir == "elif" && !stack.empty()) stack.back() = "(!(" + stack.back() + ") && (" + arg + "))";   // approximation: the earlier branches' negation is not tracked
            else if (dir == "endif" && !stack.empty()) stack.pop_back();
        }
        i = e + 1;
    }
    open_lines.clear();
    for (const auto& c : stack) open_lines += "\n#if " + c;
    open_lines += "\n"; depth_out = (int)stack.size();
}

// DX9 allows a flat list for an array of vectors (`float2 o[2] = { 0.5f, 0.5f, -0.5f, 0.5f };`); glslang wants one constructor per element. Only a brace list of exactly N * dim plain values is regrouped.
static std::string GroupFlatVectorArrayInitialisers(const std::string& src) {
    static const std::regex decl(R"(\b(float|int|half)([2-4])\s+(\w+)\s*\[\s*(\d+)\s*\]\s*=\s*\{([^{}]*)\})");
    std::string out; size_t pos = 0;
    for (std::sregex_iterator it(src.begin(), src.end(), decl), end; it != end; ++it) {
        const int dim = std::stoi((*it)[2]), n = std::stoi((*it)[4]);
        std::vector<std::string> v; std::string cur; const std::string body = (*it)[5];
        for (char c : body) { if (c == ',') { v.push_back(cur); cur.clear(); } else cur += c; }
        if (cur.find_first_not_of(" \t\r\n") != std::string::npos) v.push_back(cur);
        if ((int)v.size() != n * dim || body.find_first_of("()") != std::string::npos) continue;   // already grouped (or not a flat list of plain values)
        std::string rep = (*it)[1].str() + (*it)[2].str() + " " + (*it)[3].str() + "[" + std::to_string(n) + "] = {";
        for (int e = 0; e < n; ++e) {
            rep += (e ? ", " : " ") + (*it)[1].str() + (*it)[2].str() + "(";
            for (int d = 0; d < dim; ++d) { std::string t = v[(size_t)e * dim + d]; t.erase(0, t.find_first_not_of(" \t\r\n")); t.erase(t.find_last_not_of(" \t\r\n") + 1); rep += (d ? ", " : "") + t; }
            rep += ")";
        }
        rep += " }";
        out.append(src, pos, (size_t)it->position(0) - pos); out += rep; pos = (size_t)it->position(0) + (size_t)it->length(0);
    }
    out.append(src, pos, std::string::npos);
    return out;
}

std::string MakeUniformParamsWritableImpl(const std::string& src) {
    static const std::regex param(R"(\buniform\s+((?:float|int|half)[1-4]?(?:x[1-4])?)\s+(\w+)\s*(?=[,)]))");
    std::map<size_t, std::string> ins;   // position -> text inserted there (in order of arrival)
    for (std::sregex_iterator it(src.begin(), src.end(), param), end; it != end; ++it) {
        const size_t at = (size_t)it->position(0);
        size_t open_paren = std::string::npos; int depth = 0;
        for (size_t k = at; k-- > 0;) { if (src[k] == ')') ++depth; else if (src[k] == '(') { if (depth == 0) { open_paren = k; break; } --depth; } }
        if (open_paren == std::string::npos) continue;
        depth = 0; size_t close = std::string::npos;
        for (size_t k = open_paren; k < src.size(); ++k) { if (src[k] == '(') ++depth; else if (src[k] == ')' && --depth == 0) { close = k; break; } }
        if (close == std::string::npos) continue;
        size_t open = close + 1; while (open < src.size() && isspace((unsigned char)src[open])) ++open;
        if (open < src.size() && src[open] == ':') {   // the return semantic
            for (++open; open < src.size() && (isspace((unsigned char)src[open]) || isalnum((unsigned char)src[open]) || src[open] == '_'); ++open) {}
        }
        if (open >= src.size() || src[open] != '{') continue;   // a declaration without a body
        depth = 0; size_t body_end = src.size();
        for (size_t k = open; k < src.size(); ++k) { if (src[k] == '{') ++depth; else if (src[k] == '}' && --depth == 0) { body_end = k; break; } }
        const std::string name = (*it)[2], body = src.substr(open, body_end - open);
        if (!std::regex_search(body, std::regex("\\b" + name + R"(\b\s*(\.[xyzwrgba]+)?\s*[-+*/]?=[^=])"))) continue;
        ins[(size_t)it->position(2) + name.size()] += "__w";
        std::string cond; int nd = 0; EnclosingConditions(src, at, cond, nd);
        std::string decl = " " + std::string((*it)[1]) + " " + name + " = " + name + "__w;";
        if (nd > 0) { decl = cond + decl + "\n"; for (int d = 0; d < nd; ++d) decl += "#endif\n"; }
        ins[open + 1] += decl;
    }
    std::string out; size_t pos = 0;
    for (const auto& e : ins) { out.append(src, pos, e.first - pos); out += e.second; pos = e.first; }
    out.append(src, pos, std::string::npos);
    return out;
}

// glslang counts a VPOS parameter the shader does not use as input location 0 although it drops it from the interface, so every other input starts at 1 and no
// vertex shader matches. Fragment inputs are moved down by one when location 0 is empty and the lowest used one is 1.
void CompactFragmentInputLocations(std::vector<uint32_t>& spv) {
    if (spv.size() < 5) return;
    std::vector<uint32_t> inputs; std::vector<size_t> locs;
    for (size_t i = 5; i < spv.size();) {
        const uint32_t op = spv[i] & 0xffff, len = spv[i] >> 16; if (!len) return;
        if (op == 59 && len >= 4 && spv[i + 3] == 1) inputs.push_back(spv[i + 2]);   // OpVariable, StorageClass Input
        i += len;
    }
    auto isInput = [&](uint32_t id) { for (uint32_t v : inputs) if (v == id) return true; return false; };
    uint32_t lo = ~0u;
    for (size_t i = 5; i < spv.size();) {
        const uint32_t op = spv[i] & 0xffff, len = spv[i] >> 16;
        if (op == 71 && len >= 4 && spv[i + 2] == 30 && isInput(spv[i + 1])) { locs.push_back(i + 3); if (spv[i + 3] < lo) lo = spv[i + 3]; }
        i += len;
    }
    if (lo == 1) for (size_t w : locs) --spv[w];
}

// #include "x": relative to the including file, then to the directories of the dependency files.
class Includer : public glslang::TShader::Includer {
public:
    std::vector<std::string> dirs;
    std::set<std::string> seen;   // a file included twice in one unit is included once (two original programs in one unit repeat their common header)
    IncludeResult* includeLocal(const char* name, const char* includer, size_t) override {
        std::string n = name; std::vector<std::string> tries;
        if (!n.empty() && n[0] == '/') tries.push_back(n);
        std::string inc = includer ? includer : ""; size_t s = inc.rfind('/');
        if (s != std::string::npos) tries.push_back(inc.substr(0, s + 1) + n);
        for (const auto& d : dirs) tries.push_back(d + "/" + n);
        for (const auto& t : tries) {
            std::ifstream f(t, std::ios::binary); if (!f) continue;
            std::stringstream ss; ss << f.rdbuf();
            auto* body = new std::string(seen.insert(t).second ? MakeUniformParamsWritableImpl(GroupFlatVectorArrayInitialisers(ss.str())) : std::string());
            return new IncludeResult(t, body->data(), body->size(), body);
        }
        return nullptr;
    }
    IncludeResult* includeSystem(const char* name, const char* includer, size_t depth) override { return includeLocal(name, includer, depth); }
    void releaseInclude(IncludeResult* r) override { if (!r) return; delete static_cast<std::string*>(r->userData); delete r; }
};

} // namespace

std::string MakeUniformParamsWritable(const std::string& src) { return MakeUniformParamsWritableImpl(GroupFlatVectorArrayInitialisers(src)); }

const char* Version() { return "glslang-1.4.363"; }

// MD_SPV_OPT=1: after legalization and sampler combine, SPIRV-Tools' performance passes run on the result (glslang is called with its optimizer off; fxc-compiled originals reach DXVK optimized). Part of the cache key.
bool SpvOptOn() { static const bool on = [] { const char* e = getenv("MD_SPV_OPT"); return e && e[0] == '1'; }(); return on; }

static void ValidateModule(Compiled& out) {
    spvtools::SpirvTools t(SPV_ENV_VULKAN_1_1); std::string m;
    t.SetMessageConsumer([&m](spv_message_level_t, const char*, const spv_position_t&, const char* msg) { if (m.empty()) m = msg; });
    out.valid = t.Validate(out.spv.data(), out.spv.size());
    out.valid_log = out.valid ? std::string() : m;
    static std::set<std::string> logged;   // log each invalid module once
    if (!out.valid && logged.insert(out.cache_path).second) fprintf(stderr, "[SpvVal] %s: %s\n", out.cache_path.c_str(), m.c_str());
}

bool Compile(const std::string& glue, const std::vector<std::string>& dep_files, char stage, Compiled& out) {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t h = 1469598103934665603ull;
    h = Fnv(h, glue); h = Fnv(h, kFlags); h = Fnv(h, Version()); h = Fnv(h, std::string(1, stage)); if (SpvOptOn()) h = Fnv(h, "spv-perf-passes");
    for (const auto& d : dep_files) h = Fnv(h, ReadFile(d));
    char name[32]; snprintf(name, sizeof(name), "%016llx.spv", (unsigned long long)h);
    const std::string path = CacheDir() + "/" + name;
    out.cache_path = path;
    {
        std::string b = ReadFile(path);
        if (!b.empty() && b.size() % 4 == 0) {
            out.spv.assign(reinterpret_cast<const uint32_t*>(b.data()), reinterpret_cast<const uint32_t*>(b.data()) + b.size() / 4);
            out.from_cache = true; out.compile_ms = 0; ValidateModule(out);
            if (!out.valid) { out.log = "SPIR-V validation: " + out.valid_log; return false; }   // a module that fails the validator is never handed to a pipeline: every caller already falls back on false
            return true;
        }
    }
    static bool inited = false; if (!inited) { glslang::InitializeProcess(); inited = true; }
    EShLanguage lang = stage == 'v' ? EShLangVertex : stage == 'f' ? EShLangFragment : EShLangCompute;
    glslang::TShader sh(lang);
    const char* src = glue.c_str(); sh.setStrings(&src, 1);
    sh.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setEntryPoint("main"); sh.setSourceEntryPoint("main");
    Includer inc; for (const auto& d : dep_files) { size_t s = d.rfind('/'); if (s != std::string::npos) inc.dirs.push_back(d.substr(0, s)); }
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslDX9Compatible);
    if (!sh.parse(GetDefaultResources(), 100, false, msgs, inc)) { out.log = std::string(sh.getInfoLog()) + sh.getInfoDebugLog(); return false; }
    glslang::TProgram prog; prog.addShader(&sh);
    if (!prog.link(msgs)) { out.log = std::string(prog.getInfoLog()); return false; }
    glslang::SpvOptions so; so.disableOptimizer = true;
    glslang::GlslangToSpv(*prog.getIntermediate(lang), out.spv, &so);
    {   // glslang's HLSL output hands textures to functions through local copies, which Vulkan does not allow: the legalization passes inline and remove them
        spvtools::Optimizer opt(SPV_ENV_VULKAN_1_1); std::string lg; opt.SetMessageConsumer([&lg](spv_message_level_t, const char*, const spv_position_t&, const char* m) { lg += m; lg += "\n"; });
        opt.RegisterLegalizationPasses();
        std::vector<uint32_t> legal;
        spvtools::OptimizerOptions oo; oo.set_run_validator(false);   // the input has the very stores the passes remove
        if (!opt.Run(out.spv.data(), out.spv.size(), &legal, oo)) { out.log = "SPIR-V legalization failed: " + lg; return false; }
        out.spv.swap(legal);
    }
    if (stage == 'f') CompactFragmentInputLocations(out.spv);
    if (stage != 'c') { std::string e; if (!CombineSamplers(out.spv, stage, e)) { out.log = "sampler combine: " + e; return false; } }   // SDL_GPU sets: combined samplers
    if (SpvOptOn()) {
        spvtools::Optimizer po(SPV_ENV_VULKAN_1_1); std::string pl; po.SetMessageConsumer([&pl](spv_message_level_t, const char*, const spv_position_t&, const char* m) { pl += m; pl += "\n"; });
        po.RegisterPerformancePasses();
        std::vector<uint32_t> fast; spvtools::OptimizerOptions poo; poo.set_run_validator(false);
        if (!po.Run(out.spv.data(), out.spv.size(), &fast, poo)) { out.log = "SPIR-V performance passes failed: " + pl; return false; }
        out.spv.swap(fast);
    }
    out.from_cache = false;
    out.compile_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ValidateModule(out);
    if (!out.valid) { out.log = "SPIR-V validation: " + out.valid_log; return false; }   // not cached
    std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(out.spv.data()), out.spv.size() * 4);
    return true;
}

} // namespace md::hlsl
