---
name: cels-composition-and-state
description: Provide technical guidance on CELS declarative compositions, composable hierarchies, reactive state management, slot table memory, lifecycle hooks, and visual transitions. Use when building UI trees, hoisting reactive state (cel_state, cel_watch, cel_mutate), managing local persistent slots (cel_remember), handling mount/unmount lifecycles, or applying continuous visual easing (cel_transition).
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.5.0"
  last-updated: '2026-10-07'
  category: composition-and-state
  keywords:
    - composition
    - composable
    - reactive-state
    - state-hoisting
    - cel_state
    - cel_watch
    - cel_mutate
    - cel_remember
    - cel_lifecycle
    - cel_transition
    - slot-table
    - C99
---

# CELS Declarative Composition, Reactive State & Lifecycles Guide

CELS provides a declarative, double-buffered reactive state system and slot-table memory model in pure ANSI C99. Inspired by Jetpack Compose and modern reactive architectures, CELS provides automatic recomposition, local state hoisting, zero-allocation slot memory, and hot-reloadability without requiring a C++ runtime or garbage collection.

---

## 1. The Three-Tier Architecture & Mental Model

Every CELS application organizes its execution and memory into a strict three-tier hierarchy:

```
+-------------------------------------------------------------------+
|  Session (CelsSession)                                            |
|  - Independent tick loop & memory boundary                        |
|  - Owns SlotTable, GapBuffer, StateRegistry, Arena                |
|                                                                   |
|   +-------------------------------------------------------------+ |
|   |  Composition (CEL_Composition / cel_attach)                 | |
|   |  - Top-level subtree root & lifecycle arbiter               | |
|   |  - Has unique 64-bit key and CEL_Evaluation predicate       | |
|   |                                                             | |
|   |   +-------------------------------------------------------+ | |
|   |   |  Composable (CEL_Composable)                          | | |
|   |   |  - Reusable visual / logical component node           | | |
|   |   |  - Uses cel_state, cel_watch, cel_mutate              | | |
|   |   |  - Uses cel_remember (slots), cel_lifecycle           | | |
|   |   |  - Declares child composables & cel_transition        | | |
|   |   +-------------------------------------------------------+ | |
|   +-------------------------------------------------------------+ |
+-------------------------------------------------------------------+
```

### Tier Comparison & Roles:

| Tier | Primary Purpose | Number per App | Lifecycle Control | Memory Boundary |
| :--- | :--- | :--- | :--- | :--- |
| **Session** (`CelsSession`) | Isolated execution & memory arena | Usually 1 (can be 2-3 for background/worker threads) | Owned by Host `CelsEngine` (`CelsEngineInit` / `End`) | Owns physical `SlotTable` slab and double-buffered state registry |
| **Composition** (`CEL_Composition`) | Root lifecycle arbiter for an entire visual tree | 1 to several (main window, inspector, modal) | Evaluated dynamically every tick via `CEL_Evaluation` | Starts a root slot group in the session slab |
| **Composable** (`CEL_Composable`) | Reusable declarative UI or logic component | Tens to thousands (cards, buttons, rows, dialogs) | Mounts/unmounts conditionally inside parent composables | Owns a sub-group of memory slots (`cel_remember`) |

### Decomposition Decision Framework:

```
                      [ Need to add a new entity or feature ]
                                        │
                Does it require an independent OS thread, a distinct tick
                rate, or complete memory isolation from other trees?
                                 /              \
                              YES                NO
                              /                    \
                   [ Create a new SESSION ]         │
                                                    │
                   Is it a top-level root entity (e.g. a native OS window,
                   root scene) whose teardown determines the life of that entire tree?
                                 /              \
                              YES                NO
                              /                    \
                 [ Create a COMPOSITION ]           │
                                                    │
                                      Does it represent a reusable widget,
                                      container, layout, or stateful node?
                                                    │
                                          [ Create a COMPOSABLE ]
```

---

## 2. Authoring Compositions & Composables

