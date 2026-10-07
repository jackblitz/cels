---
name: cels-app-development
description: Provide technical guidance for CELS application and host development. Use when creating new CELS applications, configuring host executables and application modules, managing CMake build targets, configuring CLion run configurations, or implementing hot-reloadable workflows.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.4.0"
  last-updated: '2026-09-30'
  category: application-development
  keywords:
    - application
    - host
    - hot-reload
    - cmake
    - cels_add_application
    - engine
    - two-targets
    - cel_host
    - cel_dll
    - CLion
    - C99
---

# CELS Application & Host Development Guide

CELS cleanly separates the **Host** (the native runtime harness and loop) from the **Application** (the declarative reactive UI/logic). This enables live hot-reloading during development while compiling to a zero-overhead monolithic binary in release mode.

---

## 1. Division of Responsibilities

```
+--------------------------------------------------------------------+
|  HOST (cel_host)                                                   |
|  - Platform entry point (main / WinMain)                           |
|  - Manages OS windowing / input polling / event loop               |
|  - Owns CelsEngine and ticks it                                    |
|  - Checks for DLL reloads (CelsAppRuntimeCheck)                    |
+---------------------------------+----------------------------------+
                                  | loads / recomposes
                                  v
+--------------------------------------------------------------------+
|  APPLICATION (cel_app)                                             |
|  - Declares CEL_App or CEL_App_Def application definition          |
|  - Implements onStart / onReload / onEnd hooks                     |
|  - Defines Compositions & Composable UI trees                      |
+--------------------------------------------------------------------+
```

---

## 2. Creating an Application (`cel_app`)

An application requires:
1. One or more Compositions (e.g. `WindowComposition`).
2. The `CEL_App` declarative macro binding the root composition directly (or `CEL_App_Def` for custom hooks).
3. Optional evaluation predicate (e.g. `WindowEval`) or custom lifecycle callbacks via `CEL_App_Def`.

### Minimal Application Example (`app.c`)
```c
#include "cels.h"
#include "composition/window.h"

/* Declarative root: binds WindowComposition and WindowEval directly */
CEL_App(WindowApp, WindowComposition, WindowEval);
```

### Declarative `CEL_App` Signatures
- `CEL_App(AppName, RootComp)`: Direct 1-line root attachment.
- `CEL_App(AppName, RootComp, EvalPred)`: Direct root attachment with lifecycle evaluation predicate.
- `CEL_App(AppName, RootComp, EvalPred, UserData)`: Direct root attachment with evaluation and injected instance context.
- `CEL_App_Def(AppName, ...)`: Clean designated initializers for application definition (`.onStart = OnStart, .onReload = OnReload, .onEnd = OnEnd`). Exported automatically as `CelsGetAppDef` and `CelsGetAppManifest`.
- `CEL_App_Manifest(AppName, ...)`: Legacy alias for `CEL_App_Def`.

### Root Compositions and `userData`: How, When, and Why

In CELS, root compositions can be declared in two ways depending on whether they need external instance data:

#### 1. Standard Root Composition (Zero Arguments - Recommended)
Most application windows and UI trees do not need external parameters because state is hoisted locally with `cel_state(...)` or accessed via ambient context (`cel_user_data(Type)`):

```c
// Zero parameters, zero (void)userData boilerplate
CEL_Composition(MainWindow) {
    WindowState *win = cel_state(WindowState, { .isOpen = true });
    WindowContent(win);
}

// In onStart:
CEL_OnStart(App_OnStart) {
    cel_attach(session, MainWindow);
}
```

#### 2. Root Composition with Injected `userData` (Typed Parameter)
When the host or application factory needs to inject specific instance data (e.g. multi-window configs, native handles, or external ECS worlds), declare the parameter directly:

```c
typedef struct ViewportConfig {
    int cameraIndex;
    const char *label;
} ViewportConfig;

// Typed parameter received directly
CEL_Composition(ViewportView, ViewportConfig*, cfg) {
    RenderCamera(cfg->cameraIndex);
    DrawLabel(cfg->label);
}

// In onStart:
static ViewportConfig g_mainView = { .cameraIndex = 0, .label = "Primary 3D View" };

CEL_OnStart(App_OnStart) {
    // Injects g_mainView into ViewportView via cel_attach
    cel_attach(session, ViewportView, NULL, &g_mainView);
}
```

