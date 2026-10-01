---
name: cels-reactive-state
description: Provide technical guidance on CELS reactive state management, slot table memory, and lifecycle hooks. Use when implementing stateful composables, observing/mutating reactive state, state hoisting, allocating persistent slot variables, or managing mount/unmount resource lifecycles.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.3.0"
  last-updated: '2026-09-30'
  category: state-management
  keywords:
    - state-management
    - reactive-state
    - state-hoisting
    - slot-table
    - cel_state
    - cel_watch
    - cel_mutate
    - cel_remember
    - cel_lifecycle
    - double-buffering
    - C99
---

# CELS Reactive State, Slot Memory & Lifecycle Guide

CELS implements a declarative, double-buffered reactive state system and slot-table memory model in pure C99. This guide details how to declare state, observe changes, stage mutations, retain persistent slot memory, and bind resource lifecycles.

---

## 1. Reactive State Management & State Hoisting

CELS provides two tiers of reactive state management:
1. **State Hoisting (Primary & Recommended)**: State instances allocated within composables using `cel_state` and passed down the tree via standard C function arguments. Zero string IDs required.
2. **Keyed Session State (Global/Cross-Session)**: State registered with a 64-bit unique ID (`cel_remember_state`) for globally addressable registries or external bridges.

Both use double-buffering (front buffer = current committed read state, back buffer = staged write state).

### Defining State Structs
Define state types using the `CEL_State` macro in a shared header:

```c
#pragma once
#include "cels.h"

CEL_State(WindowState) {
    bool isOpen;
    bool showBadge;
    int  width;
    int  height;
    void *nativeHandle;
};
```

---

### Primary Pattern: State Hoisting (Zero String IDs)

#### Creating Hoisted State: `cel_state`
Inside a parent composition or container composable, allocate state with `cel_state`:

```c
CEL_Composition(MainWindow, void *userData) {
    (void)userData;
    // Pinned to this slot, auto-generated unique ID, auto-cleanup on unmount:
    WindowState *win = cel_state(WindowState, ((WindowState){
        .isOpen = true,
        .showBadge = true,
        .width  = 800,
        .height = 600,
        .nativeHandle = NULL
    }));

    // Pass down to child composables (State Hoisting)
    WindowContent(win);
}
```

#### Observing Hoisted State: `cel_watch(ptr)`
In child composables, pass the pointer to `cel_watch(ptr)`. In $O(1)$, CELS inspects the preceding `CelsStateHeader` and subscribes the calling composable group to changes:

```c
CEL_Composable(WindowContent, WindowState*, win) {
    // Subscribes this composable group to 'win'
    cel_watch(win);

    printf("Window dimensions: %d x %d\n", win->width, win->height);

    if (win->showBadge) {
        StatusBadge();
    }
}
```

#### Mutating Hoisted State: `cel_mutate(ptr)`
From event handlers, input callbacks, or host loops, mutate the instance directly—no session pointer or string ID needed:

```c
/* Mutating state from an input event or host tick */
cel_mutate(win) {
    this->showBadge = !this->showBadge;
}
```

---

### Secondary Pattern: Session State & Type-Based Registry

For global singleton states or cross-session lookups (zero manual IDs needed):

#### Initializing / Registering: `cel_remember_state`
`cel_remember_state(Type, ...)` auto-hashes `#Type` at compile time:
```c
WindowState *win = cel_remember_state(WindowState, {
    .isOpen = true,
    .width = 800,
    .height = 600
});
```

#### Reading Without Subscribing: `cel_get_state(Type)`
If you need to inspect state without registering a reactive dependency (e.g. inside `CEL_Evaluation` predicates or host loops):
```c
CEL_Evaluation(WindowEval) {
    const WindowState *state = cel_get_state(WindowState);
    return (state == NULL || state->isOpen);
}
```

#### Cross-Session & Host Communication: Signals, Not Multi-Session Mutate
> [!IMPORTANT]
> **The Actor Model Principle**: Sessions **only mutate their own state** via `cel_mutate(ptr)`.
> The host loop or external sessions must **never directly mutate** another session's state. To request a state change from the outside, dispatch a targeted signal via `cel_signal(targetSession, SignalType, ...)`. The target session handles the signal in its composition via `cel_connect()` and mutates its own state locally:
> ```c
> /* Host dispatch */
> cel_signal(cel_get_session(&engine, "main"), WindowActionSignal, { .action = WINDOW_ACTION_CLOSE });
> 
> /* Session composition handler */
> cel_connect(WindowActionSignal, sig) {
>     cel_mutate(win) {
>         this->isOpen = false;
>     }
> }
> ```

#### Tier 3 Low-Level Plumbing: `cels_session_mutate`
For raw test fixtures without composables:
```c
cels_session_mutate(&session, CEL_ID("FixtureState"), FixtureState) {
    this->value = 42;
}
```

> [!IMPORTANT]
> Never cast away `const` on pointers returned by `cel_watch` or `cel_get_state` to write directly. Always use `cel_mutate`. Direct writes bypass double-buffering, cause race conditions, and fail to notify subscribers.

---

## 2. Local Slot Memory: `cel_remember`

When a composable needs local variables that persist between frames (render counts, cached calculations, animation progress, local buffers, native handles), use `cel_remember`.

- **Signature**: `cel_remember(TypeName, initial_value, [onDestroy])`
- **Mechanism**: Allocates a slot in the session's slot table gap buffer. On first execution (mount), the initial value is stored. On subsequent recompositions, the existing slot memory is returned. When the composable leaves the tree, the optional `onDestroy(void *instance, CelsSession *session)` callback is executed during slot table reconciliation (pass `NULL` or omit if no cleanup is needed).
- **Return**: Mutable pointer `TypeName*`.