### Compositions (`CEL_Composition` / `cel_attach`)
A composition represents a standalone root tree attached to a session. It has an evaluation predicate (`CEL_Evaluation`) determining when it remains active:

```c
/* Lifecycle Evaluation Predicate: Returns false to tear down the composition */
CEL_Evaluation(WindowEval) {
    const WindowState *win = cel_get_state(WindowState);
    return (win == NULL || win->isOpen);
}

/* Zero-Argument Root Composition */
CEL_Composition(MainWindowComposition) {
    WindowState *win = cel_state(WindowState, { .isOpen = true, .title = "Editor" });
    WindowToolbar(win);
    WindowCanvas(win);
}

// In Application onStart:
static void App_OnStart(CelsSession *session) {
    cel_attach(session, MainWindowComposition, WindowEval);
}
```

### Composables (`CEL_Composable`)
Composables are the universal building blocks in CELS. They accept typed parameters, subscribe to state, render children, and maintain local slot variables:

```c
CEL_Composable(WindowToolbar, WindowState*, win) {
    cel_watch(win);

    RenderHeader(win->title);

    if (win->showSettings) {
        SettingsModal(win); /* Conditionally mounted child */
    }
}
```

---

## 3. Reactive State Management & State Hoisting

CELS provides **double-buffered reactive state**. The front buffer holds the committed read values for the current frame, while mutations are staged into the back buffer and committed atomically at frame boundaries.

### Defining State Structs
Define state types using `CEL_State` in a shared header:

```c
#pragma once
#include "cels.h"

CEL_State(CounterState) {
    int count;
    bool isEven;
};
```

### Primary Pattern: State Hoisting (Zero String IDs)

#### 1. Hoisting State with `cel_state`
Inside a parent composition or container composable, allocate state with `cel_state`. It is pinned to that slot address with an auto-generated unique 64-bit ID and cleaned up on unmount:

```c
CEL_Composition(CounterScreen) {
    // Hoisted state instance:
    CounterState *counter = cel_state(CounterState, { .count = 0, .isEven = true });

    // Pass down to child composables (State Hoisting)
    CounterDisplay(counter);
    CounterButtons(counter);
}
```

#### 2. Subscribing to State with `cel_watch`
In child composables, pass the pointer to `cel_watch(ptr)`. In $\mathcal{O}(1)$, CELS reads the preceding `CelsStateHeader` and subscribes the active composable group to changes:

```c
CEL_Composable(CounterDisplay, const CounterState*, counter) {
    cel_watch(counter); // Subscribes this composable group to 'counter'

    printf("Count: %d (%s)\n", counter->count, counter->isEven ? "Even" : "Odd");
}
```

#### 3. Mutating State with `cel_mutate`
Mutate reactive state using `cel_mutate(ptr)`. Mutations are staged to the back-buffer and commit atomically before the next recomposition pass:

```c
CEL_Composable(CounterButtons, CounterState*, counter) {
    if (PollIncrementButton()) {
        cel_mutate(counter) {
            this->count++;
            this->isEven = (this->count % 2 == 0);
        }
    }
}
```

> [!IMPORTANT]
> **The Actor Model Principle**: Sessions **only mutate their own state** via `cel_mutate(ptr)`.
> The host loop or external sessions must **never directly mutate** another session's state. To request a state change from the outside, dispatch a targeted signal via `cel_signal(targetSession, SignalType, ...)`. The target session handles the signal in its composition via `cel_connect()` and mutates its own state locally.

---

## 4. Session State & Type Registries

For global session singletons or cross-session addressability:

### `cel_remember_state(Type, init)`
Registers or retrieves double-buffered state keyed automatically by compile-time type hash:
```c
WindowState *win = cel_remember_state(WindowState, {
    .isOpen = true,
    .title = "Main Window"
});
```

### `cel_get_state(Type)`
Reads session state without registering a reactive dependency (ideal inside `CEL_Evaluation` predicates or diagnostics):
```c
CEL_Evaluation(WindowEval) {
    const WindowState *win = cel_get_state(WindowState);
    return (win == NULL || win->isOpen);
}
```

