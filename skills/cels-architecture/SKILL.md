---
name: cels-architecture
description: Provide technical guidance on CELS core architectural principles and mental models. Use when designing, decomposing, or structuring CELS applications into Sessions, Compositions, and Composables, managing lifecycle boundaries, or hoisting reactive state.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.4.0"
  last-updated: '2026-09-30'
  category: architecture
  keywords:
    - architecture
    - sessions
    - compositions
    - composables
    - slot-table
    - tree-hierarchy
    - decomposition
    - state-hoisting
    - lifecycle
    - reactive-ui
    - C99
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
|   |  Composition (CEL_Composition / cel_attach)                 | |
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

#### Session Memory & Workload Capacity Profiles (`CelsSessionProfile`)

Rather than forcing developers to calculate raw byte offsets and memory geometries, CELS provides **intent-driven capacity profiles**. Developers configure a session based on **how many composables they expect to use**:

| Profile | Capacity | Total Slab Size | Best Used For |
| :--- | :--- | :--- | :--- |
| `CELS_PROFILE_DEFAULT` | 4,096 composables | 512 KiB | Default standard capacity if unspecified |
| `CELS_PROFILE_128` | 128 composables | 16 KiB | Micro-dialogs, tiny popups, embedded widgets |
| `CELS_PROFILE_256` | 256 composables | 32 KiB | HUD overlays, tooltips, sub-panels |
| `CELS_PROFILE_512` | 512 composables | 64 KiB | L1/L2 cache-resident UI panels & focused subtrees |
| `CELS_PROFILE_1K` | 1,024 composables | 128 KiB | Standard application windows & forms |
| `CELS_PROFILE_2K` | 2,048 composables | 256 KiB | Complex screens with many active lists & controls |
| `CELS_PROFILE_4K` | 4,096 composables | 512 KiB | Broad applications with multiple concurrent modules |
| `CELS_PROFILE_8K` | 8,192 composables | 1 MiB | Heavy simulation trees & large hierarchies |

**Why Context-Neutral Profile Names?**  
CELS is a general-purpose reactive composition engine used not only for graphical user interfaces, but also for **audio DSP graphs**, **ECS simulation trees**, and **network state machines**. Numeric/capacity profile names (`CELS_PROFILE_512`, `CELS_PROFILE_1K`, etc.) describe the workload scale accurately without artificially constraining the domain.

**Intent-Driven Sizing APIs:**
```c
/* 1. Host Engine Initialization with Profile */
CelsEngine engine;
CelsEngineInitWithProfile(&engine, CELS_APP_TARGET, CELS_PROFILE_1K);

/* 2. Direct Session Initialization with Profile */
CelsSession session;
CelsSessionInitWithProfile(&session, CELS_PROFILE_512);

/* 3. Automatic Sizing from Estimated Composable Count */
CelsSessionInit(&session, &(CelsSessionConfig){
    .maxComposables = 350 /* Auto-selects 64 KiB slab / 512 groups */
});

/* 4. Keyed Heap Session Creation */
CEL_Session *s = CelSessionCreateWithProfile(CEL_ID("SubPanel"), CELS_PROFILE_256);
```

### Tier 2: Composition (`CEL_Composition` / `cel_attach`)
- **What it is**: A top-level reactive subtree attached directly to a `CelsSession`.
- **What it owns**:
  - A unique compile-time or runtime 64-bit key (auto-hashed from `#Name` or passed via `cel_attach_keyed`).
  - A root body function (`CEL_Composition(Name)` or `CEL_Composition(Name, Type*, arg)`).
  - An optional instance context pointer (`userData`).
  - An independent lifecycle predicate (`CEL_Evaluation(Name, void*, ctx)`).
- **Key Characteristics**:
  - The engine evaluates `lifecycleEval(evalCtx)` every tick.
  - If `lifecycleEval` returns `false`, the entire composition tree is torn down: all nested composables are unmounted, all slot memory for that subtree is wiped, and if it is the session's primary composition, the engine signals exit (`shouldQuit = true`).
  - Compositions represent standalone macro-entities (e.g., windows, root scenes, persistent background monitors).

#### Root Composition Signatures & `userData`

CELS supports two ways to declare a root composition:

```c
// 1. Zero-Parameter Signature (Standard & Recommended)
// Ideal when state is hoisted locally with cel_state() or accessed via ambient cel_user_data()
CEL_Composition(MainWindow) {
    WindowState *win = cel_state(WindowState, { .isOpen = true });
    WindowContent(win);
}

// 2. Injected Typed Parameter Signature
// Used when host or factory injects instance-specific configuration or hardware handles
CEL_Composition(ViewportView, ViewportConfig*, cfg) {
    RenderViewport(cfg->cameraIndex);
}
```

#### Ambient Session User Data vs. Composition Instance `userData`

CELS provides two distinct mechanisms for accessing external data:

