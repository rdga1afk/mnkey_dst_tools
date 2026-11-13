#!/usr/bin/env bash
# compile_shaders.sh — compile all monkey_dust GLSL shaders to SPIR-V.
#
# TIER 1  [VULKAN] glslc --target-env=vulkan1.1 -DSPIRV_COMPILE=1
#   Shaders with #ifdef SPIRV_COMPILE guards: UBO uniforms, explicit locations,
#   sampler set/binding. Output: shaders/spirv/<name>.spv  (SDL_GPU ready)
#
# TIER 2  [COMPAT] glslangValidator -G (OpenGL-compat SPIR-V)
#   Complex shaders (pbr, npc_instanced.frag) still using flat uniforms.
#   Output: shaders/spirv/<name>.compat.spv  (validates syntax, NOT SDL_GPU)
#
# SKIP    [SKIP]  fog.glsl — legacy Raylib fragment shader
#
# Usage:
#   ./scripts/compile_shaders.sh            # compile all
#   ./scripts/compile_shaders.sh particle   # compile only shaders matching pattern
#
# Requirements: glslc (Shaderc) + glslangValidator in PATH

set -euo pipefail
# MD_SHADER_SRC (colon-separated source dirs, later ones override earlier ones) and MD_SPIRV_OUT: the shaders of the tools checkout plus the private overlay of main (migration,
# coordinator 6805094958). The sources are merged into a temporary stage as shaders/, so every shaders/<name> below keeps its meaning; the SPIR-V goes to MD_SPIRV_OUT.
# Without MD_SHADER_SRC: the old behaviour (cwd = the repo root, shaders/ and shaders/spirv).
if [ -n "${MD_SHADER_SRC:-}" ]; then
    : "${MD_SPIRV_OUT:?MD_SPIRV_OUT is required with MD_SHADER_SRC}"
    mkdir -p "$MD_SPIRV_OUT"; MD_SPIRV_OUT=$(cd "$MD_SPIRV_OUT" && pwd)
    STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
    mkdir -p "$STAGE/shaders"
    IFS=: read -ra _SRC <<< "$MD_SHADER_SRC"
    for d in "${_SRC[@]}"; do [ -d "$d" ] && cp -a "$d"/. "$STAGE/shaders/"; done
    rm -rf "$STAGE/shaders/spirv"
    cd "$STAGE"
    SPIRV_DIR="$MD_SPIRV_OUT"
else
    cd "$(dirname "$0")/.."
    SPIRV_DIR="shaders/spirv"
fi
FILTER="${1:-}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; GRAY='\033[0;37m'; NC='\033[0m'

ok=0; fail=0; skip=0; compat=0
# the translated shaders that stay in the private repo (migration): files that include one of them are skipped when it is absent
MD_HELD_SHADERS="sky_kenshi_common.glsl sky_pano.frag terrain_kenshi_blend.glsl terrain_shading_common.glsl terrain_shading_screenspace.frag kenshi_atmosphere_fog.glsl kenshi_main_light.glsl kenshi_rtt_light.glsl water_kenshi.frag prop_tri.frag char_body.frag char_platform.frag npc_instanced.frag kenshi_lum_adapt.comp ground_composite_exp.frag"

mkdir -p "$SPIRV_DIR"

compile_vulkan() {
    local src="$1"
    local dst="$2"
    if [ ! -f "$src" ]; then printf "  ${GRAY}[MISSING]${NC} %-42s (source not in this checkout: its feature is off or falls back)\n" "$src"; ((skip++)) || true; return; fi
    local extra="${3:-}"   # optional extra glslc flags (e.g. -DTS_KENSHI_FULL=1)
    if glslc -DSPIRV_COMPILE=1 $extra --target-env=vulkan1.1 -O "$src" -o "$dst" 2>/tmp/_md_glslc_err; then
        printf "  ${GREEN}[VULKAN]${NC} %-42s → %s\n" "$src" "$(basename $dst)"
        ((ok++)) || true
    else
        # an include of a translated (held) shader that this checkout does not have (tools without main's overlay): the dependent shader is skipped, its feature is off
        local inc; inc=$(sed -n 's/.*Cannot find or open include file\. for header name: *//p' /tmp/_md_glslc_err | head -1)
        if [ -n "$inc" ] && [[ " $MD_HELD_SHADERS " == *" $inc "* ]]; then
            printf "  ${GRAY}[MISSING]${NC} %-42s (includes %s, which only the private overlay has)\n" "$src" "$inc"; ((skip++)) || true; return
        fi
        printf "  ${RED}[FAIL  ]${NC} %-42s\n" "$src"
        sed 's/^/           /' /tmp/_md_glslc_err
        ((fail++)) || true
    fi
}

