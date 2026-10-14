#include "bod2.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace md::character {
namespace {

struct Reader {
    const std::vector<uint8_t>& d; size_t p = 0; bool ok = true;
    explicit Reader(const std::vector<uint8_t>& b) : d(b) {}
    bool Need(size_t n) { if (p + n > d.size()) ok = false; return ok; }
    uint32_t U32() { if (!Need(4)) return 0; uint32_t v; std::memcpy(&v, &d[p], 4); p += 4; return v; }
    int32_t S32() { return int32_t(U32()); }
    float F32() { uint32_t u = U32(); float f; std::memcpy(&f, &u, 4); return f; }
    uint8_t U8() { if (!Need(1)) return 0; return d[p++]; }
    std::string Str() { uint32_t n = U32(); if (!ok || !Need(n)) return {}; std::string s(reinterpret_cast<const char*>(&d[p]), n); p += n; return s; }
};

template <class T, class F> void ReadBlock(Reader& r, std::vector<std::pair<std::string, T>>& out, F value) {
    int32_t n = r.S32();
    for (int32_t i = 0; i < n && r.ok; ++i) { std::string k = r.Str(); out.emplace_back(std::move(k), value()); }
}

struct Writer {
    std::vector<uint8_t> b;
    void U32(uint32_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); }
    void S32(int32_t v) { U32(uint32_t(v)); }
    void F32(float f) { uint32_t u; std::memcpy(&u, &f, 4); U32(u); }
    void U8(uint8_t v) { b.push_back(v); }
    void Str(const std::string& s) { U32(uint32_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }
};

template <class T, class F> void WriteBlock(Writer& w, const std::vector<std::pair<std::string, T>>& v, F value) {
    w.S32(int32_t(v.size()));
    for (const auto& kv : v) { w.Str(kv.first); value(kv.second); }
}

} // namespace

bool ParseBod2(const std::vector<uint8_t>& bytes, Bod2& out, std::string& error) {
    Reader r(bytes); out = Bod2();
    out.file_version = r.S32();
    int fv = out.file_version;
    if (fv != 13 && fv != 15 && fv != 16) { error = "unsupported file version " + std::to_string(fv); return false; }
    if (fv > 15) { out.version = r.S32(); out.author = r.Str(); out.description = r.Str(); out.dependencies = r.Str(); out.references = r.Str(); }
    out.last_id = r.S32();
    int32_t count = r.S32();
    for (int32_t i = 0; i < count && r.ok; ++i) {
        Bod2Record rec;
        rec.unknown = r.S32(); rec.item_type = r.S32(); rec.item_id = r.S32(); rec.name = r.Str(); rec.identifier = r.Str();
        if (fv >= 15) rec.datatype = r.U32();
        else { int32_t n = r.S32(); for (int32_t k = 0; k < n && r.ok; ++k) { std::string s = r.Str(); rec.legacy_flags.emplace_back(std::move(s), r.U8()); } }
        ReadBlock(r, rec.bools, [&] { return r.U8(); });
        ReadBlock(r, rec.floats, [&] { return r.F32(); });
        ReadBlock(r, rec.ints, [&] { return r.S32(); });
        ReadBlock(r, rec.vec3s, [&] { std::array<float, 3> v; for (float& f : v) f = r.F32(); return v; });
        ReadBlock(r, rec.quats, [&] { std::array<float, 4> v; for (float& f : v) f = r.F32(); return v; });
        ReadBlock(r, rec.strings, [&] { return r.Str(); });
        ReadBlock(r, rec.filenames, [&] { return r.Str(); });
        int32_t sections = r.S32();
        for (int32_t s = 0; s < sections && r.ok; ++s) {
            std::string key = r.Str(); int32_t n = r.S32(); std::vector<Bod2Ref> list;
            for (int32_t k = 0; k < n && r.ok; ++k) { Bod2Ref ref; ref.target = r.Str(); ref.v0 = r.S32(); ref.v1 = r.S32(); ref.v2 = r.S32(); list.push_back(ref); }
            rec.refs.emplace_back(std::move(key), std::move(list));
        }
        if (r.S32() != 0) { error = "record with objects is not supported"; return false; }
        out.records.push_back(std::move(rec));
    }
    if (!r.ok) { error = "file too short"; return false; }
    if (r.p != bytes.size()) { error = "trailing bytes: " + std::to_string(bytes.size() - r.p); return false; }
    return true;
}

