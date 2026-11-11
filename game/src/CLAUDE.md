# game/src/ — CLAUDE.md

## Game loop: 10 TPS logic / 60 FPS render
`main.cpp` — event pump раз на кадр (на початку), потім `RenderFrame`.
`logic_tick.cpp`'s `RunLogicTick()` — 36 рядків, делегує в статичні
хелпери (`TickAI`, `TickPhysics`, `TickCombat`, ...). `LOGIC_TICK_S=0.1f`.
`CrowdSystem::Update` → `JoltWorld::Step` → readback, у цьому порядку.

**SDL event pump:** ЄДИНИЙ `SDL_PollEvent` loop на початку кадру, після
`input_begin_frame()`. `window_end_frame()` НЕ дренує чергу — інакше
кліки між `imgui_new_frame` і `window_end_frame` губляться.

## flecs collect→apply (канонічний патерн)
```cpp
static flecs::entity dead[512]; int n = 0;
world.each([&](flecs::entity e, Combat& c) {
    if (c.is_dead && n < 512) dead[n++] = e;
});
for (int i = 0; i < n; ++i) if (dead[i].is_alive()) dead[i].destruct();
```
Без мутації реєстру під час ітерації — collect спочатку, apply окремо.

## Правило M-A (platform isolation)
Тільки `main.cpp` включає `platform/input.h`/`audio.h`/`window.h`.
`#include "platform_input.h"` поза `main.cpp`/`EditorMain.cpp` —
заборонено.

## Без std::vector/malloc у hot-path
Фіксовані масиви `[MAX_N]`. Combat/AI/Nav тіки — нуль alloc.
Path-індексування: `X=path[i*3], Y=path[i*3+1], Z=path[i*3+2]`, НЕ
`path[i]`.

Глибокий довідник: `docs/CLAUDE_ARCH.md`, `docs/CLAUDE_AI.md`.
