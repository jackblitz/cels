<p align="center">
  <h1 align="center">CELS</h1>
  <p align="center"><strong>C99 macros that mean what they say.</strong></p>
  <p align="center">Composition, Evaluation, Lifecycle, and State — A reactive composition engine for pure C99.</p>
  <p align="center">
    <img src="https://img.shields.io/badge/version-v0.2.0-blue" alt="version">
    <img src="https://img.shields.io/badge/license-Apache%202.0-green" alt="license">
    <img src="https://img.shields.io/badge/C99-orange?logo=c" alt="C99">
  </p>
</p>

Describe **what** should exist. CELS handles creation, destruction, state changes, and reconciliation automatically.

CELS reads like a modern declarative DSL, compiles as pure C99, and runs anywhere. Define reactive states, compose hierarchical component trees, declare when root compositions despawn with `CEL_Evaluation` and `CEL_Evaluate`, and let fine-grained recomposition process your updates. No classes, no vtables, no runtime heap allocations. Just four concepts and a single `#include <cels/cels.h>`.

- **C — Composition**: Declare what components exist and how they are hierarchically structured.
- **E — Evaluation**: React to state changes by re-running only the functions that depend on that state, skipping unchanged subtrees in $O(1)$ time. Root compositions govern their active presence via `CEL_Evaluation` and `CEL_Evaluate`.
- **L — Lifecycle**: Automate creation, teardown, and cascading cleanup on components; observe when a component is first called (`mount`) and when it leaves the tree (`unmount`) via `CEL_Lifecycle`.
- **S — State**: Manage reactive data in cache-aligned memory slabs owned by the `CEL_Session`, with snapshot diffing, keyed reactive observation (`cel_watch`), state mutation (`cel_mutate`), and persistent state memory (`cel_remember`, `cel_remember_state`).

```
 State (CEL_State)               defines DATA
 Compositions (CEL_Composition)  defines ROOT BOUNDARY (entry points & presence)
 Reactivity (cel_watch/mutate)   detects CHANGE        (fine-grained dependency)
 Lifecycles (CEL_Lifecycle)      controls LIFETIME     (mount / unmount hooks)
 Composables (CEL_Composable)    evaluates RENDERING   (stateless C functions)
```

Data flows in one direction:

```
cel_mutate updates (backBuffer) ──> Frame publish ──> cel_watch detects (frontBuffer) ──> Recomposition (O(1) skip for unchanged)
```

---

## The Core Mental Model: How CELS Executes Code

In traditional imperative C code, you manually create an object, update it when things change, and manually free it when done:

```c
/* Traditional imperative approach: manual management */
Panel *panel = PanelCreate();
PanelSetScroll(panel, 10);
/* Later: */
PanelDestroy(panel);
```

CELS works differently. Instead of manually creating and destroying objects, you write C functions that describe **what should exist right now**.

```
┌────────────────────────────────────────────────────────────────────────┐
│                        CEL_Session (Owner)                             │
│  - Cache-Aligned Memory Slab (Holds all CEL_State data)                │
│  - Slot Table (Tracks active groups, calls, and persistent slots)      │
│  - Reactive Invalidation Graph (Maps CEL_Id -> Dependent Functions)    │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│             CEL_Composition (Root Boundary)                            │
│  - Attached to CEL_Session with an explicit CEL_ID("...")              │
│  - Evaluated via a boolean condition: CEL_Evaluate(Predicate, ctx)     │
│  - Sets active session context for the duration of execution           │
│  - Determines if the entire tree branch stays active or detaches       │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Calls sub-tree functions
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│                     CEL_Composable (Sub-Tree Components)               │
│  - Plain C functions called inside a Composition                       │
│  - Operates directly on the active session with zero session passing   │
│  - Presence governed by regular C control flow (if / else)             │
│  - Automatically cleaned up if skipped during recomposition            │
│  - Uses cel_lifecycle to know when it mounts and unmounts              │
└────────────────────────────────────────────────────────────────────────┘
```

### Compositions vs. Composables

CELS divides your code into two structural tiers:

1. **`CEL_Composition` (The Root Boundary)**:
   - Attached to a session with a unique 64-bit identifier: `CEL_ID("NAME")`.
   - Evaluated by a boolean condition using `CEL_Evaluate`: if the predicate returns true, the composition stays alive; if false, CELS detaches the composition and frees its resources.
   - Sets the active session context for all child composables.
2. **`CEL_Composable` (Child Components)**:
   - Stateless C functions executed inside a composition.
   - Operates on the active session context with zero pointer passing.
   - Governed by standard C control flow (`if` / `else`).
   - If a branch evaluates to false during recomposition, CELS detects that the composable was skipped and automatically fires `unmount` hooks.

