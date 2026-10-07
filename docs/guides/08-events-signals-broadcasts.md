# Events, Signals & Engine Broadcasts

In declarative architectures, reactive state (`cel_watch`, `cel_mutate`, `cel_state`) models **continuous state of the world** (e.g., window size, player position, health). 

However, real-world interactive systems also require **discrete, instantaneous messages**:
- A button child firing a click notification up the composable tree to an ancestor.
- A combat or game session sending a directed damage signal to a specific HUD or inventory session.
- An audio engine, network worker, or physics system publishing a global event to all sessions across threads.
- A cooperative coroutine (`CEL_Task`) awaiting a specific packet, user confirmation, or timeout without polling.

CELS v0.4.0 introduces the **Unified Messaging Subsystem**, offering three precisely scoped communication mechanisms with dedicated syntax:

| Scope | Producer Macro | Consumer Block | Dispatch Mechanism | Use Case |
|---|---|---|---|---|
| **Local Tree** | `cel_event(Type, ...)` | `cel_listen(Type, var) { ... }` | Bubbles up composable tree to ancestors; immediate intra-frame delivery | UI button clicks, modal dialog responses, tab selections |
| **Targeted Session** | `cel_signal(session, Type, ...)` | `cel_connect(Type, var) { ... }` | Directed point-to-point delivery into target session inbox | Inter-session communication (e.g. Game -> HUD, Simulation -> Audio) |
| **Engine-Wide** | `cel_broadcast(Type, ...)` | `cel_bind(Type, var) { ... }` | Lock-free thread-safe engine bus drained across all sessions at frame boundaries | Background worker threads (audio, network, disk I/O, OS inputs) |

---

## 1. Local Tree Events (`cel_event` & `cel_listen`)

Local events bubble up from deep descendant composables to ancestor listeners in the same session. When an event is emitted, any registered ancestor is invalidated **immediately within the current frame** via intra-frame drain convergence.

### Producer: `cel_event(Type, ...)`
Emits a payload struct of `Type` into the active session's event queue.

```c
typedef struct ButtonClickEvent {
    int  buttonId;
    int  mouseX;
    int  mouseY;
} ButtonClickEvent;

CEL_Composable(SubmitButton, bool isPressed) {
    if (isPressed) {
        /* Emits up to parent containers */
        cel_event(ButtonClickEvent, {
            .buttonId = 101,
            .mouseX   = 450,
            .mouseY   = 320
        });
    }
}
```

### Consumer: `cel_listen(Type)` & `cel_listen(Type, var)`
Evaluates the enclosed block for every unconsumed event of `Type` emitted by descendants. Supports both 1-argument trigger form `cel_listen(Type)` and 2-argument payload form `cel_listen(Type, var)` (with automatic unused-variable warning suppression):

```c
CEL_Composable(RegistrationForm) {
    int *submitCount = cel_remember(int, 0);

    /* Listen for events bubbling from child buttons */
    cel_listen(ButtonClickEvent, ev) {
        (*submitCount)++;
        printf("[Form] Button %d clicked at (%d, %d). Submissions: %d\n",
               ev->buttonId, ev->mouseX, ev->mouseY, *submitCount);
    }

    SubmitButton(CheckNativeInput());
}
```

> [!NOTE]
> Local tree events guarantee **zero frame delay**. If a child emits an event during pass 1 of recomposition, the parent listener is marked dirty and evaluated in pass 2 before the frame is presented to the user.

---

## 2. Targeted Session Signals (`cel_signal` & `cel_connect`)

When multiple sessions exist (e.g. an engine running a primary UI session, a separate 3D viewport session, and a debugger overlay session), one session can send a directed point-to-point signal to another without broadcasting to the entire world.

### Producer: `cel_signal(targetSession, Type, ...)`
Directly places the signal into `targetSession`'s event inbox and schedules `targetSession` for recomposition:

```c
typedef struct DamageSignal {
    int   targetId;
    float damage;
    bool  isCritical;
} DamageSignal;

/* External game logic session, host loop, or simulation */
void ProcessCombatHit(CelsSession *hudSession, int entityId, float damage) {
    cel_signal(hudSession, DamageSignal, {
        .targetId   = entityId,
        .damage     = damage,
        .isCritical = (damage > 50.0f)
    });
}
```

### Engine-Supervised Named Sessions (`cel_create_session` & `cel_get_session`)
The host engine can create and supervise secondary sessions with dedicated capacity profiles and names:

