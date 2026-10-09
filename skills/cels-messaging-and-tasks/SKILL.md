---
name: cels-messaging-and-tasks
description: Provide technical guidance on CELS discrete events, cross-session signals, engine broadcasts, cooperative fiber tasks, and async message awaiting. Use when wiring up UI interactions, communicating between sessions, handling thread-safe background broadcasts, orchestrating multi-step async coroutines (CEL_Task), or awaiting events with timeouts.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.5.0"
  last-updated: '2026-10-07'
  category: messaging-and-tasks
  keywords:
    - events
    - signals
    - broadcasts
    - cel_event
    - cel_listen
    - cel_signal
    - cel_connect
    - cel_broadcast
    - cel_bind
    - tasks
    - CEL_Task
    - cel_wait
    - cel_wait_for
    - fibers
    - coroutines
    - C99
---

# CELS Events, Signals, Broadcasts & Cooperative Tasks Guide

CELS provides a unified asynchronous messaging and workflow subsystem in pure C99. Communication is organized into three distinct architectural scopes, complemented by stack-preserving fiber tasks (`CEL_Task`) that can non-blockingly await messages and delays.

---

## 1. The Three Communication Scopes

```
                    ┌────────────────────────────────────────────────────────┐
                    │               Engine-Wide Bus (Threads)                │
                    │               cel_broadcast / cel_bind                 │
                    └───────────────────────────┬────────────────────────────┘
                                                │
                 ┌──────────────────────────────┴──────────────────────────────┐
                 ▼                                                             ▼
  ┌─────────────────────────────┐                               ┌─────────────────────────────┐
  │         Session A           │                               │         Session B           │
  │                             │ ──── cel_signal(B, ...) ────> │                             │
  │  ┌───────────────────────┐  │ <─── cel_signal(A, ...) ───── │  ┌───────────────────────┐  │
  │  │ Parent Composable     │  │                               │  │ Parent Composable     │  │
  │  │ (cel_listen)          │  │                               │  │ (cel_connect)         │  │
  │  │   ▲                   │  │                               │  │                       │  │
  │  │   │ cel_event (local) │  │                               │  │                       │  │
  │  │   │                   │  │                               │  │                       │  │
  │  │ Child Composable      │  │                               │  │                       │  │
  │  └───────────────────────┘  │                               │  └───────────────────────┘  │
  └─────────────────────────────┘                               └─────────────────────────────┘
```

| Scope | Producer API | Consumer API | Boundaries | Timing / Guarantees |
| :--- | :--- | :--- | :--- | :--- |
| **1. Local Tree Event** | `cel_event(Type, { ... })` | `cel_listen(Type, ev)` | Intra-session composable tree | Bubbles up immediately; converges within the exact same frame. |
| **2. Targeted Signal** | `cel_signal(sess, Type, { ... })` | `cel_connect(Type, sig)` | Point-to-point inter-session | Pushed into target inbox; delivered on target's next recompose pass. |
| **3. Global Broadcast** | `cel_broadcast(Type, { ... })` | `cel_bind(Type, bcast)` | Thread-safe engine-wide bus | Dispatched from any OS worker thread; drained at frame boundaries. |

---

## 2. Local Tree Events (`cel_event` & `cel_listen`)

- **Scope**: Inside a single session's composable hierarchy.
- **Direction**: Bubbles up from child/descendant to parent/ancestor.
- **Timing**: Immediate intra-frame convergence. Ancestor listeners re-evaluate within the exact same frame without 1-frame latency.
- **Consumers**:
  - `cel_listen(Type) { ... }`: Trigger-only handler.
  - `cel_listen(Type, ev) { ... }`: Payload-binding handler (suppresses unused variable warnings automatically).

```c
typedef struct FormSubmitEvent {
    int formId;
    const char *username;
} FormSubmitEvent;

CEL_Composable(SubmitButton, bool isClicked) {
    if (isClicked) {
        cel_event(FormSubmitEvent, {
            .formId = 1,
            .username = "Alice"
        });
    }
}

CEL_Composable(LoginForm) {
    cel_listen(FormSubmitEvent, ev) {
        printf("Submitted form #%d for user '%s'\n", ev->formId, ev->username);
    }

    SubmitButton(PollClick());
}
```

> [!TIP]
> **Guard Event Emission**: Composables re-evaluate on every recomposition pass. Always guard `cel_event` inside user interaction flags (e.g. `if (clicked) cel_event(...)`) to prevent infinite intra-frame loops.

