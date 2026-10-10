---
id: kb-tools-readme
type: reference
status: active
date: 2026-05-14
updated: 2026-10-10
repo: tools
tags: [tools, readme, editor, kenshi, measurement, public-repo]
summary: "Public tools README (English; Ukrainian in README.uk.md): editor, measurement/QA drivers, offline Kenshi-data converters and bakers, legacy Flare tools; build; licence"
---

# monkey_dust Tools

**Editor, measurement drivers and offline converters for [monkey_dust](https://github.com/rdga1afk/mnkey_dst_engine), a native Linux engine that renders Kenshi's world from your own install.**

**English** · [Українською](README.uk.md) · [Engine](https://github.com/rdga1afk/mnkey_dst_engine) · [Docs site](https://rdga1afk.github.io/mnkey_dst_engine/)

---

> ### You need Kenshi
> Tools that touch game data read them from **your own Kenshi install** and write results outside this repository. This repository contains **no Kenshi data**. Kenshi content itself is edited with Kenshi's own **Forgotten Construction Set (FCS)**, not with these tools.

## What is here

| Group | Tool | Purpose |
|---|---|---|
| **Editor** | `monkey_dust_editor` (`editor/`) | Standalone SDL_GPU editor (Dear ImGui): 3D world viewport (view-only), hierarchy, console + Lua REPL, graphics, camera, animation, AI inspectors, GPU profiler |
| | `shader_watch.sh` | Recompile shaders on save; `/reload-shaders` in the editor console reloads pipelines without restart |
| **Measurement & QA** | `qa/game_cmd_driver.py` | Drives a running game: camera pose, screenshots, Lua commands. Basis of all performance measurements |
| | `qa/resolve_pass_measure.py`, `qa/perf_gate.py`, `qa/terrain_zone_sweep.py` | Block-sampled (DVFS-safe) GPU timing, performance gates, zone sweeps |
| | `qa/char_preview_qa.sh`, `qa/editor_screenshot_compare.py`, `qa/qa_run.sh` | Visual regression (RMSE against baselines), editor regression, QA runs |
| **Kenshi data, offline** | `tif_to_r32.py` | `fullmap.tif` → `world_hmap.r16` (now only a fallback: the engine reads `fullmap.tif` directly) |
| | `md_bake_ground_layers.py`, `md_bake_detail_array.py`, `md_encode_ground_bake.py` | Bake ground-layer colour and the detail texture array (to become a first-run cache) |
| | `md_stitch_overlay_mask.py`, `md_stitch_grasssplits.py`, `md_stitch_terrain.py` | Stitch Kenshi's tiled overlay / grass-density / terrain tiles into single textures |
| | `md_convert.py`, `md_convert_static.py`, `md_convert_features.py`, `md_mesh_conv/`, `kenshi_import/` | Ogre mesh/skeleton XML → GLB (characters, static props, `features.dat` objects). Will be replaced by runtime Ogre `.mesh` readers in the engine |
| **Repo hygiene** | `md_doctor.py`, `md_dash.py`, `spirv_reflect_check.py` | Docs-vs-code drift checks, offline status dashboard, SPIR-V binding checks |
| **Legacy (Flare)** | `md_flare_convert`, `md_flare_demo`, `flare_2d_render.py` | From the project's earlier isometric Flare direction; kept working, not developed |

## Build
`tools/` is built from the game repository root (it needs the engine as a sibling submodule):
```bash
cmake -S .. -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_SDL3=ON
ninja -C build monkey_dust_editor
ninja -C build md_flare_convert md_flare_demo
```
Python tools need Python 3, numpy and Pillow.

## Measuring the game
```bash
export KENSHI_DIR="$HOME/.local/share/Steam/steamapps/common/Kenshi"
python3 tools/qa/game_cmd_driver.py --screenshot /tmp/out.png
```
- Place the camera with `md.set_camera_pose_world(...)` and check `md.terrain_pixel_fraction()` ≥ 0.8: performance is measured only on terrain-dominated frames.
- Compare A/B in one process, interleaved blocks (ABBA).
- Pitfall: the CLI `--camera` flag (and `Driver.screenshot(camera=..., editor_open=False)`) loses the camera: `editor_open=False` switches back to the player-orbit camera. Use `editor_open=True` or a pose command.

## Editor notes
- Panels: Hierarchy, Assets, Console (Lua REPL), Graphics, Camera (orbit / fly), Animation, ViewCone inspector, FlowGraph, Director, GPU Profiler, Sequencer, 3D World, Settings. Command palette: `Ctrl+P`.
- The 3D World viewport loads biomes from FCS (`BiomeRegistry::LoadPreferFcs`) and `blendmap.png` from the Kenshi install, with offline fallbacks; terrain loads on a background thread.
- Game-coupled panels (Inspector, Terrain sculpt) live in the game repository and are compiled into the game with `-DMONKEY_DUST_EDITOR=ON`.
- GPU debugging: `SDL_GPU_DEBUGMODE=1 renderdoc build/tools/monkey_dust_editor`.

## Architecture
```
tools/
  editor/          standalone editor (depends on engine/ + tools/third_party/ only)
  qa/              game driver, timing, regression
  kenshi_import/   Ogre → GLB pipeline for Kenshi assets (offline, reads your install)
  md_*.py          converters, bakers, stitchers
  flare_convert/, flare_demo/   legacy Flare tools
  third_party/     Dear ImGui + extensions (imnodes, imgui-node-editor, ImGuiColorTextEdit, ImGuizmo, flame graph, command palette)
```
Dependency rule: `tools/` includes only `engine/` (`<monkey_dust/...>`) and `tools/third_party/`, never `game/`.

## Licence
GNU GPL v3.0, see [LICENSE](LICENSE). Kenshi is a trademark of Lo-Fi Games; this project is not affiliated with Lo-Fi Games.