---

## 5. Persistent Local Slot Memory: `cel_remember`

When a composable needs local variables that persist between frames (counters, cached calculations, animation progress, native handles), use `cel_remember`:

- **Signature**: `cel_remember(TypeName, initial_value, [onDestroy])`
- **Mechanism**: Allocates a slot in the session's slot table gap buffer. On first execution (mount), the initial value is stored. On subsequent recompositions, the existing slot memory is returned. When the composable leaves the tree, the optional `onDestroy(void *instance, CelsSession *session)` callback runs.
- **Return**: Mutable pointer `TypeName*`.

```c
/* Primitive counter retained across frames */
int *renderCount = cel_remember(int, 0);
(*renderCount)++;

/* Complex struct initialization */
typedef struct FilterCache {
    uint32_t lastHash;
    int filteredCount;
} FilterCache;

FilterCache *cache = cel_remember(FilterCache, { .lastHash = 0, .filteredCount = 0 });

/* Native resource handle with automatic cleanup on unmount */
static void OnTextureRelease(void *ptr, CelsSession *session CELS_UNUSED) {
    GLuint *tex = (GLuint *)ptr;
    glDeleteTextures(1, tex);
}

GLuint *texId = cel_remember(GLuint, 0, OnTextureRelease);
```

---

## 6. Lifecycle & Execution Model: Inline Hooks & `CEL_Lifecycle`

### Architectural Mental Model: Code Execution Order vs. Presence in the Tree
Instead of visualizing lifecycles around abstract timer ticks or 60fps frames, CELS lifecycles follow **Code-to-Code Execution**:

```
[Presence in Tree: Lifetime]
  on_attach (Fresh Mount) ───────────────► on_detach (Omitted / Pruned)
      │                                            ▲
      ▼                                            │
[Code Execution Order: Every Evaluation Pass]      │
  Scope Entry (Composable body / enter)           │
      │                                            │
      ├─── Sets ambient context (cel_set_context)  │
      ├─── Attaches components (cel_has)          │
      └─── Allocates / reads state (cel_state)     │
      │                                            │
      ▼                                            │
  Children Execute ({ ... } child composables)     │
      │                                            │
      ▼                                            │
  Scope Exit (on_exit(childCount) at closing }) ───┘
```

1. **Code Execution Order (Every Recomposition Pass)**:
   - **Scope Entry** (Composable body / `enter { ... }`): Executes *before* any children run. Sets up ambient context, attaches ECS components, and reads reactive state.
   - **Children**: Direct child composables execute sequentially within the group scope.
   - **Scope Exit** (`on_exit(childCount) { ... }`): Executes at the closing `}` of the container scope (or when the composable exits). Receives the exact settled `childCount` of direct children (ideal for focus clamping, layout bounding, and grid wrapping).
2. **Presence in the Tree (Topology Lifetime)**:
   - **`on_attach`**: Executes once on the first pass when the composable is freshly mounted into the session hierarchy.
   - **`on_detach`**: Executes when the composable leaves the hierarchy (parent condition evaluates to `false`, or root teardown).

---

### Primary Pattern: Inline Hooks inside `CEL_Composable`
You can declare lifecycle hooks directly inside `CEL_Composable`:

```c
CEL_Composable(CelsColumn) {
    on_attach {
        // Runs once when this column is first mounted
        printf("Column mounted\n");
    }

    // Composable Body (Scope Entry): runs before children execute
    CelsFocusState *focus = cel_remember_state(CelsFocusState, {0});
    cel_set_context(CelsFocusState, focus);

    on_exit(childCount) {
        // Scope Exit: runs after children have executed
        focus->totalCount = (int)childCount;
    }

    on_detach {
        // Runs when this column is removed from the hierarchy
        printf("Column unmounted\n");
    }
}
```

### Secondary Pattern: Modular External Controllers (`CEL_Lifecycle`)
For reusable lifecycle logic shared across multiple composables or external subsystem handles, define a standalone `CEL_Lifecycle`:

