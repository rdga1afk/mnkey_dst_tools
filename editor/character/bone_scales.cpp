#include "bone_scales.h"

namespace md::character {
namespace {
// constants of exe 0x14052e6c0 (.rdata addresses in the spec table)
constexpr float kPercent = 0.01f;        // 0x141682ad8
constexpr float kHipsDiv = 3.0f;         // 0x14168a254
constexpr float kThighY = 0.95f;         // 0x1416bc15c
constexpr float kHipsBlend = 0.6f;       // 0x141696c8c
constexpr float kChestX = 0.45f;         // 0x1416b3c8c
constexpr float kChestZ = 0.9f;          // 0x14168a25c
constexpr float kBoobBelow = 0.75f;      // 0x141696c88
constexpr float kBoobHalf = 0.5f;        // 0x14167e8d8
constexpr float kArmZ = 1.5f;            // 0x14168ad18
constexpr float kClavicleY = 0.3f;       // 0x14168955c
constexpr float kHeadFrame = 0.25f;      // 0x141696c80

float Lerp1(float t, float k) { return (k - 1.f) * t + 1.f; }   // exe 0x140015b63 with param_2 = 1.0
}

std::vector<BoneScale> ComputeBoneScales(const std::map<std::string, float>& values, const BodyScaleInputs& in) {
    auto sv = [&](const char* name) { auto it = values.find(name); return (it == values.end() ? 100.f : it->second) * kPercent; };
    auto raw = [&](const char* name) { auto it = values.find(name); return it == values.end() ? 100.f : it->second; };
    const float A = in.skinny, B = in.muscle;
    const float p198 = Lerp1(A, 0.7f), p1b8 = Lerp1(A, 0.86f), p180 = Lerp1(A, 0.6f), p160 = Lerp1(A, 0.4f);   // 0x141696c70, 0x1416be6cc, 0x141696c8c, 0x141696c90
    const float q188 = in.female ? Lerp1(B, 1.24f) : Lerp1(B, 1.27f);                                          // 0x1416e5f64 / 0x1416e5f6c
    const float q1a0 = in.female ? Lerp1(B, 1.12f) : Lerp1(B, 1.13f);                                          // 0x1416e5f60 / 0x1416e5f68

    const float h = in.race_scale * sv("Height"), m2h = 2.f - h, ll = sv("Leg length"), fr = in.race_scale * sv("Frame");
    const float y184 = h + ll - 1.f;
    const float lb = sv("Legs bulk") * p198 * q188, ls = sv("Legs shape"), hp = sv("Hips");
    const float base = (ls * lb + (hp - 1.f) / kHipsDiv) * fr;

    std::vector<BoneScale> out;
    auto size = [&](const char* bone, float x, float y, float z) { BoneScale b; b.bone = bone; b.size[0] = x; b.size[1] = y; b.size[2] = z; out.push_back(b); return out.size() - 1; };
    auto positional = [&](size_t i, float x, float y, float z) { out[i].has_positional = true; out[i].positional[0] = x; out[i].positional[1] = y; out[i].positional[2] = z; };
    auto pair = [&](const char* l, const char* r, float x, float y, float z) { size_t a = size(l, x, y, z), b = size(r, x, y, z); return std::make_pair(a, b); };

    // legs
    { auto t = pair("Bip01 R Thigh", "Bip01 L Thigh", base, y184 * kThighY, base); float py = fr * m2h * hp; positional(t.first, 1.f, py, 1.f); positional(t.second, 1.f, py, 1.f); }
    const float calf = (2.f - ls) * lb * fr;
    pair("Bip01 L Calf", "Bip01 R Calf", calf * calf, y184 * calf, calf * calf);

    // torso
    const float ch = sv("Chest") * q1a0 * p1b8, wa = sv("Waist") * p198, st = sv("Stomach") * p180;
    size("Bip01 Pelvis", hp * fr, h, hp * fr);
    const float hpc = (hp - 1.f) * kHipsBlend + 1.f;
    size("Bip01 Spine", hpc * fr, h, hpc * st * fr);
    size("Bip01 Spine1", wa * fr, h, st * fr);
    const float chm = ch - 1.f, chx = chm * kChestX + 1.f, chz = chm * kChestZ + 1.f;
    size("Bip01 Spine2", chx * fr, h, chz * fr);

    // breasts
    if (in.has_breast_bones) {
        const float bs = sv("Breast size") * p160;
        float bx = bs, by = bs;
        if (bs < 1.f) { bx = (bs - 1.f) * kBoobBelow + 1.f; by = (bs - 1.f) * kBoobHalf + 1.f; }
        const float bh = in.female ? sv("Breast height") : 1.f, bsp = in.female ? sv("Breast spacing") : 1.f;
        auto b = pair("L Boob", "R Boob", bx * fr, by * h, bs * fr);
        const float px = bh, py = bsp * chz * fr * ((m2h - 1.f) * kBoobHalf + 1.f), pz = ((m2h - 1.f) + 1.f) * (chz * fr);
        positional(b.first, px, py, pz); positional(b.second, px, py, pz);
    }

    // arms
    const float ab = sv("Arm bulk") * p198 * q188, sho = sv("Shoulders") * q1a0;
    const float ab2 = ab * fr;
    const float armz = ((ab - 1.f) * kArmZ + 1.f) * fr;
    pair("Bip01 L UpperArm", "Bip01 R UpperArm", ab2 * ab2, h * ab2, armz * ab2);
    pair("Bip01 L Forearm", "Bip01 R Forearm", ab2 * ab2, h * ab2, ab2 * ab2);
    pair("Bip01 L Clavicle", "Bip01 R Clavicle", sho * fr, ((sho - 1.f) * kClavicleY + 1.f) * fr, sho * fr);
    { // the UpperArm entries above get their positional size
        float px = sho * chx;
        for (BoneScale& b : out) if (b.bone == "Bip01 L UpperArm" || b.bone == "Bip01 R UpperArm") { b.has_positional = true; b.positional[0] = px; b.positional[1] = 1.f; b.positional[2] = 1.f; }
    }
    const float hd = sv("Hands");
    pair("Bip01 L Hand", "Bip01 R Hand", ab2 * hd * hd, h * hd * hd, ab2 * hd * hd);

    // feet
    const float ft = sv("Feet") * h;
    pair("Bip01 L Foot", "Bip01 R Foot", ft * ft, ll * ft, ft * ft);
    { auto t = pair("Bip01 L Toe0", "Bip01 R Toe0", ft * ft, ft * ft, ft * ft); positional(t.first, 1.f, 1.f, ft); positional(t.second, 1.f, 1.f, ft); }

    // neck, head, jaw
    const float hs = sv("Head size"), hsh = raw("Head shape"), nk = sv("Neck") * p198, nw = sv("Neck width") * p180 * q1a0, nl = raw("Neck length");
    size("Bip01 Neck", nw * fr, nl * kPercent, nk * fr);
    const float hf = (fr - 1.f) * kHeadFrame + 1.f;
    const float hx = hf * hs * hsh * kPercent, hy = hs, hz = hf * hs;
    size("Bip01 Head", hx, hy, hz);
    if (in.has_jaw_bone) size("Bip01 Jaw", sv("Jaw") * hx, hy, hz);
    return out;
}

} // namespace md::character
