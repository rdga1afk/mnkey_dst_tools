---
id: kb-tools-readme
type: reference
status: active
date: 2026-05-14
updated: 2026-10-13
repo: tools
tags: [tools, readme, editor, kenshi, measurement, public-repo]
summary: "Public tools README (EN + UA): editor (incl. Kenshi-style character editor), measurement/QA drivers, read-only MCP servers (Ghidra, RenderDoc), offline Kenshi-data converters; build; licence"
---

# monkey_dust Tools

**Editor, measurement drivers and offline converters for [monkey_dust](https://github.com/rdga1afk/mnkey_dst_engine), a native Linux engine that renders Kenshi's world from your own install.**

[English](#english) · [Українською](#українською) · [Engine](https://github.com/rdga1afk/mnkey_dst_engine) · [Docs site](https://rdga1afk.github.io/mnkey_dst_engine/)

---

## English

> ### You need Kenshi
> Tools that touch game data read them from **your own Kenshi install** and write results outside this repository. This repository contains **no Kenshi data**. Kenshi content itself is edited with Kenshi's own **Forgotten Construction Set (FCS)**, not with these tools.

### What is here

| Group | Tool | Purpose |
|---|---|---|
| **Editor** | `monkey_dust_editor` (`editor/`) | Standalone SDL_GPU editor (Dear ImGui): 3D world viewport (view-only), hierarchy, console + Lua REPL, graphics, camera, animation, AI inspectors, GPU profiler |
| | Characters tab (`editor/character_editor*.h`) | Character creator laid out like Kenshi's: race and gender, BODY / FACE / HAIR sliders, live 3D preview. Being brought to full parity with Kenshi's editor: body sliders scale skeleton bones as Kenshi does; a soft-tissue morph layer only on the bare body. Built here first, then moved into the game |
| | `shader_watch.sh` | Recompile shaders on save; `/reload-shaders` in the editor console reloads pipelines without restart |
| **Measurement & QA** | `qa/game_cmd_driver.py` | Drives a running game: camera pose, screenshots, Lua commands. Basis of all performance measurements |
| | `qa/resolve_pass_measure.py`, `qa/perf_gate.py`, `qa/terrain_zone_sweep.py` | Block-sampled (DVFS-safe) GPU timing, performance gates, zone sweeps |
| | `qa/char_preview_qa.sh`, `qa/editor_screenshot_compare.py`, `qa/qa_run.sh` | Visual regression (RMSE against baselines), editor regression, QA runs |
| **Kenshi data, offline** | `tif_to_r32.py` | `fullmap.tif` → `world_hmap.r16` (now only a fallback: the engine reads `fullmap.tif` directly) |
| | `md_bake_ground_layers.py`, `md_bake_detail_array.py`, `md_encode_ground_bake.py` | Bake ground-layer colour and the detail texture array (to become a first-run cache) |
| | `md_stitch_overlay_mask.py`, `md_stitch_grasssplits.py`, `md_stitch_terrain.py` | Stitch Kenshi's tiled overlay / grass-density / terrain tiles into single textures |
| | `md_convert.py`, `md_convert_static.py`, `md_convert_features.py`, `md_mesh_conv/`, `kenshi_import/` | Ogre mesh/skeleton XML → GLB (characters, static props, `features.dat` objects). Will be replaced by runtime Ogre `.mesh` readers in the engine |
| **Agent tooling (MCP)** | `mcp/ghidra_mcp.py` | Thin stdio MCP server over `pyghidra`: reads facts (function at an address, strings, cross-references) from a **read-only copy** of a local Ghidra project of the player's own `kenshi_x64.exe`. No network code; run under `unshare -rn`. Only facts in our own words go into repositories, never decompiled code |
| | `mcp/renderdoc_mcp.py` | Thin stdio MCP server over RenderDoc's Python module (1.45): open a capture, list draws, pipeline state, shader constants, pixel values, texture export, draw diff. Read-only, no daemon or ports |
| **Repo hygiene** | `md_doctor.py`, `md_dash.py`, `spirv_reflect_check.py` | Docs-vs-code drift checks, offline status dashboard, SPIR-V binding checks |

### Build
`tools/` is built from the game repository root (it needs the engine as a sibling submodule):
```bash
cmake -S .. -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_SDL3=ON
ninja -C build monkey_dust_editor
```
Python tools need Python 3, numpy and Pillow.

### Measuring the game
```bash
export KENSHI_DIR="$HOME/.local/share/Steam/steamapps/common/Kenshi"
python3 tools/qa/game_cmd_driver.py --screenshot /tmp/out.png
```
- Place the camera with `md.set_camera_pose_world(...)` and check `md.terrain_pixel_fraction()` ≥ 0.8: performance is measured only on terrain-dominated frames.
- Compare A/B in one process, interleaved blocks (ABBA).
- Pitfall: the CLI `--camera` flag (and `Driver.screenshot(camera=..., editor_open=False)`) loses the camera: `editor_open=False` switches back to the player-orbit camera. Use `editor_open=True` or a pose command.

### Editor notes
- Tabs (from `editor/main.cpp`, the standalone window): Items, Factions, Map, World, 3D World, NPCs, Characters, Inspector, Settings. F3 editor overlay (`editor/editor_core.cpp`): Scene, AI (Director + ViewCone), Animation, FlowGraph, Debug, Camera, Settings. Command palette: `Ctrl+P`. (Listed from the code, not by running the editor.)
- The 3D World viewport loads biomes from FCS (`BiomeRegistry::LoadPreferFcs`) and `blendmap.png` from the Kenshi install, with offline fallbacks; terrain loads on a background thread.
- Characters: the preview renders the converted Kenshi body with the skeleton scaled from the sliders; `md.char_preview_clothes` toggles clothes for checks. Body sliders are applied as bone scales (Kenshi's own method); clothes follow bone scale but not the soft-tissue morphs.
- Game-coupled panels (Inspector, Terrain sculpt) live in the game repository and are compiled into the game with `-DMONKEY_DUST_EDITOR=ON`.
- GPU debugging: `SDL_GPU_DEBUGMODE=1 renderdoc build/tools/monkey_dust_editor`.

### Architecture
```
tools/
  editor/          standalone editor (depends on engine/ + tools/third_party/ only)
  qa/              game driver, timing, regression
  mcp/             read-only MCP servers for agents (Ghidra, RenderDoc)
  kenshi_import/   Ogre → GLB pipeline for Kenshi assets (offline, reads your install)
  md_*.py          converters, bakers, stitchers
  third_party/     Dear ImGui + extensions (imnodes, imgui-node-editor, ImGuiColorTextEdit, ImGuizmo, flame graph, command palette)
```
Dependency rule: `tools/` includes only `engine/` (`<monkey_dust/...>`) and `tools/third_party/`, never `game/`.

### Licence
GNU GPL v3.0, see [LICENSE](LICENSE). Kenshi is a trademark of Lo-Fi Games; this project is not affiliated with Lo-Fi Games.

---

## Українською

> ### Потрібна Kenshi
> Інструменти, що працюють з даними гри, читають їх з **вашої власної інсталяції Kenshi** і пишуть результати поза цим репозиторієм. Тут **немає даних Kenshi**. Сам контент Kenshi редагується її власним **Forgotten Construction Set (FCS)**, а не цими інструментами.

### Що тут є

| Група | Інструмент | Призначення |
|---|---|---|
| **Редактор** | `monkey_dust_editor` (`editor/`) | Окремий редактор на SDL_GPU (Dear ImGui): 3D-вікно світу (лише перегляд), ієрархія, консоль + Lua REPL, графіка, камера, анімація, інспектори AI, профайлер GPU |
| | Вкладка Characters (`editor/character_editor*.h`) | Редактор персонажів з розкладкою як у Kenshi: раса й стать, слайдери BODY / FACE / HAIR, живий 3D-перегляд. Доводиться до повної відповідності редактору Kenshi: слайдери тіла масштабують кістки скелета, як у Kenshi; шар м'яких тканин (морфи) — лише на голому тілі. Спершу тут, потім переноситься в гру |
| | `shader_watch.sh` | Перекомпіляція шейдерів при збереженні; `/reload-shaders` у консолі перезавантажує конвеєри без перезапуску |
| **Заміри й QA** | `qa/game_cmd_driver.py` | Керує запущеною грою: камера, скриншоти, Lua-команди. Основа всіх замірів продуктивності |
| | `qa/resolve_pass_measure.py`, `qa/perf_gate.py`, `qa/terrain_zone_sweep.py` | Блокові заміри GPU (стійкі до DVFS), пороги продуктивності, обхід зон |
| | `qa/char_preview_qa.sh`, `qa/editor_screenshot_compare.py`, `qa/qa_run.sh` | Візуальна регресія (RMSE з еталонами), регресія редактора, прогони QA |
| **Дані Kenshi, офлайн** | `tif_to_r32.py` | `fullmap.tif` → `world_hmap.r16` (тепер лише запасний варіант: рушій читає `fullmap.tif` напряму) |
| | `md_bake_ground_layers.py`, `md_bake_detail_array.py`, `md_encode_ground_bake.py` | Випікання кольору шарів ґрунту й масиву деталей (стане кешем першого запуску) |
| | `md_stitch_overlay_mask.py`, `md_stitch_grasssplits.py`, `md_stitch_terrain.py` | Зшивання тайлів Kenshi (overlay, густина трави, ландшафт) в одну текстуру |
| | `md_convert.py`, `md_convert_static.py`, `md_convert_features.py`, `md_mesh_conv/`, `kenshi_import/` | Ogre XML → GLB (персонажі, статичні об'єкти, об'єкти `features.dat`). Буде замінено читачами Ogre `.mesh` у рушії під час запуску |
| **Інструменти агента (MCP)** | `mcp/ghidra_mcp.py` | Тонкий stdio-сервер MCP над `pyghidra`: читає факти (функція за адресою, рядки, перехресні посилання) з **копії лише для читання** локального проєкту Ghidra власного `kenshi_x64.exe` гравця. Без мережевого коду; запуск під `unshare -rn`. У репозиторії — лише факти своїми словами, ніколи декомпільований код |
| | `mcp/renderdoc_mcp.py` | Тонкий stdio-сервер MCP над Python-модулем RenderDoc (1.45): відкрити захоплення, список викликів, стан конвеєра, константи шейдерів, значення пікселя, експорт текстури, порівняння викликів. Лише читання, без демона й портів |
| **Гігієна репо** | `md_doctor.py`, `md_dash.py`, `spirv_reflect_check.py` | Розбіжності документації з кодом, офлайн-панель стану, перевірка прив'язок SPIR-V |

### Збірка
Збирається з кореня репозиторію гри (потрібен рушій як сусідній підмодуль) — команди вище. Python-інструментам потрібні Python 3, numpy і Pillow.

### Заміри
- Камера через `md.set_camera_pose_world(...)`, перевірка `md.terrain_pixel_fraction()` ≥ 0,8: продуктивність міряємо лише на кадрах, де переважає земля.
- A/B — в одному процесі, чергованими блоками (ABBA).
- Пастка: прапорець `--camera` (і `Driver.screenshot(camera=..., editor_open=False)`) губить камеру, бо `editor_open=False` повертає камеру гравця. Використовуйте `editor_open=True` або команду пози.

### Редактор
- Вкладки (з `editor/main.cpp`, окреме вікно): Items, Factions, Map, World, 3D World, NPCs, Characters, Inspector, Settings. Оверлей F3 (`editor/editor_core.cpp`): Scene, AI (Director + ViewCone), Animation, FlowGraph, Debug, Camera, Settings. Палітра команд: `Ctrl+P`. (Перелік за кодом, редактор не запускався.)
- 3D World бере біоми з FCS (`BiomeRegistry::LoadPreferFcs`) і `blendmap.png` з інсталяції Kenshi, із запасними офлайн-копіями; ландшафт вантажиться у фоновому потоці.
- Characters: перегляд показує конвертоване тіло Kenshi зі скелетом, масштабованим слайдерами; `md.char_preview_clothes` вмикає й вимикає одяг для перевірок. Слайдери тіла — масштаб кісток (як у самій Kenshi); одяг іде за масштабом кісток, але не за морфами м'яких тканин.
- Панелі, прив'язані до гри (Inspector, скульптинг ландшафту), — у репозиторії гри, вмикаються `-DMONKEY_DUST_EDITOR=ON`.

### Архітектура
Див. дерево вище. Правило залежностей: `tools/` включає лише `engine/` і `tools/third_party/`, ніколи `game/`.

### Ліцензія
GNU GPL v3.0, див. [LICENSE](LICENSE). Kenshi — торгова марка Lo-Fi Games; проєкт з Lo-Fi Games не пов'язаний.