compile_compat() {
    local src="$1"
    local dst="$2"
    if [ ! -f "$src" ]; then ((skip++)) || true; return; fi
    if glslangValidator -G "$src" -o "$dst" >/tmp/_md_glslv_err 2>&1; then
        printf "  ${YELLOW}[COMPAT ]${NC} %-42s → %s  (UBO migration pending)\n" \
               "$src" "$(basename $dst)"
        ((compat++)) || true
    else
        printf "  ${RED}[FAIL  ]${NC} %-42s\n" "$src"
        cat /tmp/_md_glslv_err | sed 's/^/           /'
        ((fail++)) || true
    fi
}

should_compile() {
    [[ -z "$FILTER" ]] || [[ "$1" == *"$FILTER"* ]]
}

echo ""
echo "╔═══════════════════════════════════════════════════════╗"
echo "║         monkey_dust SPIR-V Shader Compilation         ║"
echo "╚═══════════════════════════════════════════════════════╝"
echo ""
echo "── Animation / NPC ───────────────────────────────────────"
for s in shaders/animated.vert shaders/animated_prepass.vert \
         shaders/animated_dissolve.frag shaders/animated_emissive.frag \
         shaders/npc_instanced.vert shaders/npc_instanced.frag; do
    should_compile "$s" && compile_vulkan "$s" "$SPIRV_DIR/$(basename $s).spv"
done

echo ""
echo "── Particle demo ───────────────────────────────────────────"
for s in shaders/particle.vert shaders/particle.frag; do
    should_compile "$s" && compile_vulkan "$s" "$SPIRV_DIR/$(basename $s).spv"
done

echo ""
echo "── Shadow CSM ────────────────────────────────────────"
for s in shaders/shadow_csm.vert shaders/shadow_csm.frag; do
    should_compile "$s" && compile_vulkan "$s" "$SPIRV_DIR/$(basename $s).spv"
done

echo ""
echo "── Character Preview / Hair / Scene / PBR ───────────────────"
for s in shaders/char_bg.vert shaders/char_bg.frag \
         shaders/char_hair.vert \
         shaders/char_hair_game.vert shaders/char_hair_game.frag \
         shaders/char_clothes_game.vert shaders/char_clothes.frag \
         shaders/char_preview.vert shaders/char_body.frag shaders/char_hair.frag \
         shaders/char_scene.vert shaders/char_scene.frag \
         shaders/char_platform.vert shaders/char_platform.frag; do
    should_compile "$s" && compile_vulkan "$s" "$SPIRV_DIR/$(basename $s).spv"
done

echo ""
echo "── Terrain (GEOCLIPMAP geometry + Variant A screen-space shading) ─"
for s in shaders/terrain_gbuffer_mini.frag \
         shaders/terrain_shading_screenspace.vert \
         shaders/terrain_worldmap_normal_bake.comp \
         shaders/terrain_lod_normals.comp \
         shaders/terrain_quadtree.vert \
         shaders/terrain_quadtree_batched.vert \
         shaders/terrain_quadtree_batched_lod.vert \
         shaders/terrain_quadtree_boundary.vert \
         shaders/terrain_wireframe.frag \
         shaders/terrain_skyvis_probe_update.comp \
         shaders/terrain_skyvis_apply.frag; do
    should_compile "$s" && compile_vulkan "$s" "$SPIRV_DIR/$(basename $s).spv"
