// Ground live HDR / exposure (ours, glue around the ORIGINAL post/hdrfp4.hlsl of the install; the Resolve_HDR node of compositors/main.compositor run as quad passes).
// The engine prepends `#define <entry>_fp4 main` for the plain passes (Downsample2x2Luminance, Downsample3x3, AdaptLuminance, Copy, Downsample3x3Brightpass, BloomBlurH / V),
// or `#define MD_HDR_COMPOSITE` for the final one. The original text is untouched; its function `uniform` parameters land in $Global by their own names.
// Composite: our ground target marks written pixels with alpha 1 (the rest of the scene is drawn elsewhere), so our main discards alpha < 0.5 and then calls the original
// Composite_fp4 (DEVIATION: the original composites the whole rt_hdr; see docs/DEVIATIONS.md when wired).
#ifdef MD_HDR_COMPOSITE
#define Composite_fp4 md_orig_Composite
#endif
#include "@HDR@"
#ifdef MD_HDR_COMPOSITE
#undef Composite_fp4
uniform float md_BLOOM_MAGNITUDE;
uniform float md_EXPOSURE_KEY;
float4 main(v2p input) : SV_Target0
{
    if (inRTT.Sample(g_samLinear, input.texCoord).a < 0.5) discard;
    return md_orig_Composite(input, md_BLOOM_MAGNITUDE, md_EXPOSURE_KEY);
}
#endif
