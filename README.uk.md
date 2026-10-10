---
id: kb-tools-readme-uk
type: reference
status: active
date: 2026-05-14
updated: 2026-10-10
repo: tools
tags: [tools, readme, editor, kenshi, measurement, public-repo]
summary: "Public tools README (Ukrainian translation of README.md): editor, measurement/QA drivers, offline Kenshi-data converters and bakers, legacy Flare tools; build; licence"
---

# monkey_dust Tools (Українською)

[English](README.md) · **Українською** · [Engine](https://github.com/rdga1afk/mnkey_dst_engine) · [Docs site](https://rdga1afk.github.io/mnkey_dst_engine/)

---

> ### Потрібна Kenshi
> Інструменти, що працюють з даними гри, читають їх з **вашої власної інсталяції Kenshi** і пишуть результати поза цим репозиторієм. Тут **немає даних Kenshi**. Сам контент Kenshi редагується її власним **Forgotten Construction Set (FCS)**, а не цими інструментами.

## Що тут є

| Група | Інструмент | Призначення |
|---|---|---|
| **Редактор** | `monkey_dust_editor` (`editor/`) | Окремий редактор на SDL_GPU (Dear ImGui): 3D-вікно світу (лише перегляд), ієрархія, консоль + Lua REPL, графіка, камера, анімація, інспектори AI, профайлер GPU |
| | `shader_watch.sh` | Перекомпіляція шейдерів при збереженні; `/reload-shaders` у консолі перезавантажує конвеєри без перезапуску |
| **Заміри й QA** | `qa/game_cmd_driver.py` | Керує запущеною грою: камера, скриншоти, Lua-команди. Основа всіх замірів продуктивності |
| | `qa/resolve_pass_measure.py`, `qa/perf_gate.py`, `qa/terrain_zone_sweep.py` | Блокові заміри GPU (стійкі до DVFS), пороги продуктивності, обхід зон |
| | `qa/char_preview_qa.sh`, `qa/editor_screenshot_compare.py`, `qa/qa_run.sh` | Візуальна регресія (RMSE з еталонами), регресія редактора, прогони QA |
| **Дані Kenshi, офлайн** | `tif_to_r32.py` | `fullmap.tif` → `world_hmap.r16` (тепер лише запасний варіант: рушій читає `fullmap.tif` напряму) |
| | `md_bake_ground_layers.py`, `md_bake_detail_array.py`, `md_encode_ground_bake.py` | Випікання кольору шарів ґрунту й масиву деталей (стане кешем першого запуску) |
| | `md_stitch_overlay_mask.py`, `md_stitch_grasssplits.py`, `md_stitch_terrain.py` | Зшивання тайлів Kenshi (overlay, густина трави, ландшафт) в одну текстуру |
| | `md_convert.py`, `md_convert_static.py`, `md_convert_features.py`, `md_mesh_conv/`, `kenshi_import/` | Ogre XML → GLB (персонажі, статичні об'єкти, об'єкти `features.dat`). Буде замінено читачами Ogre `.mesh` у рушії під час запуску |
| **Гігієна репо** | `md_doctor.py`, `md_dash.py`, `spirv_reflect_check.py` | Розбіжності документації з кодом, офлайн-панель стану, перевірка прив'язок SPIR-V |
| **Спадок (Flare)** | `md_flare_convert`, `md_flare_demo`, `flare_2d_render.py` | З попереднього ізометричного напряму Flare; працюють, але не розвиваються |

## Збірка
Збирається з кореня репозиторію гри (потрібен рушій як сусідній підмодуль) — команди вище. Python-інструментам потрібні Python 3, numpy і Pillow.

## Заміри
- Камера через `md.set_camera_pose_world(...)`, перевірка `md.terrain_pixel_fraction()` ≥ 0,8: продуктивність міряємо лише на кадрах, де переважає земля.
- A/B — в одному процесі, чергованими блоками (ABBA).
- Пастка: прапорець `--camera` (і `Driver.screenshot(camera=..., editor_open=False)`) губить камеру, бо `editor_open=False` повертає камеру гравця. Використовуйте `editor_open=True` або команду пози.

## Редактор
- Панелі: Hierarchy, Assets, Console (Lua REPL), Graphics, Camera (орбіта / політ), Animation, ViewCone, FlowGraph, Director, GPU Profiler, Sequencer, 3D World, Settings. Палітра команд: `Ctrl+P`.
- 3D World бере біоми з FCS (`BiomeRegistry::LoadPreferFcs`) і `blendmap.png` з інсталяції Kenshi, із запасними офлайн-копіями; ландшафт вантажиться у фоновому потоці.
- Панелі, прив'язані до гри (Inspector, скульптинг ландшафту), — у репозиторії гри, вмикаються `-DMONKEY_DUST_EDITOR=ON`.

## Архітектура
Див. дерево вище. Правило залежностей: `tools/` включає лише `engine/` і `tools/third_party/`, ніколи `game/`.

## Ліцензія
GNU GPL v3.0, див. [LICENSE](LICENSE). Kenshi — торгова марка Lo-Fi Games; проєкт з Lo-Fi Games не пов'язаний.