#### The 5 Real-World Use Cases for `userData`:
1. **Multi-Window / Multi-Viewport Instances**: Running the exact same composition function for multiple distinct viewports (e.g. Top, Front, Perspective cameras) with different configuration structs.
2. **Native OS / Hardware Handles**: Injecting host platform handles (Win32 `HWND`, GLFW window, Vulkan `VkDevice`) into the root composition upon launch.
3. **External ECS / Physics Worlds**: Passing external engine contexts (e.g. Flecs `ecs_world_t*`, Box2D `b2WorldId`) down into root compositions.
4. **Plugin Embeddings (VST / CLAP / Game Mods)**: When CELS is embedded in a digital audio workstation or modding engine where the host provides an audio processor instance pointer.
5. **Unit Testing & Mocking**: Passing mock backends (simulated network lag, fake file systems) to verify UI flows under test conditions.

---

## 3. Creating a Host (`cel_host`)

The host executable initializes the `CelsEngine`, runs the tick loop, forwards platform input events as state mutations, and tears down cleanly.

### Minimal Host Example (`host.c`)
```c
#include "cels.h"
#include "cels/engine.h"
#include "window.h"
#include <stdio.h>

#if defined(_WIN32)
    #include <windows.h>
    #define SleepMs(ms) Sleep(ms)
#else
    #include <unistd.h>
    #define SleepMs(ms) usleep((ms) * 1000)
#endif

/* Approach A: Standard Canonical Host (Recommended) */
#include "host.h"

int main(int argc, char **argv)
{
    return CelsRunHost(argc, argv);
}

/* Approach B: Custom Host Engine Loop */
int custom_host_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * 1. Initialize engine with workload capacity profile and app target:
     * - CELS_PROFILE_1K: 128 KiB cache-aligned slab, up to 1,024 composables
     * - CELS_PROFILE_512: 64 KiB L1 cache-resident slab, up to 512 composables
     */
    CelsEngine engine;
    if (CelsEngineInitWithProfile(&engine, CELS_APP_TARGET, CELS_PROFILE_1K) != CELS_OK) {
        fprintf(stderr, "[Host] Failed to initialize engine\n");
        return 1;
    }

    printf("[Host] Engine started (CELS_PROFILE_1K: 128 KiB slab). Entering main loop...\n");

    /* 2. Main loop */
    while (!engine.shouldQuit) {
        /* Check if application DLL was rebuilt and hot-swap if needed */
        CelsAppRuntimeCheck(&engine);

        /* Recompose all active sessions */
        CelsEngineRecompose(&engine);

        /* Example: Send targeted signal based on input / events */
        // cel_signal(cel_get_session(&engine, "main"), WindowActionSignal, { .action = WINDOW_ACTION_TOGGLE });

        SleepMs(16); /* ~60 FPS */
    }

    /* 3. Clean teardown */
    printf("[Host] Shutting down...\n");
    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);

    return 0;
}

/* Approach C: Programmatic Host-to-App API & Multi-DLL Hosting */
int multi_dll_host_main(void)
{
    CelsEngine engine;
    CelsEngineInit(&engine, NULL);

    /* 1. Create secondary session for auxiliary tool/inspector window */
    CelsSession *toolSession = CelsEngineCreateSession(&engine, "inspector", CELS_PROFILE_512);

    /* 3. Bind and load independent application DLLs */
    CelsApp mainApp;
    CelsAppLoad(&mainApp, &engine, &engine.session, "main_app.dll");

    CelsApp toolApp;
    CelsAppLoad(&toolApp, &engine, toolSession, "tool_app.dll");

    /* 4. Start applications (runs onStart and initial composition) */
    CelsAppStart(&mainApp);
    CelsAppStart(&toolApp);

    /* 5. Main tick loop */
    while (!engine.shouldQuit) {
        /* Check hot-reloading for each loaded application */
        CelsAppCheckReload(&mainApp);
        CelsAppCheckReload(&toolApp);

        /* Only recompose if any session has pending mutations, events, or hot-swaps */
        if (CelsEngineNeedsRecompose(&engine)) {
            CelsEngineRecompose(&engine);
        }

        SleepMs(16);
    }

    /* 6. Clean teardown */
    CelsAppDestroy(&toolApp);
    CelsAppDestroy(&mainApp);
    CelsEngineDestroy(&engine);
    return 0;
}
```

