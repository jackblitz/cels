---
name: cels-architecture
description: "Core architectural principles and mental models for CELS. Use when designing, decomposing, or structuring CELS applications into Sessions, Compositions, and Composables."
---

# CELS Architectural Guide: Sessions, Compositions & Composables

CELS is a declarative, reactive framework for pure C99. Inspired by slot-table architectures (similar to Jetpack Compose) and double-buffered reactive systems, CELS provides declarative state management, automatic recomposition, and hot-reloadability without requiring a C++ runtime or external scripting language.

When designing or reading any CELS application, code is organized into a strict three-tier hierarchy:

```
+-------------------------------------------------------------------+
|  Session (CelsSession)                                            |
|  - Independent tick loop & memory boundary                        |
|  - Owns SlotTable, GapBuffer, StateRegistry, Arena                |
|                                                                   |
|   +-------------------------------------------------------------+ |
|   |  Composition (CelsCompositionRef / CEL_Composition)         | |
|   |  - Top-level subtree root & lifecycle arbiter               | |
|   |  - Has unique 64-bit key and CEL_Evaluation predicate       | |
|   |                                                             | |
|   |   +-------------------------------------------------------+ | |
|   |   |  Composable (CEL_Composable / CEL_ComposableDef)       | | |
|   |   |  - Reusable visual / logical component node           | | |
|   |   |  - Uses cel_remember (slots), cel_watch (reactivity)   | | |
|   |   |  - Declares child composables & cel_lifecycle hooks   | | |
|   |   +-------------------------------------------------------+ | |
|   +-------------------------------------------------------------+ |
+-------------------------------------------------------------------+
```

---

## 1. The Three Tiers Explained

### Tier 1: Session (`CelsSession`)
- **What it is**: An isolated runtime universe and memory boundary.
- **What it owns**:
  - `SlotTable` (gap buffer storing persistent slot groups and values across frames).
  - `StateRegistry` (double-buffered reactive state dictionary).
  - `CelsArena` (`dataArena` for linear frame/group scratch memory).
  - List of attached root `Compositions`.
- **Key Characteristics**:
  - Single-threaded execution per session.
  - Recomposed via `CelsSessionRecompose(session)`.
  - Maintains state and slot memory across hot-reload swaps.

### Tier 2: Composition (`CEL_Composition` / `CelsCompositionRef`)
- **What it is**: A top-level reactive subtree attached directly to a `CelsSession`.
- **What it owns**:
  - A unique compile-time or runtime 64-bit key (e.g. `CEL_Window` via `CEL_ID("CEL_Window")`).
  - A root body function (`CEL_Composition(Name, void *userData)`).
  - An independent lifecycle predicate (`CEL_Evaluation(Name, void*, ctx)`).
- **Key Characteristics**:
  - The engine evaluates `lifecycleEval(evalCtx)` every tick.
  - If `lifecycleEval` returns `false`, the entire composition tree is torn down: all nested composables are unmounted, all slot memory for that subtree is wiped, and if it is the session's primary composition, the engine signals exit (`shouldQuit = true`).
  - Compositions represent standalone macro-entities (e.g., windows, root scenes, persistent background monitors).

### Tier 3: Composable (`CEL_Composable` / `CEL_ComposableDef`)
- **What it is**: Reusable building blocks that declare UI elements, layout containers, or local reactive logic.
- **What it owns**:
  - A slot group in the session's slot table (automatically keyed by callsite source location and index).
  - Local persistent slot state via `cel_remember(Type, initialValue)`.
  - Reactive subscriptions via `cel_watch(Type, stateId)`.
  - Resource acquisition and release hooks via `cel_lifecycle(LifecycleName, resourceData)`.
- **Key Characteristics**:
  - Idempotent and declarative: during recomposition, composables execute sequentially, matching previous slots or inserting/deleting slots via the internal gap buffer.
  - Composable functions can take typed arguments using `CEL_ComposableDef(Name, Type, arg)` or take no arguments using `CEL_Composable(Name)`.

---

## 2. Decomposition Decision Framework

When implementing a feature or breaking down a problem in CELS, use this decision tree to determine which tier to create:

