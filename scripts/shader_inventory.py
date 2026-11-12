#!/usr/bin/env python3
"""Shader inventory of the public build (#207 release configuration, CPU only).

Lists every shader file the C++ code names as a string literal ("shaders/<name>.vert|frag|comp") and checks it against the SPIR-V set the public build compiles (scripts/compile_shaders.sh output, variants included).
A literal without a public SPIR-V is missing from a build without the private overlay. Each such file of a world surface must have a run-time
replacement (path 1: our glue over the install's HLSL); the table below names it. Exit 1 when a world-surface shader is missing and has no replacement.
usage: shader_inventory.py --spirv DIR [--src DIR ...]   (--spirv: the build's MD_SPIRV_OUT_DIR; --src defaults: game/src, ../engine/src)
"""
import argparse, os, re, sys

# missing file -> (world surface?, run-time replacement in the public build or "")
REPLACED = {
    "terrain_shading_screenspace": (True, "GroundLivePass::DrawFog (game/src/render/ground_live_fog.hlsl over post/fog.hlsl)"),
    "ground_composite_exp.frag": (True, "GroundLiveHdr (game/src/render/ground_live_hdr.hlsl over post/hdrfp4.hlsl)"),
    "prop_tri.frag": (True, "RegisterPropTriLive (game/src/render/prop_tri_live.hlsl over deferred/triplanar.hlsl)"),
    "water_kenshi.frag": (True, "MD_WATER_LIVE water pass over the install water programs"),
    "kenshi_lum_adapt.comp": (True, "GroundLiveHdr (AdaptLuminance of post/hdrfp4.hlsl)"),
    "sky_pano.frag": (True, "MD_SKY_INSTALL: SkyX_Skydome.hlsl glue (sky_panorama.cpp)"),
    "sky_install_": (True, "run-time SPIR-V of the install SkyX programs (frag_spv_path / vert_spv_path)"),
    "pbr.frag": (False, "named in a comment only"),
    "npc_instanced.frag": (False, ""),
    "deferred_lighting.vert": (False, ""),
}

def replacement(name):
    for k, v in REPLACED.items():
        if name.startswith(k):
            return v
    return (None, "")

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", action="append", default=None)
    ap.add_argument("--spirv", required=True)
    a = ap.parse_args()
    srcs = a.src or [d for d in (os.path.join(root, "game", "src"), os.path.join(root, "..", "engine", "src"), os.path.join(root, "engine", "src")) if os.path.isdir(d)]
    lit = re.compile(r'"shaders/([A-Za-z0-9_]+\.(?:vert|frag|comp))"')
    asked = {}
    for s in srcs:
        for dp, _, fs in os.walk(s):
            for f in fs:
                if f.endswith((".cpp", ".h")):
                    p = os.path.join(dp, f)
                    with open(p, encoding="utf-8", errors="replace") as fh:
                        for i, line in enumerate(fh, 1):
                            for m in lit.finditer(line):
                                asked.setdefault(m.group(1), f"{os.path.relpath(p, root)}:{i}")
    have = {f[:-4] for f in os.listdir(a.spirv) if f.endswith(".spv")} if os.path.isdir(a.spirv) else set()
    if not have:
        print(f"no SPIR-V in {a.spirv}"); return 2
    missing = sorted(n for n in asked if n not in have)
    bad = 0
    for n in missing:
        world, rep = replacement(n)
        state = "replaced: " + rep if rep else ("NO REPLACEMENT (world surface)" if world else ("not a world surface" if world is False else "unclassified"))
        if world and not rep:
            bad += 1
        if world is None:
            bad += 1
        print(f"missing {n:55s} {asked[n]:50s} {state}")
    print(f"{len(asked)} shader files named, {len(missing)} without a public SPIR-V, {bad} world surface / unclassified without a replacement")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
