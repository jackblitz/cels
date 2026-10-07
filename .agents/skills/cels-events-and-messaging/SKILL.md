---
name: cels-events-and-messaging
description: Provide technical guidance on CELS discrete events, cross-session signals, engine broadcasts, and task fiber awaiting. Use when implementing UI event bubbling, point-to-point inter-session signals, thread-safe global broadcasts, or asynchronous multi-step task workflows.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.4.0"
  last-updated: '2026-09-30'
  category: messaging
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
    - cel_wait_for
    - cel_wait_signal
    - cel_wait_broadcast
    - cel_wait_for_timeout
    - fibers
    - tasks
    - messaging
    - C99
---

# CELS Events, Signals & Global Broadcasts Guide

CELS v0.4.0 introduces the **Unified Messaging Subsystem**, supporting discrete, instantaneous communication across three distinct scopes, along with non-blocking cooperative fiber task awaiting.

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

---

## 1. The Three Communication Scopes

### 1.1 Local Tree Events (`cel_event` / `cel_listen`)
- **Scope**: Inside a single session's composable tree.
- **Direction**: Bubbles up from child/descendant to parent/ancestor.
- **Timing**: Immediate intra-frame convergence. Ancestor listeners re-evaluate within the exact same frame without 1-frame latency.
- **Producer**: `cel_event(Type, { .field = value, ... });`
- **Consumer**: `cel_listen(Type) { ... }` (trigger-only) or `cel_listen(Type, ev) { /* handle event */ }` (payload-binding; automatic unused variable suppression)

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

---

### 1.2 Targeted Session Signals (`cel_signal` / `cel_connect`)
- **Scope**: Directed point-to-point communication between sessions.
- **Direction**: Direct push from sender into target session's inbox.
- **Timing**: Delivered during the target session's next recomposition pass.
- **Producer**: `cel_signal(targetSession, Type, { .field = value, ... });`
- **Consumer**: `cel_connect(Type) { ... }` (trigger-only) or `cel_connect(Type, sig) { /* handle signal */ }` (payload-binding; automatic unused variable suppression)

```c
typedef struct DamageSignal {
    int targetId;
    float amount;
} DamageSignal;

/* External game simulation, host loop, or combat session */
void AttackEnemy(CelsEngine *engine, int enemyId, float damage) {
    /* Send signal to named secondary session or main session */
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

#### Engine Named Sessions & Lifecycle
Supervised secondary sessions can be created with dedicated capacity profiles and looked up by name:
```c
/* 1. Host creates a secondary named session */
CelsSession *hudSession = cel_create_session(&engine, "hud", CELS_PROFILE_256);

/* 2. Retrieve supervised sessions ("main", "root", or named secondary) */
CelsSession *mainSession = cel_get_session(&engine, "main");
CelsSession *hud = cel_get_session(&engine, "hud");

/* 3. Dispatch targeted signal */
cel_signal(hud, DamageSignal, { .targetId = 7, .amount = 50.0f });
```

---

### 1.3 Engine-Wide Broadcasts (`cel_broadcast` / `cel_bind`)
- **Scope**: Global, engine-wide, thread-safe message bus.
- **Direction**: Dispatched from any OS worker thread (audio, network, physics, input).
- **Timing**: Drained at frame boundaries (`CelsEngineRecompose`) and distributed to all active sessions.
- **Producer**: `cel_broadcast(Type, { .field = value, ... });` (Thread-Safe)
- **Consumer**: `cel_bind(Type) { ... }` (trigger-only) or `cel_bind(Type, bcast) { /* handle broadcast */ }` (payload-binding; automatic unused variable suppression)

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

## 2. Fiber Task Awaiting (`cel_wait_for`)

Cooperative tasks (`CEL_Task`) can non-blockingly suspend their fiber until an event, signal, or broadcast arrives:

| Macro | Description | Scope Filter |
|---|---|---|
| `cel_wait_for(Type, outPtr)` | Suspends until local event, signal, or broadcast of `Type` arrives | `CELS_EVENT_SCOPE_ANY` |
| `cel_wait_signal(Type, outPtr)` | Suspends until targeted signal of `Type` arrives | `CELS_EVENT_SCOPE_SIGNAL` |
| `cel_wait_broadcast(Type, outPtr)` | Suspends until engine broadcast of `Type` arrives | `CELS_EVENT_SCOPE_BROADCAST` |
| `cel_wait_for_timeout(Type, outPtr, timeoutMs)` | Suspends until event of `Type` arrives or `timeoutMs` elapses (returns `bool`) | `CELS_EVENT_SCOPE_ANY` |

```c
CEL_Task(OrderFulfillmentTask, OrderState*, order) {
    cancel {
        printf("Order workflow aborted.\n");
    }
    run {
        /* Step 1: Wait for user confirmation */
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

## 3. Best Practices & Invariants

1. **Zero Heap Allocation**:
   All event records, signals, and waiters reside in fixed-size contiguous circular buffers. Zero `malloc` on the messaging hot path.
2. **The Actor Model Principle (Sessions Only Mutate Their Own State)**:
   Sessions only mutate their own state via `cel_mutate(ptr)`. Cross-session and host mutations bypass actor boundaries, so `cel_mutate(&session, ...)` is removed. Instead, external callers send targeted signals via `cel_signal(targetSession, Type, ...)`. Inside the target session, `cel_connect` and `cel_listen` handlers process the signal and mutate local state safely with `cel_mutate(ptr)`.
3. **Guard Event Emission**:
   Composables re-evaluate on every recomposition pass. Always guard `cel_event` inside user interaction flags (e.g. `if (clicked) cel_event(...)`) to prevent infinite intra-frame recomposition loops.
4. **Automatic Retirement**:
   Events are transient. Once all registered listeners read an event during a pass, `CelsEventRetireConsumed` retires consumed records at frame completion.