1. **Ambient Session User Data (`cel_user_data`)**:
   - Pinned to the entire `CelsSession` via `CelsSessionSetUserData(session, ptr)`.
   - Accessible inside **any** Composable or Task via `EngineWorld *w = cel_user_data(EngineWorld);`.
   - Best for global singletons: Renderers, Loggers, Flecs Worlds, Audio Queues.

2. **Composition Instance User Data (`userData` parameter)**:
   - Pinned to a specific composition attachment via `cel_attach(session, Comp, Eval, userData)`.
   - Passed directly into the root composition function header.
   - Best for multi-instance configurations:
     - **Multi-Window / Multi-Viewport Instances**: Running the exact same composition function for multiple distinct viewports (e.g. Top, Front, Perspective cameras) with different configuration structs.
     - **Native OS / Hardware Handles**: Injecting host platform handles (Win32 `HWND`, GLFW window, Vulkan `VkDevice`) into the root composition upon launch.
     - **External ECS / Physics Worlds**: Passing external simulation contexts (`ecs_world_t*`, `b2WorldId`) into root compositions.
     - **Plugin Embeddings (VST / CLAP / Game Mods)**: When CELS is embedded in a digital audio workstation where the host provides an audio processor instance pointer.
     - **Unit Testing & Mocking**: Passing mock backends (simulated network lag, fake file systems) to verify UI flows under test conditions.

### Tier 3: Composable (`CEL_Composable` / `CEL_ComposableDef`)
- **What it is**: Reusable building blocks that declare UI elements, layout containers, or local reactive logic.
- **What it owns**:
  - A slot group in the session's slot table (automatically keyed by callsite source location and index).
  - Local persistent slot state via `cel_remember(Type, initialValue)`.
  - Hoisted reactive state instances via `cel_state(Type, initialValue)`.
  - Reactive subscriptions via `cel_watch(instancePtr)` (or `cel_watch(Type, stateId)`).
  - Resource acquisition and release hooks via `cel_lifecycle(LifecycleName, resourceData)`.
- **Key Characteristics**:
  - Idempotent and declarative: during recomposition, composables execute sequentially, matching previous slots or inserting/deleting slots via the internal gap buffer.
  - Composable functions can take typed arguments using `CEL_Composable(Name, Type, arg)` / `CEL_ComposableDef(Name, Type, arg)` or take no arguments using `CEL_Composable(Name)`.

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
  - Attached in `App_OnStart` via `cel_attach(session, WindowComposition, WindowEval)` with lifecycle evaluator `WindowEval`.

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

## 4. Core Patterns & Anti-Patterns (Best for LLMs)

### Pattern 1: Decomposing UI Nodes

```c
// WRONG: Declaring child widgets or buttons as top-level Compositions
CEL_Composition(SubmitButtonComp, void *ctx) {
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

### Pattern 2: Component-Local vs Keyed State

```c
// WRONG: Using session singleton state for reusable component-local data
CEL_Composable(CounterWidget) {
    // Bad! Collides across multiple instances of CounterWidget in the same session
    CounterState *s = cel_remember_state(CounterState, {0});
}

// CORRECT: Hoist state with cel_state (positional slot allocation, zero collisions)
CEL_Composable(CounterWidget) {
    // Distinct slot group allocated per callsite in the slot table
    CounterState *s = cel_state(CounterState, { .count = 0 });
    cel_watch(s);
    // Render counter...
}
```

### Pattern 3: Conditionally Mounting Child Elements

```c
// WRONG: Destroying the root composition to hide a child UI component
bool WindowEval(void *ctx) {
    WindowState *s = (WindowState*)ctx;
    return s->showBadge; // Bad! Closes entire window and exits app!
}

// CORRECT: Conditionally branch inside the composable body
CEL_Composable(Toolbar, const WindowState*, win) {
    cel_watch(win);
    RenderToolbarBase();
    if (win->showBadge) {
        // Slot reconciliation automatically mounts/unmounts Badge slots
        NotificationBadge();
    }
}
```

### Pattern 4: Cross-Session Actor Model (Signals vs Mutations)

```c
// WRONG: Mutating another session's state directly across session or thread boundaries
// cel_mutate(&otherSession, State) // Forbidden! Violates Actor boundary and thread safety.

// CORRECT: Send a targeted signal and let the target session mutate its own state locally
CelsSession *hudSession = cel_get_session(&engine, "hud");
cel_signal(hudSession, HealthSignal, { .delta = -25.0f });

// Inside hudSession's composition:
cel_connect(HealthSignal, sig) {
    cel_mutate(gauge) {
        this->currentHealth += sig->delta;
    }
}
```

---

## 5. Host Application Architecture & Multi-DLL Hosting

In CELS, the host executable (`cel_host`) controls all platform lifecycles and hosts one or more dynamic application modules (`cel_app`).

```
+=============================================================================+
| HOST EXECUTABLE (cel_host)                                                  |
| CelsEngine (Modules, Global Broadcast Bus, Ambient Context)                 |
|                                                                             |
|  +---------------------------+       +------------------------------------+ |
|  | CelsSession ("main")      |       | CelsSession ("editor")             | |
|  | Slab: 128 KiB             |       | Slab: 256 KiB                      | |
|  | CelsApp (game_app.dll)    |       | CelsApp (editor_tooling.dll)       | |
|  | - instanceId = 1          |       | - instanceId = 2                   | |
|  | - CEL_App_Def(GameApp)    |       | - CEL_App_Def(EditorApp)           | |
|  +---------------------------+       +------------------------------------+ |
+=============================================================================+
```

### Multi-DLL Hosting Pipeline (`CelsApp`)

Hosts can load multiple independent modules simultaneously into dedicated sessions:

```c
CelsEngine engine;
CelsEngineInitWithProfile(&engine, NULL, CELS_PROFILE_1K);

