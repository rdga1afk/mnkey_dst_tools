#!/usr/bin/env python3
"""md_bake_detail_array.py — КРОК 3 (docs/DAGOR_IMPLEMENTATION_PROMPT.md):
offline bake of the real 124-layer ground-texture set (game/data/
biome_table.txt) from two full-chromatic DDS arrays (tex_ground RGB
albedo + tex_ground_nml RGB normal) into ONE packed achromatic-detail
array + a small per-layer tint table.

Format validated by tools/krok3_decompose_spike.py (GO verdict,
docs/KROK3_DECOMPOSE_SPIKE_RESULT.md, mean hue_err 0.0059 on 6 real
layers) -- this script is that spike's format applied to all 124 real
layers, run once, output committed as a real asset (not regenerated
per-build).

Channel layout differs from Dagor's own convention (.r=brightness,
.ag=normal, .b=reflectance) -- this project's md_bc3_encode.py hardcodes
BC3 alpha to constant-opaque (no caller has ever needed real alpha; see
that file's own doc comment), so packing normal into .ag as Dagor does
would silently lose it to that hardcoded block. Repacked into the RGB
channels BC3 actually compresses (R=brightness, G=normal.x, B=normal.y)
instead -- 3 independent scalar values fit in R5G6B5 same as the
DAGOR reference's 3 non-alpha channels would.

SCOPE NOTE (corrected after re-reading the actual call graph): this
array is ADDITIONAL, not a replacement for tex_ground/tex_ground_nml --
those two stay exactly as-is because they're also the cliff-triplanar
path's (TS_SampleZoneCliffColor/Normal, terrain_cliff_blend.glsl) sole
texture source, which KROK3's validated format was never tested against
and has its own near/far-tiling correctness history (CLIFF_NEAR_TILING_
MULT, grazing-angle LOD bias) that's out of scope to touch here. This
array is consumed ONLY by TS_SampleZoneFlatDetail (base/slope/grass/
dirt/road, the flat-ground role). Net effect: MORE total VRAM resident
(a third ~124-layer array alongside the existing two), not less --
the real win is fewer texture FETCHES per pixel in the flat-detail hot
path (10/call -> 5/call), not a memory reduction. If cliff ever adopts
the same format later, revisit whether tex_ground/tex_ground_nml can
then be retired -- not attempted in this pass.

Reflectance (Dagor's .b) is not carried -- TS_SampleZoneFlatDetail's
current signature has no reflectance/specular output to feed it, so
there's nothing to lose by omitting it. Add a channel back later only
if a real caller needs one.

Output (private -- source DDS are real Kenshi RE data, tmp_/kenshi_re/,
per .claude/rules/asset-pipeline-paths.md's "private RE data never in
public dirs" rule; game/data/ is main-repo-private, correct for this):
  game/data/terrain_detail_baked/detail_<idx>.dds   -- 124 packed DDS
  game/data/terrain_detail_baked/tint_table.dds      -- 128x1 RGBA8 tint
                                                          lookup (nearest,
                                                          index=layer/128)

Usage:
  python3 tools/md_bake_detail_array.py            # bake all 124 layers
  python3 tools/md_bake_detail_array.py --verify    # bake + print MAE/RMSE/hue_err summary
"""
import os
import re
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from md_bc3_encode import encode_bc3_dds_with_mips  # noqa: E402

OUT_DIR = "game/data/terrain_detail_baked"
BIOME_TABLE = "game/data/biome_table.txt"


def load_all_layers():
    layers = []
    with open(BIOME_TABLE) as f:
        for line in f:
            m = re.match(r"tex (\d+)\|([^|]+)\|([^|\n]+)", line)
            if not m:
                continue
            idx, dif, nml = m.groups()
            layers.append((int(idx), dif.strip(), nml.strip()))
    layers.sort(key=lambda t: t[0])
    return layers


