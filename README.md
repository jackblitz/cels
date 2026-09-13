<p align="center">
  <h1 align="center">CELS</h1>
  <p align="center"><strong>C99 macros that mean what they say.</strong></p>
  <p align="center">A declarative and explicit language to stop hallucinations from killing your vibe.</p>
  <p align="center">
    <img src="https://img.shields.io/badge/version-v0.1.0-blue" alt="version">
    <img src="https://img.shields.io/badge/license-Apache%202.0-green" alt="license">
    <img src="https://img.shields.io/badge/C99-orange?logo=c" alt="C99">
  </p>
</p>

Describe **what** should exist. CELS handles creation, destruction, state changes, and reconciliation automatically.

CELS reads like a DSL, compiles as pure C99, and runs anywhere. Define reactive states, compose hierarchical component trees, declare when nodes despawn with `CEL_LifeCycle`, and let fine-grained recomposition process your updates. No classes, no vtables, no runtime heap allocations. Just four concepts and a single `#include`.

- **C** omposition -- declare what components exist and how they're hierarchically structured
- **E** valuation -- react to state changes with fine-grained recomposition and $O(1)$ subtree skipping
- **L** ifecycle -- automate creation, unmounting, and cascading cleanup; release native handles in `cel_lifecycle_state` callbacks
- **S** tate -- manage reactive data in cache-aligned memory slabs with snapshot diffing and persistent local memory (`cel_remember`)

```
 State (CEL_State)               defines DATA
 Compositions (CEL_Composition)  defines STRUCTURE    (what exists)
 Reactivity (cel_watch/mutate)   detects CHANGE       (when things happen)
 Lifecycles (CEL_LifeCycle)      controls LIFETIME    (when things live and die)
 Composables (CEL_Composable)    evaluates RENDERING  (how nodes update)
```

Data flows in one direction:

```
cel_mutate updates ──> cel_watch detects ──> Recomposition reacts (O(1) skip for unchanged)
```

---

## What Makes CELS Different

Unlike traditional GUI and state frameworks that require manual dirty-flag bookkeeping, callback spaghetti, and event cascades, CELS introduces **declarative compositions**. You describe **what** should exist, and the engine reconciles the tree automatically.

Think React's or Jetpack Compose's component and slot-table model, engineered for native systems in **pure C99** with:
- **Zero Runtime Heap Allocations**: All component structural groups, remembered variables, and reactive slots live inside an L1 cache-aligned dual gap-buffer slab.
- **$O(1)$ Subtree Skipping**: If a parent state changes but a child's watched dependencies remain untouched, CELS skips entire subtrees in constant time.
- **Dynamic Hot-Reloading in Development**: Run your host engine while editing application code live in CLion or your favorite IDE. Rebuilds swap in `<50ms` with **zero Windows DLL file locks** and full state retention.
- **Single Monolithic Binary in Production**: Switch to Release mode to compile host and app into a single standalone `.exe` with **zero `.dll` dependencies**.

---

## Code Example

You declare intent. The framework does the work.

```c
#include <cels.h>
#include <stdio.h>

/* 1. Define reactive state */
CEL_State(WindowState) {
    int width;
    int height;
    bool isOpen;
};

/* 2. Lifecycle callbacks for native resources */
static void OnWindowOpen(WindowState *w, CelsSession *s) {
    (void)s;
    printf("[Lifecycle] Window opened: %dx%d\n", w->width, w->height);
}
static void OnWindowClose(WindowState *w, CelsSession *s) {
    (void)w; (void)s;
    printf("[Lifecycle] Window closed\n");
}

/* 3. Reusable composable components */
CEL_Composable(StatusBadge, void) {
    printf("    [Badge] Window is active & visible!\n");
}

CEL_Composable(WindowContent, WindowState*, win) {
    // Persistent local memory across recompositions
    int *renders = cel_remember(int, 0);
    (*renders)++;

    // Reactive subscription: automatically re-evaluates when mutated
    WindowState state = cel_watch(win);
    printf("  [Content] Window: %dx%d | Local Renders: %d\n", 
           state.width, state.height, *renders);

    if (state.isOpen) {
        StatusBadge();
    }
}

/* 4. Composition root and lifecycle condition */
CEL_Composition(MainWindow, key) {
    (void)key;
    WindowState init = { .width = 800, .height = 600, .isOpen = true };
    WindowState *win = cel_lifecycle_state(init, OnWindowOpen, OnWindowClose);

    WindowContent(win);
}

CEL_LifeCycle(WindowGuard, WindowState) {
    if (it != NULL) {
        WindowState state = cel_watch(it);
        if (!state.isOpen) {
            cel_destroy(); // Automatically prunes and cleans up the subtree
        }
    }
}

/* 5. Application entry point */
static CelsCompositionRef App_OnStart(CelsEngine *engine, CelsSession *session) {
    (void)engine; (void)session;
    return CEL_COMPOSITION(MainWindow, WindowGuard);
}

CEL_App(WindowApp,
    .onStart = App_OnStart
);
```