---

## 3. Targeted Session Signals (`cel_signal` & `cel_connect`)

- **Scope**: Directed communication between distinct sessions or from the host into a session.
- **Direction**: Direct push from sender into target session's inbox.
- **Timing**: Delivered during the target session's next recomposition pass.
- **Consumers**:
  - `cel_connect(Type) { ... }`: Trigger-only handler.
  - `cel_connect(Type, sig) { ... }`: Payload-binding handler.

```c
typedef struct DamageSignal {
    int targetId;
    float amount;
} DamageSignal;

/* External game simulation, host loop, or combat session */
void AttackEnemy(CelsEngine *engine, int enemyId, float damage) {
    CelsSession *hudSession = cel_get_session(engine, "hud");
    cel_signal(hudSession, DamageSignal, {
        .targetId = enemyId,
        .amount = damage
    });
}

CEL_Composable(DamageIndicatorHUD) {
    cel_connect(DamageSignal, sig) {
        printf("Displaying floating damage %.1f on entity #%d\n", sig->amount, sig->targetId);
    }
}
```

### The Actor Model Invariant
Sessions **only mutate their own state** via `cel_mutate(ptr)`.
Cross-session or host-to-session state mutation directly via pointers is strictly forbidden because it violates session thread boundaries. Instead, external callers send targeted signals via `cel_signal`. Inside the target session, `cel_connect` reads the signal and stages local state changes via `cel_mutate`.

---

## 4. Engine-Wide Global Broadcasts (`cel_broadcast` & `cel_bind`)

- **Scope**: Global, engine-wide, thread-safe message bus.
- **Direction**: Dispatched from any OS worker thread (audio, network, physics, background asset loading).
- **Timing**: Thread-safe lockless append; drained at frame boundaries (`CelsEngineRecompose`) and distributed to all active sessions.
- **Consumers**:
  - `cel_bind(Type) { ... }`: Trigger-only handler.
  - `cel_bind(Type, bcast) { ... }`: Payload-binding handler.

```c
typedef struct SoundBroadcast {
    const char *soundFile;
    float volume;
} SoundBroadcast;

/* Audio worker thread callback */
void OnWorkerSoundFinished(const char *name) {
    cel_broadcast(SoundBroadcast, {
        .soundFile = name,
        .volume = 1.0f
    });
}

CEL_Composable(AudioMixerComponent) {
    cel_bind(SoundBroadcast, bcast) {
        printf("Mixer handling sound: '%s' at volume %.2f\n", bcast->soundFile, bcast->volume);
    }
}
```

---

## 5. Procedural Tasks (`CEL_Task`)

Declarative rendering is ideal for projecting state, but real software frequently requires **procedural, multi-step asynchronous processes** (network handshakes, cutscenes, timed delays, background polling).

`CEL_Task` provides a **stack-preserving cooperative coroutine** implemented via lightweight fibers. Tasks run procedural C code across multiple frames without blocking the main engine thread or creating separate OS threads.