---

## Code Example

```c
#include <cels/cels.h>
#include <stdio.h>

/* 1. Declare cache-aligned reactive state */
CEL_State(WindowState) {
    int width;
    int height;
    bool isOpen;
    bool showInspector;
};

/* 2. Reusable child component lifecycle hooks (Pattern 2: mount / unmount blocks) */
typedef struct InspectorResource {
    void *gpuBufferHandle;
} InspectorResource;

CEL_Lifecycle(InspectorLifecycle, InspectorResource *res) {
    mount {
        res->gpuBufferHandle = (void*)0x55AA;
        printf("  [InspectorLifecycle] MOUNT: Native GPU buffer acquired (%p)\n", res->gpuBufferHandle);
    }
    unmount {
        printf("  [InspectorLifecycle] UNMOUNT: Native GPU buffer released (%p)\n", res->gpuBufferHandle);
        res->gpuBufferHandle = NULL;
    }
}

/* 3. Reusable child composables */
CEL_Composable(StatusBadge) {
    printf("    [Badge] Window is active & visible!\n");
}

CEL_Composable(InspectorPanel) {
    InspectorResource *res = cel_remember(InspectorResource, 0);
    cel_lifecycle(InspectorLifecycle, res);
    printf("    [InspectorPanel] Native GPU Buffer: %p | Active\n", res->gpuBufferHandle);
}

CEL_Composable(WindowContent, WindowState *win) {
    // Persistent local slot memory across recompositions
    int *renders = cel_remember(int, 0);
    (*renders)++;

    // Reactive subscription: automatically re-evaluates when mutated (Type first to match cel_remember)
    const WindowState *state = cel_watch(WindowState, CEL_ID("MainWindow"));
    printf("  [Content] Window: %dx%d (open: %s, inspector: %s) | Local Renders: %d\n",
           state ? state->width : win->width,
           state ? state->height : win->height,
           (state ? state->isOpen : win->isOpen) ? "true" : "false",
           (state ? state->showInspector : win->showInspector) ? "visible" : "hidden",
           *renders);

    if (state == NULL || state->isOpen) {
        StatusBadge();
    }
    if (state && state->showInspector) {
        InspectorPanel(); // Mounts and unmounts dynamically via InspectorLifecycle
    }
}

/* 4. Root composition and evaluation predicate */
CEL_Evaluation(WindowEval, void*, ctx) {
    (void)ctx;
    const WindowState *win = cel_get_state(CEL_ID("MainWindow"), WindowState);
    return (win == NULL || win->isOpen);
}

CEL_Composition(MainWindow, void *userData) {
    (void)userData;
    WindowState *win = cel_remember_state(CEL_ID("MainWindow"), WindowState, ((WindowState){
        .width = 800,
        .height = 600,
        .isOpen = true,
        .showInspector = false
    }));

    WindowContent(win);
}

/* 5. Main Loop & Execution */
int main(void) {
    CEL_Session session;
    CelsSessionInit(&session, NULL);

    // Attach root composition with evaluation predicate (no lifecycle needed on root)
    cel_attach(&session, CEL_ID("MainWindow"), MainWindow, NULL, WindowEval);

    // Initial mount pass
    CelsSessionRecompose(&session);

    // Mutate state between frames: toggle child component with lifecycle
    cel_mutate(&session, CEL_ID("MainWindow"), WindowState) {
        this->showInspector = true;
    }
    CelsSessionRecompose(&session); // Mounts InspectorPanel and acquires GPU buffer

    // Mutate state again: toggle off
    cel_mutate(&session, CEL_ID("MainWindow"), WindowState) {
        this->showInspector = false;
    }
    CelsSessionRecompose(&session); // Unmounts InspectorPanel and frees GPU buffer

    // Close window to trigger evaluation teardown
    cel_mutate(&session, CEL_ID("MainWindow"), WindowState) {
        this->isOpen = false;
    }
    CelsSessionRecompose(&session); // Detaches MainWindow subtree via WindowEval

    CelsSessionDestroy(&session);
    return 0;
}
```

---

## Four Core Concepts

### 1. State: Creation, Storage, Observation, and Mutation

All state in CELS is owned and tracked by the `CEL_Session`.

- **64-bit Identifiers (`CEL_ID`)**: Hashes strings at compile time using 64-bit FNV-1a:
  ```c
  CEL_Id id = CEL_ID("PlayerState");
  ```
- **Addressable State (`cel_remember_state`)**: Allocates or retrieves named reactive state in the session slab:
  ```c
  cel_remember_state(CEL_ID("Theme"), ThemeState, ((ThemeState){ .isDark = true }));
  ```