---

## Three-Tier API

The API follows a strict three-tier prefix convention:

| Tier | Prefix | Role | Examples |
|---|---|---|---|
| **Structure** | `CEL_` | Declarative definitions, types, and attachments | `CEL_State`, `CEL_Composition`, `CEL_Composable`, `CEL_LifeCycle`, `CEL_Attach`, `CEL_App` |
| **Plumbing** | `cels_` / `Cels` | Engine lifecycle, session management, and CMake helpers | `CelsSessionInit`, `CelsSessionRecompose`, `CelsEngineStart`, `cels_add_application` |
| **Runtime** | `cel_` | In-composable reactive operations & memory | `cel_watch`, `cel_mutate`, `cel_remember`, `cel_lifecycle_state`, `cel_destroy`, `cel_init` |

---

## Quick Start with CMake

Integrate CELS into your CMake project via `FetchContent`:

```cmake
cmake_minimum_required(VERSION 3.20...4.3)
project(my_app C)

include(FetchContent)
FetchContent_Declare(
    cels
    GIT_REPOSITORY https://github.com/jackblitz/cels.git
    GIT_TAG v0.2.0
)
FetchContent_MakeAvailable(cels)

# Declare host executable and application module in one call
cels_add_application(
    HOST my_host
    APP my_app
    HOST_SOURCES src/host.c
    APP_SOURCES src/app.c src/ui.c
    # MODE AUTO: Debug -> Hot-Reload (.exe + .dll), Release -> Single Binary (.exe)
)
```

Build and run:

```bash
cmake -B build && cmake --build build && ./build/my_host
```

`cels_add_application` automatically creates:
- **`my_host`**: Your host engine executable.
- **`my_app`**: Your reloadable application logic target.
- **`my_app_rebuild`**: An IDE runner target allowing you to click the green **Play** button (<kbd>Shift</kbd>+<kbd>F10</kbd>) in CLion / IDEs to rebuild and hot-reload in `<50ms`.
- **`rebuild_my_app.bat` / `.sh`**: Ready-to-run terminal rebuild scripts in the build directory.

---

## Host Engine Architecture & Integration

CELS supports two deployment workflows using the **exact same C99 engine loop**:
1. **Dynamic Hot-Reloading in Development (`Debug`)**: Rebuilds swap in `<50ms` with zero Windows DLL file locks and full state retention in L1 cache slabs.
2. **Single-Binary Monolithic in Production (`Release`)**: Host and app compile into a single standalone `.exe` with zero `.dll` dependencies and maximum compiler inlining.

### Unified Engine Main Loop

Developers control their own hardware subsystems, input polling, and frame loop without any OS headers (`windows.h`), path utilities, or external rebuild scripts:

```c
#include <cels.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    // 1. Initialize host engine
    CelsEngine engine;
    CelsEngineInit(&engine, NULL, NULL);

    // 2. Register persistent engine subsystems (survives dynamic reloads)
    static PlatformSubsystem platform = { .renderer = "Vulkan", .targetFps = 60 };
    CEL_RegisterModule(&engine, PlatformSubsystem, &platform);

    // 3. Mount application (auto-discovers DLL in Debug, static bind in Release)
    if (CelsEngineLoadApp(&engine, "my_app") != CELS_OK) {
        CelsEngineDestroy(&engine);
        return 1;
    }

    // 4. Main Game / Engine Loop
    while (!engine.shouldQuit) {
        // Automatically detects on-disk rebuilds in Debug; inlines to false in Release
        if (CelsAppRuntimeCheck(&engine)) {
            printf("[Engine] Application code hot-swapped!\n");
        }

        Engine_PollEvents();
        Engine_Update();

        // Recompose declarative UI & reactive state
        CelsSessionRecompose(&engine.session);

        Engine_Render();
    }

    // 5. Clean teardown
    CelsEngineDestroy(&engine);
    return 0;
}
```