```c
/* 1. In Host Setup: create a named secondary session */
CelsSession *hudSession = cel_create_session(&engine, "hud", CELS_PROFILE_256);

/* 2. Retrieve supervised sessions by name ("main", "root", or secondary) */
CelsSession *mainSession = cel_get_session(&engine, "main");
CelsSession *hud = cel_get_session(&engine, "hud");

/* 3. Send targeted signal to named session */
cel_signal(cel_get_session(&engine, "hud"), DamageSignal, {
    .targetId = 42,
    .damage = 15.0f,
    .isCritical = false
});
```

> [!TIP]
> Inside your application code, `CEL_OnStart` automatically receives the primary root session directly:
> ```c
> CEL_OnStart(App_OnStart) {
>     /* 'session' is the root primary session */
>     cel_attach(session, WindowComposition, WindowEval);
> }
> ```

### Consumer: `cel_connect(Type)` & `cel_connect(Type, var)`
Inside any composable of the target session, binds an iterator over incoming signals.
`cel_connect` supports both:
1. **1-Argument Trigger Form (`cel_connect(Type)`)**: Used when a signal is an event trigger requiring no payload inspection. Eliminates dummy variable declarations and avoids `(void)sig;`.
2. **2-Argument Payload Form (`cel_connect(Type, var)`)**: Binds each incoming signal payload to `var`. Unused-variable warnings are automatically suppressed, so developers never need to write `(void)sig;`.

Under the CELS **Actor Model Principle**, sessions **only mutate their own state**. Incoming signals are handled inside `cel_connect`, where the session mutates its own state locally:

```c
/* Trigger-only signal: no dummy variable or (void)sig needed */
cel_connect(StartWorkflowSignal) {
    cel_mutate(state) {
        this->taskWorkflowActive = true;
    }
}

/* Payload-binding signal: */
CEL_Composable(FloatingDamageHUD, PlayerGaugeState*, gauge) {
    int *floatingNumbersCount = cel_remember(int, 0);

    cel_connect(DamageSignal, sig) {
        (*floatingNumbersCount)++;
        printf("[HUD] Display damage: %.1f on entity #%d (Crit: %s)\n",
               sig->damage, sig->targetId, sig->isCritical ? "YES" : "NO");

        /* Session mutates its own state locally in response to the signal: */
        cel_mutate(gauge) {
            this->currentHealth -= sig->damage;
            if (this->currentHealth < 0.0f) this->currentHealth = 0.0f;
        }
    }
}
```

---

## 3. Global Engine Broadcasts (`cel_broadcast` & `cel_bind`)

Global broadcasts provide a thread-safe, lock-free engine bus. Background worker threads, external audio callbacks, network loops, or file watchers can publish broadcasts without knowing which sessions or composables exist.

### Producer: `cel_broadcast(Type, ...)`
Can be called from **any thread** (main loop, audio callback, socket worker). The engine stages broadcasts in a thread-safe double buffer and distributes them to all active sessions at frame boundaries:

```c
typedef struct AudioBroadcast {
    char  soundEffect[32];
    float volume;
} AudioBroadcast;

/* Audio worker thread or background simulation */
void OnPhysicsCollision(const char *soundFile) {
    cel_broadcast(AudioBroadcast, {
        .soundEffect = "metal_clank.wav",
        .volume      = 0.85f
    });
}
```

### Consumer: `cel_bind(Type)` & `cel_bind(Type, var)`
Inside any session anywhere in the engine, binds to global broadcasts of `Type`. Supports both 1-argument trigger form `cel_bind(Type)` and 2-argument payload form `cel_bind(Type, var)` (with automatic unused-variable warning suppression):

```c
CEL_Composable(AudioSubsystemWidget) {
    cel_bind(AudioBroadcast, bcast) {
        printf("[Audio] Playing SFX '%s' at volume %.2f\n",
               bcast->soundEffect, bcast->volume);
        PlayNativeAudio(bcast->soundEffect, bcast->volume);
    }
}
```

---

## 4. Fiber Task Awaiting (`cel_wait_for`, `cel_wait_signal`, `cel_wait_broadcast`, `cel_wait_for_timeout`)

Procedural workflows (`CEL_Task`) frequently need to pause until an external event, signal, or broadcast arrives. Rather than polling state variables every frame, CELS fibers can suspend execution non-blockingly until the message arrives.

### Available Task Wait Macros

```c
/* 1. Universal / Local Tree Event Wait */
cel_wait_for(Type, outPointer);

/* 2. Targeted Session Signal Wait */
cel_wait_signal(Type, outPointer);

/* 3. Global Engine Broadcast Wait */
cel_wait_broadcast(Type, outPointer);

/* 4. Timeout-Guarded Wait (returns bool: true if received, false if timed out) */
bool received = cel_wait_for_timeout(Type, outPointer, timeoutMs);
```

