---
name: cels-reactive-state
description: "Reactive state management, slot table memory, and lifecycle hooks in CELS. Use when implementing stateful composables, observing/mutating reactive state, or managing resource lifecycles."
---

# CELS Reactive State, Slot Memory & Lifecycle Guide

CELS implements a declarative, double-buffered reactive state system and slot-table memory model in pure C99. This guide details how to declare state, observe changes, stage mutations, retain persistent slot memory, and bind resource lifecycles.

---

## 1. Double-Buffered State Management

Shared state in CELS is registered with a 64-bit unique ID and maintained across frames using double buffering (front buffer = current committed read state, back buffer = staged write state).

### Defining State Structs
Define state types using the `CEL_State` macro in a shared header:

```c
#pragma once
#include "cels.h"

#define CEL_Window CEL_ID("CEL_Window")

CEL_State(WindowState) {
    bool isOpen;
    bool showBadge;
    int  width;
    int  height;
    void *nativeHandle;
};
```

### Initializing / Registering State: `cel_remember_state`
Use `cel_remember_state` inside a composition or composable to register initial state on first mount. On subsequent frames, it retrieves the existing state:

```c
WindowState *win = cel_remember_state(CEL_Window, WindowState, ((WindowState){
    .isOpen = true,
    .showBadge = true,
    .width  = 800,
    .height = 600,
    .nativeHandle = NULL
}));
```

### Observing State: `cel_watch`
To make a composable reactively re-render whenever a state changes, observe it using `cel_watch`:
- **Signature**: `cel_watch(TypeName, stateId)` (Type first, matching `cel_remember`)
- **Return**: `const TypeName*` (read-only pointer to front buffer)
- **Behavior**: Registers the current composable group as a subscriber. When this state is mutated, the composable will be re-executed during the next recomposition cycle.

```c
const WindowState *win = cel_watch(WindowState, CEL_Window);
if (win != NULL) {
    printf("Window dimensions: %d x %d\n", win->width, win->height);
}
```

### Reading State Without Subscribing: `cel_get_state`
If you need to inspect state without registering a reactive dependency (e.g. inside a lifecycle evaluation predicate like `CEL_Evaluation`), use `cel_get_state`:

```c
CEL_Evaluation(WindowEval, void*, ctx) {
    (void)ctx;
    const WindowState *state = cel_get_state(CEL_Window, WindowState);
    return (state == NULL || state->isOpen);
}
```

### Mutating State: `cel_mutate`
Mutations in CELS are staged into the back buffer and only committed to the front buffer at the start of the next recompose cycle.

Use the `cel_mutate` block macro:
- **Signature**: `cel_mutate(session_ptr, stateId, TypeName) { this->field = value; }`
- **Context variable**: Inside the block, `this` is a typed pointer to the back buffer.
- **Commit**: The engine automatically marks the state dirty and swaps the buffers on tick.

```c
/* Mutating state from an input event or host tick */
cel_mutate(&engine.session, CEL_Window, WindowState) {
    this->showBadge = !this->showBadge;
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
static void OnTextureRelease(void *ptr, CelsSession *session) {
    (void)session;
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

## 4. Quick API Reference

| Macro / Function | Purpose | Typical Scope |
| :--- | :--- | :--- |
| `CEL_State(Name) { ... }` | Declares a reactive state struct | Header file (`.h`) |
| `cel_remember_state(id, Type, init)` | Registers/retrieves double-buffered state | Inside `CEL_Composition` or root composable |
| `cel_watch(Type, id)` | Observes state & registers reactive subscription | Inside any `CEL_Composable` |
| `cel_get_state(id, Type)` | Reads state without subscribing | Inside `CEL_Evaluation` predicates |
| `cel_mutate(session, id, Type) { this->... }` | Stages a state modification | Host event handler, input callback, tick loop |
| `cel_remember(Type, init)` | Persistent local slot variable | Inside any `CEL_Composable` |
| `CEL_Lifecycle(Name, data) { mount {...} unmount {...} }` | Defines mount/unmount resource handlers | Header or source file |
| `cel_lifecycle(Name, data_ptr)` | Binds lifecycle handler to current composable | Inside `CEL_Composable` |