- **Positional Local Memory (`cel_remember`)**: Stable, pinned slot-table memory preserved across frames:
  ```c
  int *renderCount = cel_remember(int, 0);
  (*renderCount)++;
  ```
- **Reactive Observation (`cel_watch`)**: Registers the calling composable group as a subscriber (Type first):
  ```c
  const ThemeState *theme = cel_watch(ThemeState, CEL_ID("Theme"));
  ```
- **Thread-Safe Mutation (`cel_mutate`)**: Double-buffered block or value mutation. Banned inside composables during evaluation:
  ```c
  cel_mutate(session, CEL_ID("Theme"), ThemeState) {
      this->isDark = false;
  }
  ```

#### Double-Buffered Thread Safety & Lock-Free Reads

CELS guarantees zero torn reads using double-buffered state snapshots:
- **`frontBuffer`**: Read-only snapshot accessible lock-free from any reader thread via `cel_watch` or `cel_get_state`.
- **`backBuffer`**: Isolated write target returned by `cel_mutate`. Writers never mutate memory concurrently read by compositions.
- **Publish at Frame Boundary**: Back buffers are atomically published to front buffers at the start of `CelsSessionRecompose`.

### 2. Evaluation: Fine-Grained Recomposition & $O(1)$ Subtree Skipping

When state is mutated, CELS does not re-run the entire tree. It consults its dependency graph and re-evaluates only the composables watching that state. Unchanged subtrees are skipped in $O(1)$ time by advancing the logical cursor past the skipped group.

Root compositions govern their active presence via `CEL_Evaluate`:
```c
CEL_Evaluation(AuthGuard, void*, ctx) {
    const UserSession *user = cel_get_state(CEL_ID("User"), UserSession);
    return user && user->isLoggedIn;
}

cel_attach(session, CEL_ID("Dashboard"), DashboardView, NULL, AuthGuard);
```

Compositions can also be attached permanently without a predicate:
```c
cel_attach(session, CEL_ID("MainView"), MainView);
```

### 3. Lifecycle: Pure Mount and Unmount Hooks

Native resource allocation (Vulkan pipelines, file descriptors, audio voices) uses `CEL_Lifecycle`:
```c
CEL_Lifecycle(TextureLifecycle, Texture *tex) {
    mount {
        tex->handle = GpuCreateTexture(tex->path);
    }
    unmount {
        GpuDestroyTexture(tex->handle);
        tex->handle = 0;
    }
}
```
Inside a composable:
```c
Texture *tex = cel_remember(Texture, .path = "hero.png");
cel_lifecycle(TextureLifecycle, tex);
```
Parameterless lifecycles are also supported:
```c
CEL_Lifecycle(AudioStreamLifecycle) {
    mount { AudioPlayBgm(); }
    unmount { AudioStopBgm(); }
}
// Inside composable:
cel_lifecycle(AudioStreamLifecycle);
```

### 4. Multi-Session Architecture

Sessions in CELS are completely isolated instances. You can run multiple concurrent sessions for different subsystems:
```c
CEL_Session *uiSession   = CelSessionCreate(CEL_ID("UI"), NULL);
CEL_Session *gameSession = CelSessionCreate(CEL_ID("Game"), NULL);

// Lookup anywhere:
CEL_Session *s = cel_session(CEL_ID("UI"));
CEL_Session *curr = cel_active_session();

CelSessionDestroy(uiSession);
CelSessionDestroy(gameSession);
```

---

## Memory Slab Partitioning

All session memory lives in a contiguous 64-byte L1 cache-aligned slab. Zero runtime calls to `malloc()` or `free()`.

```
┌────────────────┬─────────────────────┬─────────────────────────────────┬──────────────────────┐
│  Groups Gap    │  Slot Allocation    │  Composable Slots               │  Session CEL_State   │
│  Buffer        │  Table              │  (grows UP from dataGapStart)   │  (grows DOWN from    │
│  (CelsSlotGroup│  (CelsSlotAllocation│                                 │   dataGapEnd)        │
└────────────────┴─────────────────────┴─────────────────────────────────┴──────────────────────┘
```

Configurable slab profiles:
- `CELS_SLAB_16K` (16 KB)
- `CELS_SLAB_32K` (32 KB)
- `CELS_SLAB_48K` (48 KB)
- `CELS_SLAB_64K` (64 KB) -- Default
- `CELS_SLAB_128K` .. `CELS_SLAB_1M`

```c
CEL_SLAB(mySlab, CELS_SLAB_32K);
CelsSessionConfig config = { .slab = mySlab, .slabSize = sizeof(mySlab) };
CelsSessionInit(&session, &config);
```

---

## Host & Application Architecture: Effortless Hot Reloading