CelsSession *mainSession   = cel_get_session(&engine, "main");
CelsSession *editorSession = CelsEngineCreateSession(&engine, "editor", CELS_PROFILE_2K);

CelsApp gameApp;
CelsApp editorApp;

/* Load dynamic modules independently into target sessions */
CelsAppLoad(&gameApp, &engine, mainSession, "game_app");
CelsAppLoad(&editorApp, &engine, editorSession, "editor_tooling");

CelsAppStart(&gameApp);
CelsAppStart(&editorApp);

while (!engine.shouldQuit) {
    /* Hot-reload checks for both modules */
    CelsAppCheckReload(&gameApp);
    CelsAppCheckReload(&editorApp);

    if (CelsEngineNeedsRecompose(&engine)) {
        CelsEngineRecompose(&engine);
    }
    SleepMs(16);
}

CelsAppDestroy(&editorApp);
CelsAppDestroy(&gameApp);
CelsEngineDestroy(&engine);
```

- **Lock Evasion & Multi-Module Isolation**: Shadow copy files are stamped with `app->instanceId` (`app.hot_<PID>_<instanceId>_<reloadCount>.tmp.dll`), preventing OS file locks and shared handle ref-counts between parallel modules.
- **Selective Entry Symbols**: Multiple app definitions can reside in a single DLL and be loaded individually via `CelsAppLoadEntry(app, engine, session, dllPath, "CelsGetAppDef_SubApp")`.

### Host Execution Modes: Immediate vs. Retained

The host explicitly selects how recomposition is driven using `CelsEngineSetMode(&engine, mode)`:

| Mode | Flag | Frame Loop Strategy | Ideal Use Case |
| :--- | :--- | :--- | :--- |
| **Immediate Mode** | `CELS_MODE_IMMEDIATE` *(default)* | Recomposes every frame tick unconditionally | Real-time games, physics simulations, 60+ FPS rendering loops |
| **Retained Mode** | `CELS_MODE_RETAINED` | Recomposes only when `CelsEngineNeedsRecompose` returns `true` | Native desktop GUIs, toolbars, dialogs, battery-saving apps |

In Retained Mode, `CelsEngineNeedsRecompose(&engine)` evaluates:
1. Primary or secondary sessions have uncomposed root trees.
2. Watched reactive state cells have been invalidated via `cel_mutate`.
3. Discrete tree events (`cel_event`), signals (`cel_signal`), or global broadcasts (`cel_broadcast`) are queued.
4. Active fiber tasks (`CEL_Task`) have expired delays or are ready to resume.
5. Code hot-reload is pending (`CelsSessionHotReload`).

---

### Summary Cheat Sheet

| Requirement | Entity Type | API Definition | Example |
| :--- | :--- | :--- | :--- |
| Standalone OS Window / Root Scene | **Composition** | `CEL_Composition(Name)` | `cel_attach(session, WindowComposition, WindowEval)` |
| Reusable Widget / Subtree | **Composable** | `CEL_Composable(Name, ...)` | `ProfileCard(user)` |
| Dedicated Real-time Thread | **Session** | `CelsSession* session` | Audio thread session |
| Dynamic Hosted Application | **Host App Handle** | `CelsApp app` | `CelsAppLoad(&app, &engine, session, "game_app")` |
| Persistent Local Value | **Slot Value** | `cel_remember(Type, init)` | `uint32_t *frame = cel_remember(uint32_t, 0)` |
| Scoped Reactive Model | **Hoisted State** | `cel_state(Type, { ... })` | `WindowState *win = cel_state(...)` |
| Local Tree Event | **Discrete Event** | `cel_event(Type, ...)` / `cel_listen(Type, ev)` | `cel_event(ClickEvent, { .id = 1 })` |
| Cross-Session Signal | **Directed Signal** | `cel_signal(session, Type, ...)` / `cel_connect(Type, sig)` | `cel_signal(hudSession, DamageSignal, { .dmg = 10 })` |
| Engine-Wide Broadcast | **Global Bus** | `cel_broadcast(Type, ...)` / `cel_bind(Type, bcast)` | `cel_broadcast(SoundBroadcast, { .sfx = "hit.wav" })` |
| Fiber Task Wait | **Async Coroutine Wait** | `cel_wait_for` / `cel_wait_for_timeout` | `cel_wait_for(ClickEvent, &ev)` |