```
                            [ Need to add a new entity or feature ]
                                              │
                      Does it require an independent thread, a distinct tick
                      rate, or complete memory isolation from other trees?
                                       /              \
                                    YES                NO
                                    /                    \
                         [ Create a new SESSION ]         │
                                                          │
                         Is it a top-level root entity (e.g. a native OS window,
                         modal dialog manager, or root scene) whose teardown
                         determines the life of that entire visual tree?
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

### Detailed Decision Criteria

| Criteria | Session | Composition | Composable |
| :--- | :--- | :--- | :--- |
| **Primary Purpose** | Execution isolation & memory arena boundary | Root lifecycle management for a tree | Reusable declarative UI or logic component |
| **Number per App** | Usually 1 (can be 2-3 for background/worker threads) | 1 to several (e.g. main window, inspector window) | Tens to thousands (buttons, rows, cards, panels) |
| **Lifecycle Control** | Managed by Host `CelsEngine` (`CelsEngineInit`/`End`) | Evaluated dynamically every tick via `CEL_Evaluation` | Mounts and unmounts conditionally within parent composable |
| **Slot Memory** | Owns the physical `SlotTable` buffer | Starts a root slot group in the session | Owns a sub-group of memory slots (`cel_remember`) |
| **Reactivity** | Orchestrates double-buffer commit and recompose | Recomposes when watched states or inputs change | Re-executes during recomposition; subscribes via `cel_watch` |
| **Thread Safety** | Must be confined to a single thread | Confined to the session's thread | Confined to the session's thread |

---

## 3. Concrete Scenario Walkthroughs

### Scenario A: A Native Desktop Window
- **Decision**: **Composition**.
- **Rationale**:
  - A window has a root visual hierarchy.
  - A window has a distinct lifecycle predicate: if `state->isOpen == false`, the window should close and tear down its entire subtree.
  - Defined with `Window_GetComposition()` returning a `CelsCompositionRef` with `.lifecycleEval = WindowEval`.

### Scenario B: A Toggleable Status Badge / Tooltip / Button
- **Decision**: **Composable**.
- **Rationale**:
  - Lives inside a window or container.
  - Doesn't need its own tick loop or independent lifecycle evaluation predicate.
  - Rendered conditionally inside its parent composable:
    ```c
    if (state->showBadge) {
        CEL_StatusBadge();
    }
    ```
  - When `state->showBadge` becomes `false`, the slot table detects the omitted child group and automatically triggers the badge's `unmount` lifecycle hook!

### Scenario C: A Multi-Threaded Audio Synthesizer or Physics Simulation
- **Decision**: **Session**.
- **Rationale**:
  - Audio runs at high frequency (e.g. 512 samples buffer callback) on a dedicated realtime OS thread.
  - Running audio inside the main UI session would cause lock contention or audio glitches.
  - An independent `CelsSession` on the audio thread owns its own reactive state and slot table, communicating with the UI session via lock-free state queues.

---

## 4. Architectural Rules & Anti-Patterns

### Rules to Follow
1. **Single Responsibility per Composable**: Keep composables small, modular, and focused on one visual or logical unit.
2. **Push State Up, Pass Down or Watch**: Shared state should be registered in the session (`cel_remember_state`), observed by descendants with `cel_watch(Type, id)`, and mutated via `cel_mutate`.
3. **Decouple Child Lifecycles**: Do not tie child visibility or teardown to root destruction. If a child component (like a badge or modal) should close, use a boolean field in state (`state->showBadge = false`) and conditionally invoke the child composable.
4. **Use `CEL_ID` Macros**: Always define `#define CEL_MyEntity CEL_ID("CEL_MyEntity")` in the entity's header so callers can reference `CEL_MyEntity` directly.

### Anti-Patterns to Avoid
- ❌ **Anti-Pattern: Creating a Session for a dialog or popup**.
  * *Why*: Creating a new session allocates a separate arena and slot table. Popups and dialogs belong to the same UI loop and should be composables or secondary compositions within the existing session.
- ❌ **Anti-Pattern: Making every widget a Composition**.
  * *Why*: Compositions are top-level roots registered with the engine. Having 50 compositions for buttons and text labels ruins tree hierarchy and bypasses child slot management. Widgets are always **Composables**.
- ❌ **Anti-Pattern: Storing heap pointers inside `cel_remember` without lifecycle cleanup**.
  * *Why*: Memory remembered with `cel_remember` stays alive across recompositions. If you allocate external resources (file handles, textures, sockets), pair them with `cel_lifecycle` so they are freed on `unmount`.