```c
CEL_Lifecycle(DialogLifecycle, DialogState *dialog) {
    mount {
        printf("Dialog mounted: %s\n", dialog->title);
    }
    before_children {
        // Runs before children: establish ambient theme
        cel_set_context(UITheme, &DarkTheme);
    }
    after_children(childCount) {
        // Runs after children: record exact rendered child count
        dialog->renderedItems = childCount;
    }
    unmount {
        printf("Dialog unmounted: releasing textures\n");
        FreeDialogTextures(dialog);
    }
}

CEL_Composable(CEL_Dialog, DialogState*, dialog) {
    cel_lifecycle(DialogLifecycle, dialog);

    DialogHeader(dialog);
    DialogContent(dialog);
}
```

### Container Composables & Natural Block Calling
In CELS, container composables allow developers to write declarative trailing `{ ... }` blocks naturally:

```c
// Direct invocation with clean Compose-like syntax!
CEL_Composable(SettingsPanel) {
    Column() {
        Slider("Volume");
        Toggle("Fullscreen");
    }
}
```

#### How to Author a Custom Container Composable (e.g. `LazyColumn`)
Container authors write a standard `CEL_Composable` with components (`cel_has`) and inline `on_exit(childCount)`, then add a 1-line alias using `cel_scope`:

```c
// 1. Declare container logic as a standard composable with inline on_exit
CEL_Composable(LazyColumn, int, count) {
    cel_has(LazyColumnConfig, { .count = count });

    on_exit(childCount) {
        printf("LazyColumn rendered %u items\n", childCount);
    }
}

// 2. Export 1-line macro enabling trailing block syntax
#define LazyColumn(...) cel_scope(LazyColumn, ##__VA_ARGS__)

// 3. Application developers call it naturally:
LazyColumn(10) {
    Item(0);
    Item(1);
    Item(2);
}
```

### Conditional Rendering & Automatic Unmount Teardown
If the parent composable toggles the child off:
```c
if (state->showBadge) {
    CEL_StatusBadge();
}
```
1. When `state->showBadge` is `true`: `CEL_StatusBadge` is called. On first call, `mount` executes.
2. When `state->showBadge` becomes `false`: `CEL_StatusBadge` is omitted. The gap buffer detects the missing slot group and immediately invokes `unmount` during reconciliation!

---

## 7. Declarative Visual Transitions (`cel_transition`)

### Core Concept: Continuous Mathematical Convergence
In CELS, game and UI state remains **authoritative and instantaneous**. You do not animate gameplay data; you declare how the visual projection smoothly converges toward that gameplay data over time.

```c
CEL_Composable(HealthBarHUD, const PlayerState*, player) {
    cel_watch(player);
    if (!player) return;

    /* Smoothly eases visual display toward authoritative player->health over 400ms */
    float visualHealth = cel_transition(player->health, 400, CEL_EASE_OUT_QUAD);

    RenderProgressBar(visualHealth / player->maxHealth);
}
```

### Behavior & Guarantees:
1. **Immediate Retargeting**: When `player->health` drops instantly from `100.0` to `40.0`, `visualHealth` begins easing toward `40.0`. If hit again mid-flight at $t = 150\text{ms}$ (dropping to `10.0`), `cel_transition` immediately catches the current interpolated position and retargets toward `10.0` with **zero visual pops or teleports**.
2. **Zero Heap Allocations**: Transition state (`CelsTransitionState`) is stored directly in the active composable's slot table (`cel_remember`).
3. **0% CPU Idle**: While running, the transition marks the composable group as dirty, causing continuous re-evaluation. Once settled at `targetValue`, dirty flags cease and the engine drops to **0% CPU recompositions**.
4. **Automatic Reclamation**: When the parent composable unmounts, transition memory is freed automatically.