done
# task #227: Kenshi-parity resolve variant (same source, -DTS_KENSHI_FULL=1); the only ground resolve variant.
should_compile shaders/terrain_gbuffer_mini.frag && compile_vulkan shaders/terrain_gbuffer_mini.frag "$SPIRV_DIR/terrain_gbuffer_mini_slot.frag.spv" "-DTS_SLOT=1"   # ground live pass: second target = the patch's biome-list slot
should_compile shaders/terrain_gbuffer_mini.frag && compile_vulkan shaders/terrain_gbuffer_mini.frag "$SPIRV_DIR/terrain_gbuffer_mini_slot0.frag.spv" "-DTS_SLOT0=1"   # ground live pass: second target = 0 (geometry without a patch list)
should_compile shaders/feature_gbuf.vert && compile_vulkan shaders/feature_gbuf.vert "$SPIRV_DIR/feature_gbuf_slot.vert.spv" "-DFEATURE_SLOT=1"   # ground live pass: a TERRAIN-mode feature writes the slot of the list at its own position (coordinator #207 6861963075)
should_compile shaders/terrain_quadtree_batched_lod.vert && compile_vulkan shaders/terrain_quadtree_batched_lod.vert "$SPIRV_DIR/terrain_quadtree_batched_lod_slot.vert.spv" "-DTS_SLOT=1"
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1"
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_noskip.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_SKIP_FLAT_ON_CLIFF=0"
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_klight.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_KLIGHT=1"
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_reuse_klight.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_REUSE=1 -DTS_KLIGHT=1"
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_reuse_only_klight.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_REUSE=1 -DTS_KLIGHT=1 -DTS_REUSE_ONLY=1"
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_reuse_only_bil_klight.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_REUSE=1 -DTS_KLIGHT=1 -DTS_REUSE_ONLY=1 -DTS_PA=64"   # #207 Settings «History filter while moving» = Fast: one bilinear history tap in pass A   # #207 stencil split, pass A
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_onebiome_klight.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_KLIGHT=1 -DTS_ONE_BIOME_EXACT=1"   # #207 stencil split, pass for the one-biome pixels
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_klight_live.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_KLIGHT=1 -DTS_LIVE=1"
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_klight_live_tiny.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_KLIGHT=1 -DTS_LIVE=1 -DTS_LIVE_TINY=1"   # MD_GROUND_LIVE_TINY: lit live pixels only   # MD_GROUND_LIVE_LIGHT: live ground material consumer
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_onebiome_klight_live.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_KLIGHT=1 -DTS_ONE_BIOME_EXACT=1 -DTS_LIVE=1"
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_reuse.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_REUSE=1"
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_pvt_klight.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_KLIGHT=1 -DTS_PVT=1"   # #207 PVT stage 1, resolve pass
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_pvt_mark_klight.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_KLIGHT=1 -DTS_PVT=1 -DTS_PVT_MARK=1"   # #207 PVT marker pass (measurement)
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_ubiome_klight.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_KLIGHT=1 -DTS_UNIFORM_BIOME=1"   # #207 H1 measurement: biome parameters per draw (uniform block), material + klight
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_ubiome_mat.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_KLIGHT=1 -DTS_UNIFORM_BIOME=1 -DTS_MATONLY=1"   # #207 H1 measurement: biome parameters per draw, material only (2 colour outputs)
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_pvt_a_klight.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_REUSE=1 -DTS_KLIGHT=1 -DTS_REUSE_ONLY=1 -DTS_PVT=1"
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_pvt_a_bil_klight.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_REUSE=1 -DTS_KLIGHT=1 -DTS_REUSE_ONLY=1 -DTS_PVT=1 -DTS_PA=64"   # the same with the PVT lookup   # #207 PVT in pass A
should_compile shaders/terrain_shading_screenspace.frag && compile_vulkan shaders/terrain_shading_screenspace.frag "$SPIRV_DIR/terrain_shading_screenspace_kenshi_pvt_tile.frag.spv" "-DTS_KENSHI_FULL=1 -DTS_BIOME_SSBO=1 -DTS_KLIGHT=1 -DTS_PVT_TILE=1"   # #207 PVT stage 1, tile bake

echo ""
echo "── Props / Vegetation / World ────────────────────────────────"
for s in shaders/prop.vert shaders/prop.frag shaders/prop_tri.frag shaders/prop_cut.frag shaders/feature_gbuf.vert shaders/hub_inst.vert shaders/hzb_down.frag shaders/hub_flat.frag shaders/hub_alpha.frag \
         shaders/world_pin.vert shaders/world_pin.frag \
         shaders/overlay.vert shaders/overlay.frag \
         shaders/mygui.vert shaders/mygui.frag; do
    should_compile "$s" && compile_vulkan "$s" "$SPIRV_DIR/$(basename $s).spv"
done

echo ""
echo ""
echo "── CAS / World3D / NPC Cull (previously missing from this script) ─"
for s in shaders/cas.vert shaders/cas.frag shaders/upscale.vert shaders/upscale.frag shaders/upscale_light.frag shaders/sgsr1.frag shaders/fxaa.frag shaders/ground_composite.frag shaders/ground_composite_exp.frag shaders/kenshi_lum_adapt.comp \
         shaders/world3d.vert shaders/world3d.frag \
         shaders/npc_cull.comp; do
    should_compile "$s" && compile_vulkan "$s" "$SPIRV_DIR/$(basename $s).spv"
done


echo ""
echo "── Sky / Water ────────────────────────────────────────────────"
for s in shaders/sky.vert shaders/sky.frag shaders/sky_kenshi.frag shaders/sky_pano.frag \
         shaders/water.vert shaders/water.frag shaders/water_kenshi.frag; do
    should_compile "$s" && compile_vulkan "$s" "$SPIRV_DIR/$(basename $s).spv"
done

echo ""
echo "── Skipped (legacy Raylib, not SDL_GPU target) ──────────────"
for s in shaders/fog.glsl; do
    if should_compile "$s"; then
        printf "  ${GRAY}[SKIP  ]${NC} %s\n" "$s"
        ((skip++)) || true
    fi
done

echo ""
echo "═══════════════════════════════════════════════════════════"
printf "  ${GREEN}VULKAN${NC}:  %2d   ${YELLOW}COMPAT${NC}: %2d   ${RED}FAIL${NC}: %2d   ${GRAY}SKIP${NC}: %2d\n" \
       "$ok" "$compat" "$fail" "$skip"
if (( fail > 0 )); then
    echo ""
    printf "  ${RED}✗ ${fail} shader(s) failed — fix before SDL_GPU migration${NC}\n"
    exit 1
else
    echo ""
    printf "  ${GREEN}✓ All shaders compiled successfully${NC}\n"
fi
echo ""
