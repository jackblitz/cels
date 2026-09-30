---
name: cels-time-and-motion
description: Provide technical guidance on temporal state convergence, declarative transitions, and procedural asynchronous tasks in CELS. Use when implementing smooth visual transitions, easing curves, multi-step asynchronous workflows, timers, network polling, or coroutine state machines.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.3.0"
  last-updated: '2026-09-30'
  category: time-and-motion
  keywords:
    - time
    - motion
    - transitions
    - tasks
    - cel_transition
    - CEL_Task
    - cel_wait
    - cel_yield
    - cel_cancel
    - fibers
    - coroutines
    - easing-curves
    - animation
    - asynchronous
    - C99
---

# CELS Temporal Architecture: Tasks & Declarative Transitions

CELS provides a unified temporal architecture designed for real-time applications and game engines in pure C99. Time and motion in CELS are divided into two distinct, complementary paradigms:

1. **Declarative Transitions (`cel_transition`)**: **Continuous Mathematical Convergence**. Used inside composables to smoothly interpolate visual representations toward authoritative state over time without keyframes or tween controllers.
2. **Procedural Tasks (`CEL_Task`)**: **Discrete Chronological Workflows**. Cooperative, fiber-backed coroutines that execute multi-step logic, delays (`cel_wait`), and background workflows across frames with guaranteed unmount teardown (`cancel`).

```
┌────────────────────────────────────────────────────────────────────────┐
│                   AUTHORITATIVE RECTIVE STATE                          │
│                   (e.g., PlayerState, NetworkState)                    │
└───────────────────▲────────────────────────────────┬───────────────────┘
                    │ Mutates discrete steps         │ Read target value
                    │ (cel_mutate)                   │ (cel_watch)
┌───────────────────┴────────────────┐   ┌───────────▼───────────────────┐
│     PROCEDURAL TASKS (CEL_Task)    │   │  TRANSITIONS (cel_transition) │
│ - Asynchronous coroutines          │   │ - Continuous visual smoothing │
│ - Multi-step workflows & delays    │   │ - Easing curves (quad, bounce)│
│ - cel_wait(ms), cel_yield()        │   │ - Retargets mid-flight        │
│ - cancel { ... } cleanup on unmount│   │ - 0% CPU idle when settled    │
└────────────────────────────────────┘   └───────────────────────────────┘
```

---

## 1. Quick Positioning: When to Use What?

Use this decision matrix to determine whether a problem calls for a **Transition**, a **Task**, or a **Combination of Both**:

| Requirement / Scenario | Recommended Tool | Why? |
| :--- | :--- | :--- |
| **Health bar, exp gauge, volume slider** | `cel_transition()` | Smoothly follows a scalar target. No tween management; automatically retargets if hit again mid-flight. |
| **Fade in / out, modal slide-in** | `cel_transition()` | Declarative interpolation of opacity or coordinate offsets based on boolean visibility state. |
| **Network connection handshake** | `CEL_Task` | Procedural multi-frame progression (DNS -> Connect -> Auth -> Loop) with socket cleanup in `cancel`. |
| **Timed delay before next event** | `CEL_Task` (`cel_wait`) | Non-blocking sleep that cooperates with the engine frame loop without blocking OS threads. |
| **Multi-stage boss battle cutscene** | **Both** | `CEL_Task` coordinates the chronological script & state changes; `cel_transition` visually eases the characters & UI. |
| **Physical momentum, bouncing cards** | `celm_spring()` | Domain motion physics (second-order ODE) built on top of CELS slot tables. |
| **Mass physics simulation (10k units)** | **ECS (Flecs)** | Heavy parallel simulation belongs in cache-aligned ECS systems, not reactive UI memory. |

---

## 2. Declarative Transitions (`cel_transition`)

### Core Concept
In CELS, game state remains **authoritative and instantaneous**. You do not animate gameplay data; you declare how the visual projection converges toward that gameplay data.

