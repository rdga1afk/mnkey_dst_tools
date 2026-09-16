#!/usr/bin/env python3
"""
krok3_decompose_spike.py — КРОК 3 feasibility spike: color/structure
decompose for detail-texture format (docs/DAGOR_IMPLEMENTATION_PROMPT.md
Рівень 1, reopening #12/Фаза 2).

Context: Dagor's micro-detail texture packs brightness-multiplier +
tangent-space normal XY + reflectance into ONE RGBA8, WITHOUT storing
chromatic albedo in it at all (land_micro_detail.dshl:283-291) -- color
comes from elsewhere (a low-frequency layer). This directly contradicts
this project's earlier "packing needs 5 channels, dead end" conclusion.

Before touching any live shader/asset-pipeline code (currently TWO
separate real DDS arrays, tex_ground RGB albedo + tex_ground_nml RGB
normal, engine/src/render/terrain_renderer.cpp:196-233, 124 real Kenshi
layers each), this spike numerically checks: on REAL Kenshi ground
textures, how much does a per-texture flat tint + achromatic-brightness
decomposition actually lose vs the real, full-chromatic texture?

Does NOT touch the engine/shaders/asset pipeline. Pure numerical check
on a few real texture layers from game/data/biome_table.txt.
"""
import re
import sys

import numpy as np
from PIL import Image

OUT_DIR = "/tmp/claude-1001/-home-rdga1-rdga1prj-monkeydust/021d5116-7c77-464a-bd69-2b8160c80074/scratchpad"


def load_layers(n=4):
    layers = []
    with open("game/data/biome_table.txt") as f:
        for line in f:
            m = re.match(r"tex (\d+)\|([^|]+)\|([^|\n]+)", line)
            if not m:
                continue
            idx, dif, nml = m.groups()
            if "_MD_Flat_Normal_NML" in nml:
                continue  # skip flat-normal layers, want real detail
            layers.append((int(idx), dif.strip(), nml.strip()))
            if len(layers) >= n:
                break
    return layers


def decompose_check(dif_path: str, nml_path: str, label: str):
    dif = np.array(Image.open(dif_path).convert("RGB"), dtype=np.float64) / 255.0
    nml = np.array(Image.open(nml_path).convert("RGB"), dtype=np.float64) / 255.0

    # Current format: full RGB albedo, full RGB normal -- 6 channels total
    # across the two arrays.
    #
    # Proposed (Dagor-style) format: ONE tint colour per texture (flat,
    # low-frequency -- would live in a small per-layer table, not a
    # per-texel texture) + per-texel brightness multiplier (R) + normal XY
    # (packed GA) + reflectance/AO (B) = 4 channels in ONE packed texture,
    # replacing both arrays.
    tint = dif.mean(axis=(0, 1))  # per-texture average colour (the "low-freq" part)
    tint_safe = np.clip(tint, 0.05, None)  # avoid div-by-near-zero on dark textures
    brightness_mult = np.clip((dif / tint_safe).mean(axis=2), 0.0, 2.0)  # achromatic R channel

    reconstructed = tint_safe[None, None, :] * brightness_mult[:, :, None]
    reconstructed = np.clip(reconstructed, 0.0, 1.0)

    mae = float(np.mean(np.abs(reconstructed - dif)))
    rmse = float(np.sqrt(np.mean((reconstructed - dif) ** 2)))

    # Hue error specifically (does the reconstructed pixel's COLOR direction
    # drift from the original, not just brightness) -- normalize both to
    # remove luminance, compare chroma.
    def chroma(img):
        lum = img.mean(axis=2, keepdims=True)
        return img - lum
    hue_err = float(np.mean(np.abs(chroma(reconstructed) - chroma(dif))))

    print(f"[{label}] tint={tint.round(3).tolist()}  "
          f"MAE={mae:.4f}  RMSE={rmse:.4f}  hue_err={hue_err:.4f}")

    out = Image.fromarray((np.concatenate([dif, reconstructed], axis=1) * 255).astype(np.uint8))
    out.save(f"{OUT_DIR}/krok3_{label}_orig_vs_reconstructed.png")
    return mae, rmse, hue_err


def main():
    layers = load_layers(n=6)
    print(f"[krok3] testing {len(layers)} real Kenshi ground-texture layers\n")
    results = []
    for idx, dif, nml in layers:
        label = dif.split("/")[-1].replace(".dds", "")
        try:
            r = decompose_check(dif, nml, label)
            results.append((label, *r))
        except Exception as e:
            print(f"[{label}] FAILED: {e}")

    print("\n=== SUMMARY ===")
    print(f"{'layer':30s} {'MAE':>8s} {'RMSE':>8s} {'hue_err':>8s}")
    for label, mae, rmse, hue in results:
        print(f"{label:30s} {mae:8.4f} {rmse:8.4f} {hue:8.4f}")
    if results:
        avg_hue = np.mean([r[3] for r in results])
        print(f"\naverage hue_err across {len(results)} layers: {avg_hue:.4f} "
              f"(0=perfect colour preservation, higher=more chroma lost to "
              f"single-tint approximation)")
    print(f"\nside-by-side PNGs written to {OUT_DIR}")


if __name__ == "__main__":
    main()