---

## 4. CMake Configuration

CELS provides built-in CMake helper macros: `cels_add_application` and `cels_add_host`.

### Example `CMakeLists.txt`
```cmake
cmake_minimum_required(VERSION 3.20)
project(my_cels_project C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Include CELS module
find_package(CELS REQUIRED) # Or include(path/to/cmake/Cels.cmake)

# Define Application Pair (**_host executable + **_dll shared library)
cels_add_application(
    HOST cel_host
    DLL cel_dll
    HOST_SOURCES
        src/host.c
    APP_SOURCES
        src/app.c
        src/composition/window.c
    INCLUDES
        include
        src
)
```

In CLion and IDEs, two standard run configurations are provided for each app:
- **`**_host`**: Launches the main engine executable (`cel_host.exe`, `cel_task_host.exe`, `cel_transition_host.exe`, `cel_event_host.exe`).
- **`**_dll`**: Recompiles the dynamic application library (`cel_dll.dll`, `cel_task_dll.dll`, `cel_transition_dll.dll`, `cel_event_dll.dll`) and displays hot-reload confirmation with exit code 0.

---

## 5. Hot Reloading vs. Monolithic Build Modes

CELS adapts automatically based on the `CELS_HOT_RELOAD` CMake option:

### Hot Reload Mode (`CELS_HOT_RELOAD=ON`, default in Debug)
- Application builds as a shared dynamic library (`cel_dll.dll` / `libcel_dll.so`).
- Host monitors the file write timestamp via `CelsAppRuntimeCheck`.
- When you edit and recompile `cel_dll` in CLion (Play or Build), the host:
  1. Detects the new timestamp.
  2. Copies the new DLL to a shadow temporary file (avoiding Windows file locks).
  3. Reloads the symbols and refreshes lifecycle function pointers.
  4. Triggers an immediate recompose, preserving the entire session state and slot memory!
- On host exit, temporary shadow `.dll` files are automatically cleaned up.

### Monolithic Mode (`CELS_HOT_RELOAD=OFF`, default in Release)
- Application sources are statically linked directly into `cel_host`.
- No DLLs are created or loaded at runtime.
- Maximum performance, zero runtime dynamic loading overhead, and simple single-executable distribution.

---

## 6. Core Patterns & Anti-Patterns (Best for LLMs)

### Pattern 1: Application Pair Declaration in CMake

```cmake
# WRONG: Creating 3rd custom targets or separate app aliases
cels_add_application(HOST my_host DLL my_dll ...)
add_custom_target(my_app DEPENDS my_dll) # Bad! Confuses IDE configurations and generates redundant targets

# CORRECT: Strictly two targets per application (host and dll)
cels_add_application(
    HOST cel_host
    DLL cel_dll
    HOST_SOURCES examples/window/host.c
    APP_SOURCES examples/window/app/app.c
)
```

### Pattern 2: Hot-Reload Check in Host Loop

```c
// WRONG: Forgetting CelsAppRuntimeCheck in host loop
while (!engine.shouldQuit) {
    CelsEngineRecompose(&engine); // DLL is recompiled on disk, but never reloaded into host!
    SleepMs(16);
}

// CORRECT: Call CelsAppRuntimeCheck on every tick before recomposition
while (!engine.shouldQuit) {
    CelsAppRuntimeCheck(&engine); // Detects DLL modification, shadow-copies, and updates lifecycle pointers
    CelsEngineRecompose(&engine);
    SleepMs(16);
}
```

### Pattern 3: Application Definition Export
 
```c
// WRONG: Using manual symbol exports, custom structs, or boilerplate onStart
__declspec(dllexport) void* MyCustomInit() { ... }

// CORRECT: Declarative root composition attachment
CEL_App(WindowApp, WindowComposition, WindowEval);

// Or when custom application hooks are needed:
// CEL_OnStart(App_OnStart) { cel_attach(session, WindowComposition); }
// CEL_OnEnd(App_OnEnd) { /* teardown */ }
// CEL_App_Def(WindowApp, .onStart = App_OnStart, .onReload = App_OnStart, .onEnd = App_OnEnd);
```