```c
CEL_Composable(HealthBarHUD, const PlayerState*, player) {
    cel_watch(player);
    if (!player) return;

    /* Smoothly eases visual display toward authoritative player->health over 400ms */
    float visualHealth = cel_transition(player->health, 400, CEL_EASE_OUT_QUAD);

    RenderProgressBar(visualHealth / player->maxHealth);
}
```

When `player->health` drops instantly from `100.0` to `40.0`:
- The game logic immediately registers `40.0`.
- `visualHealth` smoothly eases toward `40.0` over `400ms`.
- If the player is hit again at $t = 150\text{ms}$ (dropping to `10.0`), `cel_transition` immediately catches the current interpolated position and retargets toward `10.0`. **There are zero visual pops or teleports.**

### API Reference
Defined in [`include/cels/transition.h`](file:///D:/cels-workspace/library/cels/include/cels/transition.h):

```c
float cel_transition(float targetValue, uint32_t durationMs, CelsEaseFn easeFn);
```

- **`targetValue`**: The authoritative target scalar (position, health, alpha, scale).
- **`durationMs`**: Duration in milliseconds. If `0`, snaps to target immediately.
- **`easeFn`**: Easing curve matching `float (*CelsEaseFn)(float t)`. Pass `NULL` for `CEL_EASE_LINEAR`.
- **Return**: Interpolated scalar for the current frame.

### Key Characteristics
1. **Zero Heap Allocations**: Transition tracking state (`CelsTransitionState`) is stored directly in the active composable's slot table (`cel_remember`).
2. **Automatic Frame Invalidation**: While running (`elapsed < durationMs`), the transition flags the composable group as dirty, causing continuous re-evaluation each frame.
3. **0% CPU Idle**: Once the transition settles at `targetValue`, invalidations cease entirely. The engine drops to zero recompositions until state changes again.
4. **Automatic Reclamation**: When the parent composable leaves the composition hierarchy (e.g. window closed), the transition state is freed automatically.

### Built-in Easing Functions
| Easing Identifier | Curve Description | Best Used For |
| :--- | :--- | :--- |
| `CEL_EASE_LINEAR` | Constant velocity ($f(t) = t$) | Progress bars, constant timers |
| `CEL_EASE_IN_QUAD` | Accelerating from zero ($f(t) = t^2$) | Falling items, exiting modals |
| `CEL_EASE_OUT_QUAD` | Decelerating to zero ($f(t) = 1 - (1-t)^2$) | Entering UI, HUD gauges, camera snapping |
| `CEL_EASE_IN_OUT_QUAD`| Accelerating then decelerating | Layout element swapping |
| `CEL_EASE_IN_CUBIC` | Stronger acceleration ($f(t) = t^3$) | Rapid departures |
| `CEL_EASE_OUT_CUBIC`| Stronger deceleration ($f(t) = 1 - (1-t)^3$) | Crisp, responsive UI panels |
| `CEL_EASE_IN_OUT_CUBIC`| Steep S-curve acceleration | Full-screen transitions |
| `CEL_EASE_OUT_BOUNCE`| Decelerates and bounces off target | Loot drops, alerts, cartoony UI |
| `CEL_EASE_OUT_ELASTIC`| Overshoots with decaying harmonic oscillation | Snappy switches, button badges |

Custom easing curves are standard C functions:
```c
float MyCustomCurve(float t) {
    return t * t * (3.0f - 2.0f * t); /* Smoothstep */
}
float val = cel_transition(target, 300, MyCustomCurve);
```

---

## 3. Procedural Tasks (`CEL_Task`)

### Core Concept
Declarative rendering is ideal for projecting state, but real software frequently requires **procedural, multi-step asynchronous processes**:
- Establishing network sockets with handshakes and timeouts.
- Playing sequenced cutscenes with timed pauses.
- Periodic polling or background processing.
- Multi-step tutorial flows.

`CEL_Task` provides a **stack-preserving cooperative coroutine** implemented via fibers. Tasks run procedural C code across multiple frames without blocking the main engine thread or creating separate OS threads.

### Defining a Task
Defined in [`include/cels/task.h`](file:///D:/cels-workspace/library/cels/include/cels/task.h):

```c
#include "cels.h"

CEL_Task(NetworkConnectTask, const char*, server, int, port) {
    cancel {
        /* Cleanup executed on explicit cancel OR when unmounted from composition */
        printf("Closing socket to %s:%d\n", server, port);
        CloseSocket();
    }

    run {
        printf("Resolving %s...\n", server);
        ResolveHost(server);
        cel_wait(150); /* Non-blocking 150ms delay */

        printf("Connecting to port %d...\n", port);
        ConnectSocket(server, port);
        cel_wait(100);

        printf("Handshake completed!\n");

        /* Continuous polling loop */
        while (1) {
            cel_wait(50); /* Poll every 50ms */
            PollServerPackets();
        }
    }
}
```

### Scheduling a Task
Invoke the task inside any `CEL_Composition` or `CEL_Composable` using `cel_task()`:

```c
CEL_Composable(NetworkHUD, NetworkState*, net) {
    cel_watch(net);

    /* Conditionally mount task. While isConnecting is true, the task executes.
     * The moment isConnecting becomes false, the task is unmounted and its
     * cancel {} block runs immediately! */
    if (net->isConnecting) {
        cel_task(NetworkConnectTask, "auth.game.net", 443);
    }
}
```

### Task Control Operators
Inside the `run { ... }` block:
- **`cel_wait(ms)`**: Suspends execution for `ms` milliseconds. The engine continues ticking other composables and processing input. Once the monotonic deadline passes, the task resumes immediately after `cel_wait` with all stack variables intact.
- **`cel_yield()`**: Suspends execution and yields control back to the engine until the next frame tick.
- **`cel_cancel()`**: Aborts the task from within its own execution body and jumps directly to `cancel { ... }`.

### Lifecycle & Unmount Teardown (`cancel`)
The `cancel { ... }` block is **guaranteed to run** in two situations:
1. **Explicit Cancellation**: When `cel_cancel()` is called internally or `cel_cancel_task(TaskName)` is triggered externally.
2. **Structural Unmount**: If the parent composable unmounts or skips the `cel_task(...)` branch, the CELS slot table reconciliation detects the unmount and immediately invokes the task's `cancel` block before freeing its memory.

### Task Query & Control Functions
| Function | Purpose | Where to Call |
| :--- | :--- | :--- |
| `cel_task(TaskName, ...)` | Mounts/advances task | Inside `CEL_Composition` or `CEL_Composable` |
| `cel_cancel_task(TaskName)` | Externally cancels the task and runs `cancel` | Event handlers, button callbacks |
| `cel_restart_task(TaskName)` | Resets task to step 0 and triggers execution | Retry buttons, reconnect handlers |
| `cel_is_task_running(TaskName)` | Returns `true` if active and not done/cancelled | UI loading spinners, button disabling |
| `cel_is_task_done(TaskName)` | Returns `true` if `run` finished to completion | Flow progression, next-step triggers |
| `cel_is_task_cancelled(TaskName)` | Returns `true` if task was cancelled | Error/retry banners |

---

## 4. Architectural Patterns: Combining Tasks & Transitions

The most powerful pattern in CELS pairs **Tasks** for procedural state progression with **Transitions** for visual smoothing:

### The Cutscene / Boss Battle Pattern
```c
/* 1. Procedural Choreography: Advances authoritative game state over time */
CEL_Task(BossIntroSequence, BossState*, boss) {
    cancel {
        /* If cutscene is skipped, jump to end state */
        cel_mutate(boss) {
            this->cutsceneActive = false;
            this->cameraZoom = 1.0f;
            this->alpha = 1.0f;
        }
    }
    run {
        cel_mutate(boss) { this->cameraZoom = 2.5f; } // Zoom in on boss
        cel_wait(1000);

        cel_mutate(boss) { this->roarRoarPlayed = true; }
        cel_wait(1500);

        cel_mutate(boss) { 
            this->cameraZoom = 1.0f; 
            this->cutsceneActive = false; 
        }
    }
}

/* 2. Visual Projection: Smoothly animates toward the state driven by the task */
CEL_Composable(BossCameraHUD, const BossState*, boss) {
    cel_watch(boss);

    /* Smoothly eases camera zoom over 500ms using Quad Out */
    float smoothZoom = cel_transition(boss->cameraZoom, 500, CEL_EASE_OUT_QUAD);
    SetCameraProjection(smoothZoom);

    if (boss->cutsceneActive) {
        DrawCinematicBlackBars();
    }
}
```

**Why this architecture excels:**
- **Separation of Concerns**: The task contains zero easing math, interpolation timers, or rendering logic. It only cares about game events.
- **Decoupled Smoothing**: The UI composable contains zero state-machine logic. It purely reflects the current state with smooth transitions.
- **Skip Safety**: If the player presses [ESC] to skip the cutscene, calling `cel_cancel_task(BossIntroSequence)` triggers `cancel` to immediately snap the game state to gameplay readiness, and the camera automatically eases cleanly to normal view.

---

## 5. Core Patterns & Anti-Patterns (Best for LLMs)

### Pattern 1: Continuous Visual Animations

```c
// WRONG: Manually ticking delta time or interpolating coordinates in state loops
run {
    for (float x = 0; x < 100; x += 1.0f) {
        cel_mutate(ui) { this->x = x; } // Bad! Triggers 100 frame recompositions
        cel_yield();
    }
}

// CORRECT: Mutate target once, and declare mathematical convergence with cel_transition
cel_mutate(ui) { this->targetX = 100.0f; }

CEL_Composable(MyWidget, const UIState*, ui) {
    cel_watch(ui);
    float x = cel_transition(ui->targetX, 300, CEL_EASE_OUT_QUAD);
    RenderBox(x);
}
```

### Pattern 2: Multi-Frame Delays

```c
// WRONG: Calling OS blocking sleep functions
CEL_Task(BadTask) {
    run {
        Sleep(500); // Catastrophic! Freezes the engine frame loop and OS window!
    }
}

// CORRECT: Use cooperative non-blocking cel_wait inside a CEL_Task
CEL_Task(GoodTask) {
    run {
        cel_wait(500); // Suspends fiber; engine continues ticking other composables
    }
}
```

### Pattern 3: Resource Management across Task Delays

```c
// WRONG: Allocating heap memory across cel_wait without a cancel block
CEL_Task(LeakTask) {
    run {
        void *buf = malloc(1024);
        cel_wait(1000); // If unmounted while waiting, buf leaks!
        free(buf);
    }
}

// CORRECT: Guarantee cleanup in cancel block
CEL_Task(SafeTask) {
    static void *buf = NULL;
    cancel {
        if (buf) { free(buf); buf = NULL; }
    }
    run {
        buf = malloc(1024);
        cel_wait(1000);
        free(buf);
        buf = NULL;
    }
}
```

---

## 6. Summary Cheat Sheet

| Feature | `cel_transition` | `CEL_Task` |
| :--- | :--- | :--- |
| **Header** | `#include "cels/transition.h"` | `#include "cels/task.h"` |
| **Typical Target** | Floating-point scalars (pos, alpha, scale, health) | Multi-step procedures, timers, network workflows |
| **Persistence** | Slot table (`CelsTransitionState`) | Slot table (`CelsTaskState` + fiber context) |
| **Non-blocking Delay** | N/A (continuous interpolation) | `cel_wait(milliseconds)` |
| **Yield to Next Frame**| N/A | `cel_yield()` |
| **Teardown Hook** | Automatic slot memory deallocation | Explicit `cancel { ... }` block |
| **Recomposition Trigger**| Invalidates group while interpolating | Invalidates group while stepping/waiting |
| **Idle Behavior** | Drops to 0% CPU once settled | Drops to 0% CPU once done or waiting |