bool ReadBod2(const std::string& path, Bod2& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return ParseBod2(bytes, out, error);
}

std::vector<uint8_t> WriteBod2(const Bod2& b) {
    Writer w; int fv = b.file_version;
    w.S32(fv);
    if (fv > 15) { w.S32(b.version); w.Str(b.author); w.Str(b.description); w.Str(b.dependencies); w.Str(b.references); }
    w.S32(b.last_id); w.S32(int32_t(b.records.size()));
    for (const Bod2Record& rec : b.records) {
        w.S32(rec.unknown); w.S32(rec.item_type); w.S32(rec.item_id); w.Str(rec.name); w.Str(rec.identifier);
        if (fv >= 15) w.U32(rec.datatype);
        else { w.S32(int32_t(rec.legacy_flags.size())); for (const auto& kv : rec.legacy_flags) { w.Str(kv.first); w.U8(kv.second); } }
        WriteBlock(w, rec.bools, [&](uint8_t v) { w.U8(v); });
        WriteBlock(w, rec.floats, [&](float v) { w.F32(v); });
        WriteBlock(w, rec.ints, [&](int32_t v) { w.S32(v); });
        WriteBlock(w, rec.vec3s, [&](const std::array<float, 3>& v) { for (float f : v) w.F32(f); });
        WriteBlock(w, rec.quats, [&](const std::array<float, 4>& v) { for (float f : v) w.F32(f); });
        WriteBlock(w, rec.strings, [&](const std::string& v) { w.Str(v); });
        WriteBlock(w, rec.filenames, [&](const std::string& v) { w.Str(v); });
        w.S32(int32_t(rec.refs.size()));
        for (const auto& kv : rec.refs) { w.Str(kv.first); w.S32(int32_t(kv.second.size())); for (const Bod2Ref& r : kv.second) { w.Str(r.target); w.S32(r.v0); w.S32(r.v1); w.S32(r.v2); } }
        w.S32(0);
    }
    return w.b;
}

std::string DumpBod2(const std::string& file, const Bod2& b) {
    std::ostringstream o; char buf[64];
    auto fl = [&](float f) { std::snprintf(buf, sizeof buf, "%.9g", double(f)); return std::string(buf); };
    o << "bod2|" << file << "|header|version|" << b.file_version << '\n';
    for (const Bod2Record& r : b.records) {
        o << "bod2|" << file << "|record|" << r.item_type << ':' << r.item_id << "|" << r.name << '|' << r.identifier << '\n';
        for (const auto& kv : r.bools) o << "bod2|" << file << "|bool|" << kv.first << '|' << int(kv.second) << '\n';
        for (const auto& kv : r.floats) o << "bod2|" << file << "|float|" << kv.first << '|' << fl(kv.second) << '\n';
        for (const auto& kv : r.ints) o << "bod2|" << file << "|int|" << kv.first << '|' << kv.second << '\n';
        for (const auto& kv : r.vec3s) o << "bod2|" << file << "|vec3|" << kv.first << '|' << fl(kv.second[0]) << ',' << fl(kv.second[1]) << ',' << fl(kv.second[2]) << '\n';
        for (const auto& kv : r.strings) o << "bod2|" << file << "|string|" << kv.first << '|' << kv.second << '\n';
        for (const auto& kv : r.filenames) o << "bod2|" << file << "|filename|" << kv.first << '|' << kv.second << '\n';
        for (const auto& kv : r.refs) for (const Bod2Ref& x : kv.second) o << "bod2|" << file << "|ref|" << kv.first << '|' << x.target << '\n';
    }
    return o.str();
}

} // namespace md::character