Writing a host engine with dynamic hot reloading is effortless. The CELS library handles module path discovery, Windows shadow-copying (to avoid OS DLL file locks), symbol loading, state retention, and hot-swap recomposition automatically. **No external scripts or batch files required.**

### 1. Simple CMake Setup (`CMakeLists.txt`)

Declare your host executable and application target in a single call:

```cmake
cmake_minimum_required(VERSION 3.20...4.3)
project(my_project C)

include(FetchContent)
FetchContent_Declare(cels GIT_REPOSITORY https://github.com/jackblitz/cels.git GIT_TAG v0.2.0)
FetchContent_MakeAvailable(cels)

# In Debug: builds my_host (.exe) and my_app (.dll) with hot-reloading
# In Release: compiles everything into a single standalone monolithic .exe
cels_add_application(
    HOST my_host
    APP my_app
    HOST_SOURCES src/host.c
    APP_SOURCES  src/app.c src/ui.c
)
```

### 2. Simple Host Engine (`src/host.c`)

The host executable initializes the engine, loads the application, and runs the tick loop:

```c
#include <cels/cels.h>
#include <cels/engine.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    // 1. Initialize host engine and auto-load application
    CelsEngine engine;
    if (CelsEngineInit(&engine, NULL) != CELS_OK) {
        fprintf(stderr, "[Host] Failed to initialize engine\n");
        return 1;
    }

    // 2. Main host loop
    while (!engine.shouldQuit) {
        // Automatically checks if app was rebuilt on disk and hot-swaps live in <50ms
        CelsAppRuntimeCheck(&engine);

        // Recompose all active sessions
        CelsEngineRecompose(&engine);

        // Frame pacing
        SleepMs(16);
    }

    // 3. Clean teardown
    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);
    return 0;
}
```

### 3. Simple Application Module (`src/app.c`)

Your application logic returns its root composition on start:

```c
#include <cels/cels.h>

CEL_Composable(Header) { ... }
CEL_Composition(MainWindow, void *userData) {
    Header();
}

static CelsCompositionRef App_OnStart(CelsEngine *engine, CelsSession *session) {
    (void)engine; (void)session;
    return CEL_COMPOSITION(MainWindow);
}

CEL_App(MyApp,
    .onStart = App_OnStart
);
```

### 4. Organizing Composables: Headers (.h) vs. Source Files (.c)

CELS gives you complete freedom to structure components cleanly across files:

- **Header-Only Composables (`.h`)**:
  Use `CEL_Composable(Name)` directly in `.h` files. Because it expands to `static inline`, it can be included across multiple files with zero linker collisions:
  ```c
  // status_badge.h
  #pragma once
  #include <cels/cels.h>
  
  CEL_Composable(StatusBadge) {
      printf("[Badge] Rendered\n");
  }
  ```

- **Separated Composables (`.h` + `.c`)**:
  For larger components or when hiding implementation details:
  ```c
  // window_content.h
  #pragma once
  #include <cels/cels.h>
  #include "window.h"

  CEL_ComposableDecl(WindowContent, WindowState *win);
  ```
  ```c
  // window_content.c
  #include "window_content.h"

  CEL_ComposableDef(WindowContent, WindowState*, win) {
      // Component logic here
  }
  ```

When you edit `src/app.c` or any composable `.c` file, simply build target `my_app` (`cmake --build build --target my_app`). CELS detects the new library and hot-swaps it live while preserving all slot memory and state!

---

## Three-Tier API Reference

| Tier | Prefix | Role | Examples |
|---|---|---|---|
| **Structure** | `CEL_` | Declarative definitions, types, and attachments | `CEL_State`, `CEL_Composition`, `CEL_Composable`, `CEL_ComposableDecl`, `CEL_ComposableDef`, `CEL_Lifecycle`, `CEL_Evaluation`, `CEL_Evaluate`, `CEL_ID`, `CEL_Module` |
| **Plumbing** | `cels_` / `Cels` | Engine lifecycle, session management, and recomposition | `CelSessionCreate`, `CelSessionDestroy`, `CelsSessionInit`, `CelsSessionRecompose`, `CelsEngineInit` |
| **Runtime** | `cel_` | In-composable reactive operations & memory | `cel_watch`, `cel_mutate`, `cel_remember`, `cel_remember_state`, `cel_lifecycle`, `cel_attach`, `cel_session`, `cel_active_session` |

---

## Building and Testing

```bash
# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run the 8-feature test suite (39 tests)
./build/debug/windows/test_cli.exe

# Run the performance benchmark
./build/debug/windows/benchmark.exe --quick
```

---

## License

Apache License 2.0. Copyright (c) 2026 Jack Blitz.
