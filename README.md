---
id: kb-tools-readme
type: reference
status: active
date: 2026-05-14
updated: 2026-11-11
repo: tools
tags: [tools, readme, editor, kenshi, measurement, public-repo]
summary: "Public tools README (EN + UA): editor (incl. Kenshi-style character editor and a Linux-native, FCS-compatible Kenshi data / mod editor), measurement/QA drivers, read-only MCP servers (Ghidra, RenderDoc), offline Kenshi-data converters; build; licence"
---

# monkey_dust Tools

**Editor, measurement drivers and offline converters for [monkey_dust](https://github.com/rdga1afk/mnkey_dst_engine), a native Linux engine that renders Kenshi's world from your own install.**

[English](#english) · [Українською](#українською) · [Engine](https://github.com/rdga1afk/mnkey_dst_engine) · [Docs site](https://rdga1afk.github.io/mnkey_dst_engine/)

---

## English

> ### You need Kenshi
> Tools that touch game data read them from **your own Kenshi install** and write results outside this repository. This repository contains **no Kenshi data**. Kenshi content itself is edited with Kenshi's own **Forgotten Construction Set (FCS)**, not with these tools.

### The game's source is here (since 11.11.2026)
The game itself moved into this repository (MIT): `game/` (source, CMake, our own small data), `shaders/` (our GLSL), `tests/` (unit tests), `scripts/` (shader compile and embedding). Kenshi's own shader programs are not here: the game compiles them at run time from your install. A few shaders translated from Kenshi stay in a private repository until they are replaced by the originals; a game built from this repository alone starts and loads the world from your install, and logs one line for each feature that is off for that reason.
**This repository builds on its own** (the game, the editor and the unit tests; see Build below). Running the game needs a Kenshi install (`KENSHI_DIR`); the features that need the few private translated shaders stay off and each logs one `[GpuPipeline] missing shader` line. Status, measurements and goals: [engine README](https://github.com/rdga1afk/mnkey_dst_engine#readme) and the [docs site](https://rdga1afk.github.io/mnkey_dst_engine/).

### What is here

| Group | Tool | Purpose |
|---|---|---|
| **Editor** | `monkey_dust_editor` (`editor/`) | Standalone SDL_GPU editor (Dear ImGui): 3D world viewport (view-only), hierarchy, console + Lua REPL, graphics, camera, animation, GPU profiler |
| | Characters tab (`editor/character_editor*.h`) | Character creator laid out like Kenshi's: race and gender, BODY / FACE / HAIR sliders, live 3D preview. Being brought to full parity with Kenshi's editor: body sliders scale skeleton bones as Kenshi does; a soft-tissue morph layer only on the bare body. Built here first, then moved into the game |
| | **Kenshi data** tab (`editor/data/`) | **A Linux-native data and mod editor compatible with Kenshi's Forgotten Construction Set (FCS)** (in progress; FCS-style layout since 24.10: main toolbar in FCS order, Game World tree and item list, Item / Info / Change List / Find / Settings / Assets / Errors windows, light WinForms-like theme). Reads the field definitions (`fcs*.def`) from your Kenshi install at startup — no schema is bundled — and the game data plus mods in Kenshi's load order. Browse by category, edit every field type, references and instances, create / duplicate / delete items, undo / redo, dialogue editor (lines, conditions, effects, triggers), search and replace, validation before save, merge, clean-up, translation export. Saves `.mod` files in Kenshi's format (v17); the writer re-creates Kenshi's own files byte for byte. Never writes into the Kenshi folder. About 29 of 41 FCS features so far (all the essential ones). Not yet checked by opening our `.mod` in the original FCS |
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
Standalone (the engine is the submodule `engine/`; needs SDL3, Lua 5.4, GoogleTest, Vulkan headers and loader, shaderc / glslang, clang, CMake ≥ 3.21 and Ninja; glslang and SPIRV-Tools are fetched at the pinned release):
```bash
git clone --recurse-submodules https://github.com/rdga1afk/mnkey_dst_tools.git && cd mnkey_dst_tools
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build                       # game (build/game/monkey_dust), editor (build/tools/monkey_dust_editor), unit tests
ctest --test-dir build -L fast -LE needs-install    # unit tests that need no Kenshi install
```
Running the game or the editor needs a Kenshi install (`KENSHI_DIR`). The public CI (`.github/workflows/ci.yml`) builds the game, the editor and the tests from scratch on every push, in an Arch Linux container with packages pinned to a fixed date, and runs the tests that need no Kenshi install. As a part of the private game repository, `tools/` is still built from that repository's root (it adds this directory as a subproject):
```bash
cmake -S .. -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build monkey_dust_editor
```
Python tools need Python 3, numpy and Pillow.

**Prebuilt (since 2026-10-19):** CI (`.github/workflows/package.yml`) builds the editor, `kenshi_navmesh_extract` and the editor/character libraries against the engine release pinned in `ci/ENGINE_SHA`, in an Arch Linux container matched to the test laptop, and publishes them as GitHub Release `build-<sha12>`. The game repository fetches them with `scripts/fetch_prebuilt.sh tools`.

### Measuring the game
```bash
export KENSHI_DIR="$HOME/.local/share/Steam/steamapps/common/Kenshi"
python3 tools/qa/game_cmd_driver.py --screenshot /tmp/out.png
```
- Place the camera with `md.set_camera_pose_world(...)` and check `md.terrain_pixel_fraction()` ≥ 0.8: performance is measured only on terrain-dominated frames.
- Compare A/B in one process, interleaved blocks (ABBA).
- Pitfall: the CLI `--camera` flag (and `Driver.screenshot(camera=..., editor_open=False)`) loses the camera: `editor_open=False` switches back to the player-orbit camera. Use `editor_open=True` or a pose command.

### Editor notes
- Tabs (from `editor/main.cpp`, the standalone window): World, 3D World, Characters, Inspector, Settings. F3 editor overlay (`editor/editor_core.cpp`): Scene, Animation, FlowGraph, Debug, Camera, Settings. The old prototype-game tabs and tools (Items, Factions, NPCs, AI, Map, Flare) were removed on 2026-10-17. Command palette: `Ctrl+P`. (Listed from the code, not by running the editor.)
- The 3D World viewport loads biomes from FCS (`BiomeRegistry::LoadPreferFcs`) and `blendmap.png` from the Kenshi install, with offline fallbacks; terrain loads on a background thread.
- Characters: the tab is the Kenshi character editor (layout `Kenshi_CharacterEditor.layout`): races, sliders and buttons come from the install (`md::character::Session`); the preview renders the race body read from `KENSHI_DIR` (Ogre mesh + skeleton), body sliders as bone sizes, face sliders as mesh poses. Scenarios: `md.char_editor_select`, `md.char_editor_set`, `md.char_editor_view`.
- Character editor, 01.11: behaviour follows Kenshi's own code (face-tab camera frames the head bone; RANDOMISE per tab and RANDOMISE ALL also pick a random head, the body tab an idle stance; slider values clamped to Kenshi's ranges). Real-click tests at 1366×768 and 1920×1080: `tests/scenarios/char_real_input.lua`. Settings → Upscaling: Ground quality, Resolution while moving, Sharpening while moving (Full / Light / Off); FSR 1 / 2 and the fixed low-resolution modes were removed.
- Game-coupled panels (Inspector) live in the game repository and are compiled into the game with `-DMONKEY_DUST_EDITOR=ON`.
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

### Releases
Early Linux builds of the monkey_dust game will be published in this repository's [Releases](https://github.com/rdga1afk/mnkey_dst_tools/releases) (planned, date not set yet). The builds contain no Kenshi files: they read your own Kenshi install at startup.

### Licence
MIT, see [LICENSE](LICENSE) (until 08.11.2026 the repository was GPL-3.0; copies taken before then keep that licence). `third_party/imgui_test_engine` has its own licence (Dear ImGui Test Engine License), is used only for automated tests and is not part of release builds. Kenshi is a trademark of Lo-Fi Games; this project is not affiliated with Lo-Fi Games.

---

## Українською

> ### Потрібна Kenshi
> Інструменти, що працюють з даними гри, читають їх з **вашої власної інсталяції Kenshi** і пишуть результати поза цим репозиторієм. Тут **немає даних Kenshi**. Сам контент Kenshi редагується її власним **Forgotten Construction Set (FCS)**, а не цими інструментами.

### Код гри тепер тут (з 11.11.2026)
Сама гра переїхала в цей репозиторій (MIT): `game/` (код, CMake, наші невеликі дані), `shaders/` (наш GLSL), `tests/` (модульні тести), `scripts/` (компіляція й вбудовування шейдерів). Власних шейдерних програм Kenshi тут немає: гра компілює їх під час запуску з вашої інсталяції. Кілька шейдерів, перекладених з Kenshi, лишаються в приватному репозиторії, доки їх не замінять оригінали; гра, зібрана лише з цього репозиторію, запускається й завантажує світ з вашої інсталяції, а для кожної функції, вимкненої з цієї причини, пише один рядок у журнал.
**Цей репозиторій збирається самостійно** (гра, редактор і модульні тести; див. «Збірка»). Для запуску гри потрібна інсталяція Kenshi (`KENSHI_DIR`); можливості, що потребують кількох приватних перекладених шейдерів, вимкнені, і кожна пише один рядок `[GpuPipeline] missing shader`. Стан, заміри й цілі — у [README рушія](https://github.com/rdga1afk/mnkey_dst_engine#readme) і на [сайті документації](https://rdga1afk.github.io/mnkey_dst_engine/).

### Що тут є

| Група | Інструмент | Призначення |
|---|---|---|
| **Редактор** | `monkey_dust_editor` (`editor/`) | Окремий редактор на SDL_GPU (Dear ImGui): 3D-вікно світу (лише перегляд), ієрархія, консоль + Lua REPL, графіка, камера, анімація, профайлер GPU |
| | Вкладка Characters (`editor/character_editor*.h`) | Редактор персонажів з розкладкою як у Kenshi: раса й стать, слайдери BODY / FACE / HAIR, живий 3D-перегляд. Доводиться до повної відповідності редактору Kenshi: слайдери тіла масштабують кістки скелета, як у Kenshi; шар м'яких тканин (морфи) — лише на голому тілі. Спершу тут, потім переноситься в гру |
| | Вкладка **Kenshi data** (`editor/data/`) | **Нативний для Linux редактор даних і модів, сумісний з Forgotten Construction Set (FCS) Kenshi** (у роботі; з 24.10 — розкладка як у FCS: головна панель інструментів у порядку FCS, дерево Game World і список предметів, вікна Item / Info / Change List / Find / Settings / Assets / Errors, світла тема як у WinForms). Під час запуску читає описи полів (`fcs*.def`) з вашої інсталяції Kenshi — схема не вбудована — і дані гри з модами в порядку Kenshi. Перегляд за категоріями, редагування всіх типів полів, посилань і екземплярів, створення / копіювання / видалення предметів, скасування / повтор, редактор діалогів (репліки, умови, ефекти, тригери), пошук і заміна, перевірка перед збереженням, злиття, очищення, експорт для перекладу. Зберігає `.mod` у форматі Kenshi (v17); запис відтворює власні файли Kenshi байт у байт. Ніколи не пише в теку Kenshi. Поки ≈ 29 з 41 функції FCS (усі основні). Відкриття нашого `.mod` в оригінальному FCS ще не перевірено |
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
Самостійно: `git clone --recurse-submodules …`, `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`, `ctest --test-dir build -L fast -LE needs-install` (потрібні SDL3, Lua 5.4, GoogleTest, заголовки й завантажувач Vulkan, shaderc / glslang, clang, CMake ≥ 3.21, Ninja; рушій — підмодуль `engine/`; glslang і SPIRV-Tools завантажуються закріпленим випуском). Для запуску гри чи редактора потрібна інсталяція Kenshi (`KENSHI_DIR`). Відкритий CI (`.github/workflows/ci.yml`) на кожен пуш збирає гру, редактор і тести з нуля в контейнері Arch Linux з пакетами на закріплену дату і запускає тести, яким не потрібна інсталяція Kenshi. Як частина приватного репозиторію гри збирається з його кореня — команди вище. Python-інструментам потрібні Python 3, numpy і Pillow.

**Готові збірки (з 19.10.2026):** CI (`.github/workflows/package.yml`) збирає редактор, `kenshi_navmesh_extract` і бібліотеки редактора / персонажів проти релізу рушія, закріпленого в `ci/ENGINE_SHA`, у контейнері Arch Linux з версіями тестового ноутбука, і публікує їх як GitHub Release `build-<sha12>`. Репозиторій гри завантажує їх командою `scripts/fetch_prebuilt.sh tools`.

### Заміри
- Камера через `md.set_camera_pose_world(...)`, перевірка `md.terrain_pixel_fraction()` ≥ 0,8: продуктивність міряємо лише на кадрах, де переважає земля.
- A/B — в одному процесі, чергованими блоками (ABBA).
- Пастка: прапорець `--camera` (і `Driver.screenshot(camera=..., editor_open=False)`) губить камеру, бо `editor_open=False` повертає камеру гравця. Використовуйте `editor_open=True` або команду пози.

### Редактор
- Вкладки (з `editor/main.cpp`, окреме вікно): World, 3D World, Characters, Inspector, Settings. Оверлей F3 (`editor/editor_core.cpp`): Scene, Animation, FlowGraph, Debug, Camera, Settings. Вкладки й інструменти старого прототипу гри (Items, Factions, NPCs, AI, Map, Flare) видалено 17.10.2026. Палітра команд: `Ctrl+P`. (Перелік за кодом, редактор не запускався.)
- 3D World бере біоми з FCS (`BiomeRegistry::LoadPreferFcs`) і `blendmap.png` з інсталяції Kenshi, із запасними офлайн-копіями; ландшафт вантажиться у фоновому потоці.
- Characters: вкладка — редактор персонажів Kenshi (розкладка `Kenshi_CharacterEditor.layout`): раси, слайдери й кнопки з інсталяції (`md::character::Session`); перегляд малює тіло раси з `KENSHI_DIR` (меш і скелет Ogre), слайдери тіла — розміри кісток, обличчя — пози меша. Сценарії: `md.char_editor_select`, `md.char_editor_set`, `md.char_editor_view`.
- Редактор персонажа, 01.11: поведінка за кодом самої Kenshi (камера вкладки обличчя кадрує кістку голови; RANDOMISE для вкладки й RANDOMISE ALL також вибирають випадкову голову, вкладка тіла — стійку; значення повзунків обмежено діапазонами Kenshi). Тести справжніми кліками при 1366×768 і 1920×1080: `tests/scenarios/char_real_input.lua`. Settings → Upscaling: Ground quality, Resolution while moving, Sharpening while moving (Full / Light / Off); FSR 1 / 2 і постійні режими зниженої роздільності прибрано.
- Панелі, прив'язані до гри (Inspector), — у репозиторії гри, вмикаються `-DMONKEY_DUST_EDITOR=ON`.

### Архітектура
Див. дерево вище. Правило залежностей: `tools/` включає лише `engine/` і `tools/third_party/`, ніколи `game/`.

### Релізи
Ранні збірки гри monkey_dust для Linux публікуватимуться в [релізах](https://github.com/rdga1afk/mnkey_dst_tools/releases) цього репозиторію (заплановано, дату ще не визначено). Збірки не містять файлів Kenshi: вони читають вашу власну інсталяцію під час запуску.

### Ліцензія
MIT, див. [LICENSE](LICENSE) (до 08.11.2026 репозиторій був під GPL-3.0; копії, взяті раніше, лишаються під нею). `third_party/imgui_test_engine` має власну ліцензію (Dear ImGui Test Engine License), використовується лише для автоматичних тестів і не входить у релізні збірки. Kenshi — торгова марка Lo-Fi Games; проєкт з Lo-Fi Games не пов'язаний.