### Examples
```c
/* Simple primitive counter (no cleanup) */
int *renderCount = cel_remember(int, 0);
(*renderCount)++;

/* Complex struct initialization */
typedef struct CustomData {
    float opacity;
    const char *title;
} CustomData;

CustomData *data = cel_remember(CustomData, ((CustomData){
    .opacity = 1.0f,
    .title = "Default"
}));

/* Native resource handle with automatic cleanup on unmount */
static void OnTextureRelease(void *ptr, CelsSession *session CELS_UNUSED) {
    GLuint *tex = (GLuint *)ptr;
    glDeleteTextures(1, tex);
}

GLuint *texId = cel_remember(GLuint, 0, OnTextureRelease);
```

---

## 3. Lifecycle Hooks: `CEL_Lifecycle` & `cel_lifecycle`

CELS tracks component topology. When a composable appears in the call tree for the first time, it is **mounted**. When it ceases to be called (e.g. parent condition evaluates to `false`), it is **unmounted**.

Lifecycle hooks let you acquire resources on mount and cleanly release them on unmount.

### Defining a Lifecycle Hook
```c
CEL_Lifecycle(StatusBadgeLifecycle, BadgeData *badge) {
    mount {
        printf("Badge mounted: %s\n", badge->label);
    }
    unmount {
        printf("Badge unmounted: releasing resources\n");
    }
}
```

### Attaching the Hook inside a Composable
```c
CEL_Composable(CEL_StatusBadge) {
    BadgeData *badge = cel_remember(BadgeData, { .label = "Active" });
    cel_lifecycle(StatusBadgeLifecycle, badge);

    printf("Rendering badge: %s\n", badge->label);
}
```

### Conditional Rendering & Lifecycle Teardown
If the parent composable toggles the child off:
```c
if (state->showBadge) {
    CEL_StatusBadge();
}
```
1. When `showBadge` is `true`: `CEL_StatusBadge` is called. First time: `mount` executes.
2. When `showBadge` is switched to `false`: `CEL_StatusBadge` is skipped. The gap buffer detects the missing group and invokes the `unmount` block immediately during reconciliation.

### Hot-Reload Pointer Safety
In dynamic hot-reload mode (`CELS_HOT_RELOAD == 1`), code DLLs are recompiled and reloaded into memory. CELS automatically preserves the slot table while updating the `onDestroy` function pointers (`CelsSessionUpdateLifecycle`) so unmount cleanups never jump into unmapped memory addresses.

---

## 4. Core Patterns & Anti-Patterns (Best for LLMs)

### Pattern 1: Mutating Reactive State

```c
// WRONG: Mutating reactive state directly without staging
void OnButtonClicked(WindowState *win) {
    win->isOpen = false; // Error! Bypasses double buffer, creates race conditions, skips recomposition
}

// CORRECT: Mutate via cel_mutate block
void OnButtonClicked(WindowState *win) {
    cel_mutate(win) {
        this->isOpen = false; // Staged to back-buffer and commits atomically on next frame
    }
}
```

### Pattern 2: Subscribing to State

```c
// WRONG: Reading state in composables without cel_watch
CEL_Composable(MyLabel, const WindowState*, win) {
    // Missing cel_watch! UI will never re-render when win->width changes
    printf("Width: %d\n", win->width);
}

// CORRECT: Always declare cel_watch at the start of the composable
CEL_Composable(MyLabel, const WindowState*, win) {
    cel_watch(win); // Registers current slot group as dependent
    printf("Width: %d\n", win->width);
}
```

### Pattern 3: Resource Allocation & Teardown

```c
// WRONG: Allocating heap memory inside a composable without lifecycle tracking
CEL_Composable(TextureViewer) {
    // Memory leak! Executes on every recomposition pass!
    void *tex = malloc(4096);
}

// CORRECT: Remember once and attach cel_lifecycle for guaranteed unmount teardown
CEL_Lifecycle(TextureLifecycle, void *tex) {
    mount { /* acquired */ }
    unmount { free(tex); }
}

CEL_Composable(TextureViewer) {
    void *tex = cel_remember(void*, malloc(4096));
    cel_lifecycle(TextureLifecycle, tex);
}
```

---

## 5. Quick API Reference

| Macro / Function | Purpose | Typical Scope |
| :--- | :--- | :--- |
| `CEL_State(Name) { ... }` | Declares a reactive state struct | Header file (`.h`) |
| `cel_state(Type, [init])` | Allocates hoisted reactive state instance (Zero string IDs) | Parent composable / composition |
| `cel_watch(instancePtr)` | Subscribes to hoisted state instance | Child composable |
| `cel_mutate(instancePtr) { this->... }` | Mutates hoisted state instance directly (Sessions only mutate own state) | Host tick, input callback, task |
| `cel_remember_state(Type, init)` | Registers/retrieves double-buffered state by Type | Inside `CEL_Composition` or root composable |
| `cel_remember_state_keyed(id, Type, init)` | Registers/retrieves double-buffered state by ID | Inside `CEL_Composition` or root composable |
| `cel_get_state(Type)` | Reads state without subscribing (ambient session) | Inside `CEL_Evaluation` predicates |
| `cel_get_state(session, Type)` | Reads state without subscribing (explicit session) | Host checks, diagnostics |
| `cels_session_mutate(session, id, Type)` | Tier 3 Low-Level Plumbing: Stages mutation directly for raw test fixtures | Test fixtures, engine internals |
| `cel_remember(Type, init)` | Persistent local slot variable | Inside any `CEL_Composable` |
| `CEL_Lifecycle(Name, data) { mount {...} unmount {...} }` | Defines mount/unmount resource handlers | Header or source file |
| `cel_lifecycle(Name, data_ptr)` | Binds lifecycle handler to current composable | Inside `CEL_Composable` |