def pack_layer(dif_path, nml_path):
    """Returns (packed_rgb_u8 HxWx3, tint_rgb float64[3])."""
    dif = np.array(Image.open(dif_path).convert("RGB"), dtype=np.float64) / 255.0

    tint = dif.mean(axis=(0, 1))
    tint_safe = np.clip(tint, 0.05, None)
    brightness = np.clip((dif / tint_safe).mean(axis=2), 0.0, 2.0)  # 0..2
    r_chan = (brightness * 0.5 * 255.0).round().clip(0, 255).astype(np.uint8)  # store /2

    # _MD_Flat_Normal_NML.dds placeholder layers (real Kenshi has no per-
    # layer normal map for most biomes) decode to a flat "pointing up in
    # tangent space" normal (nx=0,ny=0) -- consistent with the existing
    # tex_ground_nml array's own handling of the same placeholder.
    if "_MD_Flat_Normal_NML" in nml_path:
        h, w = dif.shape[:2]
        g_chan = np.full((h, w), 128, dtype=np.uint8)
        b_chan = np.full((h, w), 128, dtype=np.uint8)
    else:
        nml = np.array(Image.open(nml_path).convert("RGB"), dtype=np.float64) / 255.0
        if nml.shape[:2] != dif.shape[:2]:
            nml_img = Image.open(nml_path).convert("RGB").resize(
                (dif.shape[1], dif.shape[0]), Image.BILINEAR)
            nml = np.array(nml_img, dtype=np.float64) / 255.0
        g_chan = (nml[:, :, 0] * 255.0).round().clip(0, 255).astype(np.uint8)
        b_chan = (nml[:, :, 1] * 255.0).round().clip(0, 255).astype(np.uint8)

    packed = np.stack([r_chan, g_chan, b_chan], axis=-1)
    return packed, tint


def main():
    verify = "--verify" in sys.argv
    os.makedirs(OUT_DIR, exist_ok=True)
    layers = load_all_layers()
    print(f"[bake] {len(layers)} real layers from {BIOME_TABLE}")

    tints = np.zeros((128, 3), dtype=np.float64)  # 128-wide tint table (124 used, headroom)
    verify_results = []

    for idx, dif_path, nml_path in layers:
        packed, tint = pack_layer(dif_path, nml_path)
        tints[idx] = tint

        dds_bytes = encode_bc3_dds_with_mips(packed)
        out_path = f"{OUT_DIR}/detail_{idx}.dds"
        with open(out_path, "wb") as f:
            f.write(dds_bytes)

        if verify:
            # Reconstruct albedo from packed+tint, compare to the ORIGINAL
            # dif image (pre-BC3, since BC3's own ~2.7/255 error is already
            # separately characterized in md_bc3_encode.py's own doc
            # comment -- this checks the DECOMPOSE step alone, same metric
            # the spike used).
            dif = np.array(Image.open(dif_path).convert("RGB"), dtype=np.float64) / 255.0
            tint_safe = np.clip(tint, 0.05, None)
            brightness = np.clip((dif / tint_safe).mean(axis=2), 0.0, 2.0)
            reconstructed = np.clip(tint_safe[None, None, :] * brightness[:, :, None], 0.0, 1.0)
            mae = float(np.mean(np.abs(reconstructed - dif)))
            rmse = float(np.sqrt(np.mean((reconstructed - dif) ** 2)))
            lum = lambda im: im.mean(axis=2, keepdims=True)
            hue_err = float(np.mean(np.abs((reconstructed - lum(reconstructed)) - (dif - lum(dif)))))
            verify_results.append((idx, dif_path.split("/")[-1], mae, rmse, hue_err))

        if idx % 20 == 0:
            print(f"[bake] layer {idx}/{len(layers)}...")

    tint_u8 = (tints * 255.0).round().clip(0, 255).astype(np.uint8)
    tint_rgba = np.concatenate([tint_u8, np.full((128, 1), 255, dtype=np.uint8)], axis=1)
    tint_img = tint_rgba.reshape(1, 128, 4)
    Image.fromarray(tint_img, mode="RGBA").save(f"{OUT_DIR}/tint_table.png")
    print(f"[bake] tint table written: {OUT_DIR}/tint_table.png (128x1 RGBA8, PNG not DDS -- "
          f"tiny, nearest-sampled, no mip/compression needed)")

    print(f"[bake] {len(layers)} packed DDS written to {OUT_DIR}/")

    if verify_results:
        print("\n=== VERIFY (decompose-step error, BC3's own ~2.7/255 error is separate) ===")
        maes = [r[2] for r in verify_results]
        rmses = [r[3] for r in verify_results]
        hues = [r[4] for r in verify_results]
        print(f"mean MAE={np.mean(maes):.4f}  mean RMSE={np.mean(rmses):.4f}  mean hue_err={np.mean(hues):.4f}")
        print(f"worst hue_err: {max(verify_results, key=lambda r: r[4])}")


if __name__ == "__main__":
    main()
