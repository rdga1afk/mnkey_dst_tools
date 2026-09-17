# tools/editor/ — CLAUDE.md

## Єдиний бінарник (hot-reload видалено 2026-09-17)
`monkey_dust_editor` — звичайний одинарний виконуваний файл, БЕЗ
`dlopen`/`build/hot/libeditor_panels.so`. Рішення власника: повторний
стан "панелі .so застаріле відносно engine .a" (ninja dependency
tracking для link-кроку shared lib не бачив зміни в .a) спричиняв
довгу плутанину під час діагностики — простіше видалити механізм, ніж
далі латати. `ninja -C build monkey_dust_editor` — єдина ціль,
нічого більше білдити не треба, редактор одразу підхоплює всі зміни.

Фонові треди (`s_loader_thread` тощо) — `.join()`, НІКОЛИ `.detach()`:
`GpuDevice::Shutdown()` може знищити Vulkan-device поки detached-тред
ще вивантажує текстуру (SIGSEGV, підтверджено coredump'ом 2026-07-26) —
цей інваріант лишається чинним і без hot-reload.

## `DrawContent()` invariant
Кожна панель: `Draw()` (Begin/End + visibility guard) і `DrawContent()`
(тільки вміст). F3-таби (`##f3editor`) викликають ТІЛЬКИ `DrawContent()`
— тому lazy init (`seq_.count`, `imnodes_ctx_`) МУСИТЬ бути в
`DrawContent()`, не в `Draw()`.

## ImGui — тільки #ifdef MONKEY_DUST_EDITOR
Ніколи не в реліз-білді гри. Callbacks на кшталт `FlameGetter`
(imgui-flame-graph) передають `nullptr` для деяких параметрів — завжди
null-check перед dereference.

## Editor 3D World viewport
`EDITOR_TNKN=64` (повний 64×64 світ) — ІНША система за `TNKN=9` у грі.
Synthesis VBO (256×256) — завжди фон, LOD chunks поверх через depth test.
`handle_input()` guard МУСИТЬ включати `any_move_key` — інакше WASD
зависає при виході миші за межі viewport.

Глибокий довідник: `docs/CLAUDE_ARCH.md`.