| Deployment Mode | Build Type | CMake Mode | Runtime Behavior |
|---|---|---|---|
| **Hot-Reload** | `Debug` | `MODE HOT_RELOAD` (default) | `CelsEngineLoadApp` creates shadow copy `.hot_*.tmp.dll`. `CelsAppRuntimeCheck` polls timestamps and hot-swaps code live in `<50ms`. |
| **Monolithic** | `Release` | `MODE SINGLE_BINARY` | Compiles into a single `.exe`. `CelsEngineLoadApp` binds statically. `CelsAppRuntimeCheck` is a zero-cost inline returning `false`. |

---

## Non-Hot-Reloading Setup (Single-Binary & Static Linking)

If your project does not need dynamic DLL hot-reloading (e.g., embedded systems, production distribution, or traditional static binaries), you can disable hot-reloading entirely and compile everything into a single `.exe`.

### Approach 1: Single-Binary via `cels_add_application` (Recommended)

Pass the `SINGLE_BINARY` (or `MONOLITHIC`) flag directly in your `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20...4.3)
project(my_app C)

include(FetchContent)
FetchContent_Declare(
    cels
    GIT_REPOSITORY https://github.com/jackblitz/cels.git
    GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(cels)

# Forces a single monolithic executable in ALL build configurations (Debug & Release)
cels_add_application(
    HOST my_app
    APP my_app_logic
    HOST_SOURCES src/host.c
    APP_SOURCES src/app.c
    SINGLE_BINARY
)
```

Alternatively, leave `MODE AUTO` and pass `-DCELS_HOT_RELOAD=OFF` or `-DCMAKE_BUILD_TYPE=Release` at configuration time:

```bash
cmake -B build -DCELS_HOT_RELOAD=OFF
cmake --build build
```

This compiles `HOST_SOURCES` and `APP_SOURCES` directly into a single `my_app.exe` with **zero `.dll` files** and **zero OS dynamic library dependencies**.

---

### Approach 2: Traditional Static Linking (`target_link_libraries`)

If you prefer traditional CMake without host/app target splitting, link directly to the `cels::cels` static library target:

```cmake
cmake_minimum_required(VERSION 3.20...4.3)
project(my_standalone_app C)

set(CMAKE_C_STANDARD 99)

# Include CELS library via GitHub FetchContent
include(FetchContent)
FetchContent_Declare(
    cels
    GIT_REPOSITORY https://github.com/jackblitz/cels.git
    GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(cels)

# Standard single executable
add_executable(my_standalone_app src/main.c)
target_link_libraries(my_standalone_app PRIVATE cels::cels)
```

#### Direct Embedded `CelsSession` (Zero Engine Overhead)

If you only want CELS's reactive composition tree, slot table, and memory slabs embedded directly inside your own custom engine or subsystem:

```c
#include <cels.h>
#include <stdio.h>

CEL_State(MyState) { int score; };

CEL_Composable(HUD, MyState*, s) {
    MyState state = cel_watch(s);
    printf("Player score: %d\n", state.score);
}

CEL_Composition(GameRoot, key) {
    (void)key;
    MyState init = { .score = 100 };
    MyState *s = cel_lifecycle_state(init, NULL, NULL);
    HUD(s);
}

int main(void) {
    // 1. Initialize reactive session (allocates L1 cache-aligned slab arena)
    CelsSession session;
    CelsSessionInit(&session, NULL);

    // 2. Attach composition root
    CelsCompositionRef root = CEL_COMPOSITION(GameRoot);
    CelsSessionAttachComposition(&session, root.key, root.body, root.lifecycleEval, NULL);

    // 3. Initial recomposition
    CelsSessionRecompose(&session);

    // 4. In-frame state mutation and recomposition
    MyState *state = CEL_GetState(&session, root.key, MyState);
    if (state != NULL) {
        cel_mutate(&session, state) {
            this->score += 50;
        }
        CelsSessionRecompose(&session);
    }

    // 5. Clean teardown
    CelsSessionDestroy(&session);
    return 0;
}
```

---