### Defining a Task
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

        while (1) {
            cel_wait(50); /* Poll every 50ms */
            PollServerPackets();
        }
    }
}
```

### Scheduling & Mounting a Task
Invoke the task inside any `CEL_Composition` or `CEL_Composable` using `cel_task()`:

```c
CEL_Composable(NetworkHUD, NetworkState*, net) {
    cel_watch(net);

    /* Conditionally mount task:
     * While isConnecting is true, the task executes.
     * The moment isConnecting becomes false, the task is unmounted and its
     * cancel {} block runs immediately! */
    if (net->isConnecting) {
        cel_task(NetworkConnectTask, "auth.game.net", 443);
    }
}
```

### Task Control Operators (Inside `run { ... }`)
- **`cel_wait(ms)`**: Suspends execution for `ms` milliseconds. Engine continues ticking and rendering. When the monotonic deadline expires, the task resumes with all local stack variables preserved.
- **`cel_yield()`**: Suspends execution until the next engine frame tick.
- **`cel_cancel()`**: Aborts the task immediately from within its body and jumps directly to `cancel { ... }`.

### Lifecycle Teardown (`cancel`)
The `cancel { ... }` block is **guaranteed to run** in two situations:
1. **Explicit Cancellation**: When `cel_cancel()` is called internally or `cel_cancel_task(TaskName)` is called externally.
2. **Structural Unmount**: If the parent composable unmounts or skips the `cel_task(...)` branch, slot table reconciliation detects the omission and invokes `cancel` before reclaiming memory.

### Task Query & Control Functions
| Function | Purpose | Where to Call |
| :--- | :--- | :--- |
| `cel_task(TaskName, ...)` | Mounts / advances task | Inside `CEL_Composition` or `CEL_Composable` |
| `cel_cancel_task(TaskName)` | Cancels task and runs `cancel` block | Event handlers, UI button callbacks |
| `cel_restart_task(TaskName)` | Resets task to step 0 and re-executes | Retry buttons, reconnect handlers |
| `cel_is_task_running(TaskName)` | Returns `true` if active and not completed/cancelled | UI spinners, button disabling |
| `cel_is_task_done(TaskName)` | Returns `true` if `run` finished to completion | Sequential wizard progression |
| `cel_is_task_cancelled(TaskName)` | Returns `true` if cancelled | Error / retry banners |

---

## 6. Fiber Task Message Awaiting (`cel_wait_for`)

Inside a `CEL_Task`, cooperative fibers can non-blockingly suspend their execution until an event, signal, or broadcast arrives:

| Macro | Description | Scope Filter |
| :--- | :--- | :--- |
| `cel_wait_for(Type, outPtr)` | Suspends until local event, signal, or broadcast of `Type` arrives | `CELS_EVENT_SCOPE_ANY` |
| `cel_wait_signal(Type, outPtr)` | Suspends until targeted signal of `Type` arrives | `CELS_EVENT_SCOPE_SIGNAL` |
| `cel_wait_broadcast(Type, outPtr)` | Suspends until engine broadcast of `Type` arrives | `CELS_EVENT_SCOPE_BROADCAST` |
| `cel_wait_for_timeout(Type, outPtr, ms)` | Suspends until message arrives or `ms` timeout elapses (returns `bool`) | `CELS_EVENT_SCOPE_ANY` |

### Multi-Step Async Workflow Example:
```c
CEL_Task(OrderFulfillmentTask, OrderState*, order) {
    cancel {
        printf("Order workflow cancelled or unmounted.\n");
    }

    run {
        /* Step 1: Wait for local confirmation click */
        ConfirmEvent confirm;
        cel_wait_for(ConfirmEvent, &confirm);

        /* Step 2: Wait for payment gateway signal */
        PaymentSignal payment;
        cel_wait_signal(PaymentSignal, &payment);

        /* Step 3: Wait for inventory confirmation with 5-second timeout */
        InventoryEvent inv;
        bool confirmed = cel_wait_for_timeout(InventoryEvent, &inv, 5000);
        if (!confirmed) {
            printf("Inventory verification timed out!\n");
            cel_cancel();
        }

        /* Mutate order state safely inside task */
        cel_mutate(order) {
            this->isCompleted = true;
        }
    }
}
```

---

## 7. The Power Combo: Tasks (Choreography) + Transitions (Visuals)

Pairing **Tasks** for procedural state progression with **Transitions** for visual smoothing is the canonical CELS pattern for cutscenes, tutorials, and complex UI flows:

```c
/* 1. Procedural Choreography: Advances authoritative game state over time */
CEL_Task(BossIntroSequence, BossState*, boss) {
    cancel {
        cel_mutate(boss) {
            this->cutsceneActive = false;
            this->cameraZoom = 1.0f;
        }
    }
    run {
        cel_mutate(boss) { this->cameraZoom = 2.5f; } // Zoom in
        cel_wait(1000);

        cel_mutate(boss) { this->roarPlayed = true; }  // Roar
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
    float visualZoom = cel_transition(boss->cameraZoom, 500, CEL_EASE_OUT_QUAD);
    ApplyCameraMatrix(visualZoom);

    if (boss->cutsceneActive) {
        cel_task(BossIntroSequence, (BossState*)boss);
    }
}
```

---

## 8. Best Practices & Invariants

1. **Zero Heap Allocation**: All event records, signals, and task waiters reside in fixed-size contiguous circular buffers. Zero `malloc` on the messaging hot path.
2. **Actor Model Principle**: Sessions mutate their own state via `cel_mutate(ptr)`. Cross-session mutations must use `cel_signal`.
3. **Guard Event Emission**: Always wrap `cel_event` in condition checks (`if (clicked) cel_event(...)`) to prevent infinite intra-frame recompositions.
4. **Automatic Retirement**: Events are transient. Once all active listeners process an event during a frame pass, consumed records are retired automatically.
