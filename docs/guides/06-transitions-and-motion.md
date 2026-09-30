# Transitions & Declarative Motion (`cel_transition`)

In traditional game engines and UI frameworks, animating a value requires creating tween objects, managing timeline controllers, tracking elapsed delta times, and handling keyframes. This tightly couples animation logic to your gameplay or application state.

CELS takes a fundamentally different, declarative approach: **Continuous Mathematical Convergence via `cel_transition`**. You keep your authoritative state instantaneous and pure; you simply declare how the visual projection smoothly converges toward that state over time.

---

## 1. Authoritative State vs. Visual Smoothing

In CELS, gameplay rules and logic operate on **Authoritative State**. Visual presentation layers operate on **Eased Transitions**:

```
┌────────────────────────────────────────────────────────┐
│  AUTHORITATIVE GAME STATE                              │
│  player->health drops instantaneously: 100.0 ──► 40.0   │
└───────────────────────────┬────────────────────────────┘
                            │ target value
┌───────────────────────────▼────────────────────────────┐
│  VISUAL PROJECTION (cel_transition)                    │
│  visualHealth smoothly interpolates:                   │
│  100.0 ──[CEL_EASE_OUT_QUAD over 400ms]──► 40.0        │
└────────────────────────────────────────────────────────┘
```

When a player takes damage, their authoritative health changes immediately. The HUD composable uses `cel_transition` to ease the visual progress bar toward the authoritative number.

---

## 2. Basic Usage (`cel_transition`)

Include `cels.h` and call `cel_transition` inside any active composable:

```c
#include "cels.h"
#include <stdio.h>

CEL_Composable(HealthBar, const PlayerState*, player) {
    cel_watch(player);
    if (!player) return;

    /* Smoothly eases toward player->health over 400ms using Quad Out easing */
    float visualHealth = cel_transition(player->health, 400, CEL_EASE_OUT_QUAD);

    printf("  [HUD] Visual: %.1f / Auth: %.1f\n", visualHealth, player->health);
}
```

### Signature
```c
float cel_transition(float target, uint32_t durationMs, [CelsEasingFn easing]);
```
- **`target`**: The destination value (e.g. coordinates, alpha, health, volume).
- **`durationMs`**: Time in milliseconds to reach the target.
- **`easing`** *(optional)*: Easing curve function pointer (defaults to `CEL_EASE_OUT_QUAD` if omitted).
- **Return Value**: Current interpolated scalar for this frame.

---

## 3. Retargeting Mid-Flight (Zero Visual Pops)

What happens if the target value changes while an animation is already halfway through?

In naive tween systems, retargeting either causes an abrupt visual jump ("teleport") or starts a new tween from the old baseline.

`cel_transition` handles this seamlessly:
```
t = 0ms:   Health is 100. Target changes to 40. Easing begins.
t = 200ms: Visual reaches 75. Player gets hit AGAIN! Target drops to 10.
           ──► cel_transition catches current position (75.0) as the new start!
           ──► Easing continues smoothly from 75.0 toward 10.0 without any visual pop!
```

---

## 4. Zero Allocations & 0% CPU Idle

`cel_transition` is engineered for extreme performance:
1. **Zero Heap Allocations**: The transition state (`CelsTransitionState`) is pinned directly in the composable's slot table (`cel_remember`). No `malloc` or `free` calls ever occur.
2. **Automatic Frame Invalidation**: While converging, `cel_transition` marks the composable group as dirty, ensuring continuous smooth frames.
3. **0% CPU Idle When Settled**: As soon as the current value reaches the target within numerical epsilon, CELS marks the transition as **settled**. It stops requesting recompositions, dropping CPU usage to zero until state changes again!

---

## 5. Built-In Easing Curves

CELS includes a comprehensive suite of hardware-friendly easing curves:

| Easing Macro | Behavior | Recommended Use Case |
| :--- | :--- | :--- |
| `CEL_EASE_LINEAR` | Constant velocity ($f(t) = t$) | Timers, continuous progress bars |
| `CEL_EASE_IN_QUAD` | Accelerates from zero | Gravity drops, elements falling offscreen |
| `CEL_EASE_OUT_QUAD` | Decelerates to zero | Default UI transitions, camera snapping, HUD |
| `CEL_EASE_IN_OUT_QUAD` | Smooth acceleration & deceleration | Moving dialogue boxes, modal panels |
| `CEL_EASE_IN_CUBIC` | Stronger acceleration | Fast exits |
| `CEL_EASE_OUT_CUBIC` | Crisp, rapid deceleration | Modern snappy desktop UI |
| `CEL_EASE_OUT_BOUNCE` | Decelerates and bounces off target | Loot drops, alerts, cartoony interfaces |
| `CEL_EASE_OUT_BACK` | Slightly overshoots target before settling | Playful buttons, badge popups |

### Custom Easing Functions
You can pass any function matching `float (*CelsEasingFn)(float t)`:
```c
static float SmoothStep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float val = cel_transition(target, 300, SmoothStep);
```

---

## 6. Real-World Walkthrough: Health Bar HUD

In [`examples/transition/app/app.c`](file:///D:/cels-workspace/library/cels/examples/transition/app/app.c), a console gauge demonstrates `cel_transition` smoothing:

```c
CEL_Composable(HealthBarHUD, const PlayerGaugeState*, gauge) {
    cel_watch(gauge);
    if (gauge == NULL) return;

    /* Smoothly interpolate visual gauge toward authoritative health */
    float visualHealth = cel_transition(gauge->currentHealth, 400, CEL_EASE_OUT_QUAD);

    int percent = (int)((visualHealth / gauge->maxHealth) * 100.0f);
    printf("Health: %3d%% (Visual: %5.1f | Auth: %5.1f)\n",
           percent, visualHealth, gauge->currentHealth);
}
```

When you press keys in the host process to deal damage, the authoritative health drops instantly, while the gauge prints a smooth ASCII progress bar sliding down over 400 milliseconds.

---

## 7. Best Practices & Pitfalls

### Do This
- **Use for Visual Presentation**: Use `cel_transition` for positions, opacities, rotations, and UI gauges.
- **Keep Authoritative State Discrete**: Keep game rules (e.g. "player is dead when health <= 0") bound to the authoritative state, not the transition scalar.
- **Omit Easing for Defaults**: `cel_transition(target, 300)` automatically selects `CEL_EASE_OUT_QUAD`, which looks great in 90% of UI scenarios.

### Don't Do That
- ❌ **Don't Manually Accumulate Delta Time**:
  ```c
  /* BAD: Manual delta-time accumulator */
  float *t = cel_remember(float, 0.0f);
  *t += dt;
  float val = Lerp(start, end, *t / dur);
  ```
  *Use `cel_transition(end, dur)`—CELS manages monotonic time, in-flight retargeting, and zero-CPU idle for you.*
- ❌ **Don't Base Game Logic on Transition Values**: Never check `if (visualHealth <= 0.0f) GameOver();`. Check `if (player->health <= 0)` instead.

---

## Next Steps

Now that you've mastered transitions:
- Check out the final guide: [07. Live Code Hot-Reloading](07-hot-reloading.md) to supercharge your iteration cycle.
