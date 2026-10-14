// Hair and beards: ATTACHMENT records (FCS type 6) worn on the body's skeleton (M:docs/formats/ogre-skeleton.md "Binding worn meshes": mode 1, the mesh's bone assignments index the body's
// skeleton by bone handle). RACE `hairs` lists them; `attach slot` 2 = hair, 13 = beard. Colour of the hair: HSB from the appearance values Hair Colour / Hair Saturation / Hair Brightness
// (x 0.01, Ogre ColourValue::setHSB; exe 0x140532600, M:docs/characters.md "Hair colour").
#pragma once
#include <monkey_dust/io/fcs_gamedata.h>
#include <array>
#include <string>

namespace md::character {

constexpr int kSlotHair = 2, kSlotBeard = 13;

struct AttachmentInfo {
    std::string id, name, mesh, mesh_female, texture, texture_female;
    int slot = 0, alpha_rejection = 128, diffuse_channel = 0, alpha_channel = 0;
    bool HasMesh(bool female) const { return !(female && !mesh_female.empty() ? mesh_female : mesh).empty(); }
    const std::string& Mesh(bool female) const { return female && !mesh_female.empty() ? mesh_female : mesh; }
    const std::string& Texture(bool female) const { return female && !texture_female.empty() ? texture_female : texture; }
};

bool LoadAttachment(const md::fcs::GameDataContainer& data, const std::string& id, AttachmentInfo& out);
// COLOR_DATA record (type 55) `color 1` as 0..255 RGB (the int is 0xRRGGBB).
bool LoadColour(const md::fcs::GameDataContainer& data, const std::string& id, std::array<int, 3>& rgb);
// Ogre ColourValue::setHSB / getHSB for values in 0..1 (hue wraps).
std::array<float, 3> HsbToRgb(float h, float s, float b);
std::array<float, 3> RgbToHsb(float r, float g, float b);

} // namespace md::character