### Built-in Easing Curves (`CelsEaseFn`)
| Easing Identifier | Curve Description | Best Used For |
| :--- | :--- | :--- |
| `CEL_EASE_LINEAR` | Constant velocity ($f(t) = t$) | Progress bars, linear timers |
| `CEL_EASE_IN_QUAD` | Accelerating from zero ($f(t) = t^2$) | Exiting dialogs, falling elements |
| `CEL_EASE_OUT_QUAD` | Decelerating to zero ($f(t) = 1 - (1-t)^2$) | Entering UI, HUD gauges, camera snapping |
| `CEL_EASE_IN_OUT_QUAD`| Accelerating then decelerating | Layout item reordering |
| `CEL_EASE_OUT_BOUNCE`| Decelerates and bounces off target | Loot drops, alerts, playful UI |
| `CEL_EASE_OUT_ELASTIC`| Overshoots with decaying harmonic oscillation | Switches, toggles, badges |

---

## 8. Core Patterns & Anti-Patterns (Best for LLMs)

### Pattern 1: Decomposing UI Nodes
```c
// WRONG: Declaring child widgets or buttons as top-level Compositions
CEL_Composition(SubmitButtonComp) {
    // Bad! Bypasses parent slot nesting and creates an isolated root entity
}

// CORRECT: Compose widgets as reusable Composables inside a parent Composition
CEL_Composable(SubmitButton, const FormState*, state) {
    cel_watch(state);
    if (!state->canSubmit) return;
    RenderButton("Submit");
}

CEL_Composition(MyWindowComposition) {
    FormState *form = cel_state(FormState, { .canSubmit = true });
    SubmitButton(form);
}
```

### Pattern 2: Mutating Reactive State
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

### Pattern 3: Subscribing to State
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

### Pattern 4: Resource Allocation & Teardown
```c
// WRONG: Allocating heap memory inside a composable without lifecycle tracking
CEL_Composable(TextureViewer) {
    void *tex = malloc(4096); // Memory leak! Executes on every recomposition pass!
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

### Pattern 5: The 3-Way Rule: Props vs. State vs. Context
| Mechanism | Where it Lives | Primary Purpose | How Updates Happen |
| :--- | :--- | :--- | :--- |
| **Props (Arguments)** | Function parameters | Inputs for an immediate child widget (label, width). | Parent re-evaluates and passes new values. |
| **State (`cel_state`)** | Double-buffered slot memory | Dynamic data that changes over time and triggers recomposition. | Staged through `cel_mutate(ptr)`. |
| **Context (`cel_context`)** | Ambient call stack | Passes ambient environment (themes, input) down subtrees without parameter drilling. | Non-reactive courier; restored on composable return. |

---

## 9. Quick API Reference

| Macro / Function | Purpose | Typical Scope |
| :--- | :--- | :--- |
| `CEL_Composition(Name)` | Declares a root reactive subtree | Header or source file |
| `CEL_Evaluation(Name)` | Evaluates whether a composition stays alive | Header or source file |
| `cel_attach(session, Comp, [Eval])` | Attaches root composition to a session | Application `onStart` |
| `CEL_Composable(Name, ...)` | Declares a reusable composable component | Header or source file |
| `CEL_State(Name) { ... }` | Declares a reactive state struct | Shared header file |
| `cel_state(Type, [init])` | Allocates hoisted reactive state instance | Parent composable / composition |
| `cel_watch(ptr)` | Subscribes current composable to state instance | Child composable body |
| `cel_mutate(ptr) { this->... }` | Stages atomic mutation to state | Event callback, host tick, task |
| `cel_remember_state(Type, init)` | Registers/retrieves session state by Type | Inside `CEL_Composition` |
| `cel_get_state(Type)` | Reads state without subscribing | Inside `CEL_Evaluation` predicates |
| `cel_remember(Type, init, [destructor])`| Persistent local slot variable in gap buffer | Inside any `CEL_Composable` |
| `CEL_Lifecycle(Name, ptr)` | Defines mount/unmount handlers | Header or source file |
| `cel_lifecycle(Name, ptr)` | Binds lifecycle handler to current composable | Inside `CEL_Composable` |
| `cel_transition(target, ms, ease)` | Smoothly interpolates visual scalar over time | Inside `CEL_Composable` |