### Example: Multi-Step Interactive Workflow

```c
CEL_Task(WorkflowCoordinatorTask, EventDemoState*, state) {
    cancel {
        printf("[WorkflowTask] Workflow aborted.\n");
    }
    run {
        /* Step 1: Wait for local user interaction */
        UserActionEvent userEv;
        printf("[WorkflowTask] Step 1: Waiting for user action...\n");
        cel_wait_for(UserActionEvent, &userEv);
        printf("[WorkflowTask] Step 1 COMPLETE: User chose '%s'\n", userEv.actionName);

        /* Step 2: Wait for directed combat signal */
        CombatSignal combatSig;
        printf("[WorkflowTask] Step 2: Waiting for combat damage signal...\n");
        cel_wait_signal(CombatSignal, &combatSig);
        printf("[WorkflowTask] Step 2 COMPLETE: Took %.1f damage!\n", combatSig.damage);

        /* Step 3: Wait for background audio broadcast */
        AudioBroadcast audioBcast;
        printf("[WorkflowTask] Step 3: Waiting for victory broadcast...\n");
        cel_wait_broadcast(AudioBroadcast, &audioBcast);
        printf("[WorkflowTask] Step 3 COMPLETE: Playing '%s'\n", audioBcast.soundEffect);

        /* Step 4: Wait up to 500ms for confirmation; timeout gracefully if absent */
        UserActionEvent confirmEv;
        printf("[WorkflowTask] Step 4: Awaiting confirmation with 500ms timeout...\n");
        bool gotConfirm = cel_wait_for_timeout(UserActionEvent, &confirmEv, 500);
        if (gotConfirm) {
            printf("[WorkflowTask] Confirmed by user!\n");
        } else {
            printf("[WorkflowTask] Timed out waiting for confirmation.\n");
        }

        /* Mutate state from task fiber */
        cel_mutate(state) {
            this->workflowCompleted = true;
        }
        printf("[WorkflowTask] Workflow successfully completed!\n");
    }
}
```

---

## 5. Architectural Guarantees & Memory Model

1. **Zero Heap Allocation**:
   All event records, signal payloads, listeners, and task waiters live in static, cache-aligned circular memory pools inside `CelsEventQueue` (`session->eventQueue`) and `CelsEngine` (`engine->broadcastQueue`). No `malloc` or `free` calls are made on the hot path.
2. **Automatic Lifecycle Consumption**:
   Events are transient. Once all listeners in an evaluation pass finish reading an event, `CelsEventRetireConsumed` cleans up the record at frame commit. Unread events persist until consumed or retired.
3. **C99 Standard Compliance**:
   All producer and consumer macros expand to standard ISO C99 compound literals and `for` loops. Compatible with MSVC, Clang, GCC, and embedded C compilers.

---

## 6. Best Practices: Do This / Don't Do That

### Do This
- **Enforce the Actor Model Principle**: Sessions only mutate their own state via `cel_mutate(ptr)`. For cross-session or host communication, use `cel_signal(session, Type, ...)`.
- **Mutate State in Event and Signal Handlers**: Inside `cel_listen` or `cel_connect`, calling `cel_mutate(ptr)` stages the back-buffer mutation and schedules a convergence pass cleanly.
- **Use `cel_event` for user interactions**: Form submissions, button clicks, menu picks, drag-and-drop triggers.
- **Use `cel_signal` for cross-session messaging**: Communication between decoupled application modules or game sessions.
- **Use `cel_broadcast` for worker threads**: Audio playback completion, async network responses, hardware gamepad hot-plugging.
- **Use `cel_wait_for_timeout` to guard asynchronous tasks**: Prevent coroutines from hanging indefinitely when an expected network packet or event fails to arrive.

### Don't Do That
- **Don't directly mutate another session's state**: Calling multi-session mutate from the host or across threads is removed. Always dispatch a discrete signal via `cel_signal` to preserve concurrency safety and encapsulation.
- **Don't use events for continuous scalar state**: Never emit mouse positions on every frame via `cel_event`. Use double-buffered reactive state (`cel_state`, `cel_watch`, `cel_mutate`) or smooth transitions (`cel_transition`).
- **Don't unconditionally emit `cel_event` in a composable body**: If a composable emits an event on every render unconditionally without an input trigger, it will trigger infinite recomposition passes until `maxDrainIterations` is hit. Always guard emissions with interaction flags or user inputs.
