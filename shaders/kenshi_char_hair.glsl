// The hair rule shared by the character shaders (D-007 c): the colour of a hair texel is the hair colour times a texel value (the record's channel), and every character surface writes
// that colour times 0.7 into the lighting. The head-skin layer (value = the layer's multiplier channel) and the hair mesh (value = the mesh texture's brightness channel) both call this,
// so one hair colour gives one albedo scale on both.
#ifndef KENSHI_CHAR_HAIR_GLSL
#define KENSHI_CHAR_HAIR_GLSL
vec3 KenshiHairAlbedo(vec3 hairColour, float value) { return hairColour * clamp(value, 0.0, 1.0) * 0.7; }
#endif
