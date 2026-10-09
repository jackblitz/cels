---
name: cels-app-and-host
description: Provide technical guidance for CELS application and host architecture. Use when creating new CELS applications, configuring host executables and dynamic application modules, setting up CMake build targets, configuring hot-reloadable workflows, or managing engine capacity profiles and multi-session hosting.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.5.0"
  last-updated: '2026-10-07'
  category: application-and-host
  keywords:
    - application
    - host
    - engine
    - hot-reload
    - cmake
    - cels_add_application
    - CEL_App_Manifest
    - cel_attach
    - capacity-profiles
    - multi-session
    - C99
---

# CELS Application & Host Development Guide

CELS cleanly separates the **Host** (the native platform harness and event loop) from the **Application** (declarative reactive compositions and logic). This architecture enables live hot-reloading during development while compiling to a zero-overhead monolithic binary in release builds.

---

## 1. Division of Responsibilities

```
+--------------------------------------------------------------------+
|  HOST (cel_host)                                                   |
|  - Platform entry point (main / WinMain)                           |
|  - Manages OS windowing / input polling / event loop               |
|  - Owns CelsEngine, configures session capacity profiles           |
|  - Attaches platform context (input, renderers, audio) to session  |
|  - Checks for DLL reloads (CelsAppRuntimeCheck)                    |
+---------------------------------+----------------------------------+
                                  | loads / recomposes
                                  v
+--------------------------------------------------------------------+
|  APPLICATION (cel_app)                                             |
|  - Declares CEL_App_Manifest application manifest                  |
|  - Implements onStart / onReload / onEnd lifecycle callbacks       |
|  - Receives root session and attaches root compositions            |
|  - Implements declarative compositions and reactive logic          |
+--------------------------------------------------------------------+
```

---

## 2. Creating an Application (`cel_app`)

An application module requires:
1. One or more root Compositions (e.g. `MainWindowComposition`).
2. An `onStart(CelsSession *session)` callback that receives the host-created root session and attaches root compositions with `cel_attach(session, MainWindowComposition, MainWindowEval)`.
3. The `CEL_App_Manifest` macro exporting application metadata (`title`, `appVersion`) and lifecycle callbacks (`onStart`, `onReload`, `onEnd`).

### Canonical Application Example (`app.c`)
```c
#include "cels.h"
#include "cels/app.h"
#include "composition/main_window.h"

static void MainWindowApp_OnStart(CelsSession *session)
{
    /* Attach root composition and optional lifecycle evaluation predicate */
    cel_attach(session, MainWindowComposition, MainWindowEval);
}

static void MainWindowApp_OnEnd(CelsSession *session)
{
    (void)session;
}

/*
 * Application Manifest.
 * Communicates metadata and lifecycle hooks to the host engine.
 */
CEL_App_Manifest(MainWindowApp,
    .title = "Main Application Window",
    .appVersion = "1.0.0",
    .onStart = MainWindowApp_OnStart,
    .onReload = MainWindowApp_OnStart,
    .onEnd = MainWindowApp_OnEnd
);
```

### Manifest Definition Signature
- `CEL_App_Manifest(AppName, ...)`: Designated initializer exporting application metadata and hooks (`.title`, `.appVersion`, `.onStart`, `.onReload`, `.onEnd`).

### Root Session Ownership & Platform Context Handshake
In CELS, the **host executable always creates and owns the primary root session** (`&engine.session` initialized via `CelsEngineInitWithProfile`). The host passes this root session directly to the application via `onStart(CelsSession *session)`.

1. **Unopinionated Core**: CELS core does not impose custom engine-level context layers or enforce opinionated session creators.
2. **Standard Ambient Context Injection**: If the host provides platform services (input devices, window handles, renderers, audio), it attaches them directly to the root session at startup using standard `cel_set_context`:
   ```c
   /* In host main: attach platform input to root session at depth 0 */
   cel_set_context(&engine.session, PdcursesInputContext, &inputCtx);
   ```
3. **Subtree Inheritance**: Any composable anywhere in the session tree can resolve platform services via `cel_context(PdcursesInputContext)`.

### Root Compositions: How, When, and Why

Root compositions attached via `cel_attach` can be declared in two ways:

#### 1. Standard Root Composition (Zero Arguments - Recommended)
Most UI trees do not require external parameters because state is hoisted locally with `cel_state(...)` or accessed via ambient context (`cel_context(Type)`):

```c
CEL_Composition(MainWindow) {
    WindowState *win = cel_state(WindowState, { .isOpen = true });
    WindowContent(win);
}

// In onStart:
static void App_OnStart(CelsSession *session) {
    cel_attach(session, MainWindow, WindowEval);
}
```

#### 2. Root Composition with Injected `userData` (Typed Parameter)
When the host or application factory needs to inject specific instance data (e.g. multi-window configs, native OS handles, or external ECS worlds), declare the parameter directly:

```c
typedef struct ViewportConfig {
    int cameraIndex;
    const char *label;
} ViewportConfig;

CEL_Composition(ViewportView, ViewportConfig*, cfg) {
    RenderCamera(cfg->cameraIndex);
    DrawLabel(cfg->label);
}

// In onStart:
static ViewportConfig g_mainView = { .cameraIndex = 0, .label = "Primary 3D View" };

static void App_OnStart(CelsSession *session) {
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

The host executable initializes the `CelsEngine`, runs the tick loop, forwards platform input events, and tears down cleanly.

### Canonical Host Implementation (`host.c`)
```c
#include "cels.h"
#include "cels/engine.h"
#include <stdio.h>

#if defined(_WIN32)
    #include <windows.h>
    #define SleepMs(ms) Sleep(ms)
#else
    #include <unistd.h>
    #define SleepMs(ms) usleep((ms) * 1000)
#endif

int main(int argc, char **argv)
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

    /* 2. Attach Host Platform Services to the Root Session */
    static PdcursesInputContext inputCtx;
    cel_set_context(&engine.session, PdcursesInputContext, &inputCtx);

    /* 3. Read Application Manifest Metadata */
    const CelsAppManifest *manifest = CelsEngineGetManifest(&engine);
    if (manifest && manifest->title) {
        printf("[Host] Launching application: %s v%s\n", 
               manifest->title, 
               manifest->appVersion ? manifest->appVersion : "1.0");
    }

    printf("[Host] Engine started. Entering main loop...\n");

    /* 4. Main loop */
    while (!engine.shouldQuit) {
        /* Poll native input events and commit key states */
        // CelsPdcursesPollInput(&inputCtx);

        /* Check if application DLL was rebuilt and hot-swap if needed */
        CelsAppRuntimeCheck(&engine);

        /* Recompose all active sessions */
        CelsEngineRecompose(&engine);

        SleepMs(16); /* ~60 FPS */
    }

    /* 5. Clean teardown */
    printf("[Host] Shutting down...\n");
    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);

    return 0;
}
```

### Workload Capacity Profiles (`CelsSessionProfile`)

Rather than forcing developers to calculate raw byte offsets, CELS provides **intent-driven capacity profiles** based on expected composable counts:

| Profile | Composable Capacity | Total Slab Size | Best Used For |
| :--- | :--- | :--- | :--- |
| `CELS_PROFILE_DEFAULT` | 4,096 composables | 512 KiB | Default standard capacity if unspecified |
| `CELS_PROFILE_128` | 128 composables | 16 KiB | Micro-dialogs, tiny popups, embedded widgets |
| `CELS_PROFILE_256` | 256 composables | 32 KiB | HUD overlays, tooltips, sub-panels |
| `CELS_PROFILE_512` | 512 composables | 64 KiB | L1/L2 cache-resident UI panels & focused subtrees |
| `CELS_PROFILE_1K` | 1,024 composables | 128 KiB | Standard application windows & forms |
| `CELS_PROFILE_2K` | 2,048 composables | 256 KiB | Complex screens with many active lists & controls |
| `CELS_PROFILE_4K` | 4,096 composables | 512 KiB | Broad applications with multiple concurrent modules |
| `CELS_PROFILE_8K` | 8,192 composables | 1 MiB | Heavy simulation trees & large hierarchies |

### Host Execution Cadence: Continuous vs. Event-Driven

| Cadence Strategy | Host Loop Implementation | Ideal Use Case |
| :--- | :--- | :--- |
| **Continuous Game Loop** | Runs `CelsEngineRecompose(&engine)` every tick at 60/120 FPS | Real-time games, physics simulations, continuous animations |
| **Event-Driven / Power-Saving** | Calls `CelsEngineRecompose(&engine)` only when `CelsEngineNeedsRecompose(&engine)` is `true` | Native desktop GUIs, toolbars, dialogs, battery-saving apps |

`CelsEngineNeedsRecompose(&engine)` returns `true` when:
1. Primary or secondary sessions have uncomposed root trees.
2. Watched reactive state cells have been invalidated via `cel_mutate`.
3. Discrete tree events (`cel_event`), signals (`cel_signal`), or broadcasts (`cel_broadcast`) are queued.
4. Active fiber tasks (`CEL_Task`) have expired delays or are ready to resume.
5. Code hot-reload is pending (`CelsSessionHotReload`).

---

## 4. Multi-Session & Multi-DLL Hosting

A single host can load multiple independent modules simultaneously into dedicated sessions:

```c
CelsEngine engine;
CelsEngineInitWithProfile(&engine, NULL, CELS_PROFILE_1K);

/* Create secondary session for auxiliary tool/inspector window */
CelsSession *mainSession   = cel_get_session(&engine, "main");
CelsSession *editorSession = CelsEngineCreateSession(&engine, "editor", CELS_PROFILE_2K);

CelsApp gameApp;
CelsApp editorApp;

/* Load dynamic modules independently into target sessions */
CelsAppLoad(&gameApp, &engine, mainSession, "game_app");
CelsAppLoad(&editorApp, &engine, editorSession, "editor_tooling");

/* Start applications (runs onStart and initial composition) */
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
- **Selective Entry Symbols**: Multiple app definitions can reside in a single DLL and be loaded individually via `CelsAppLoadEntry(app, engine, session, dllPath, "CelsGetAppManifest_SubApp")`.

---

## 5. CMake Configuration

CELS provides built-in CMake helper macros: `cels_add_application`.

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
    APP cel_dll
    HOST_SOURCES
        src/host.c
    APP_SOURCES
        src/app.c
        src/composition/main_window.c
    INCLUDES
        include
        src
)
```

### IDE Run Configurations (CLion / Visual Studio)
Two standard run configurations are provided for each app:
- **`**_host`**: Launches the main engine executable (`cel_host.exe`).
- **`**_dll`**: Recompiles the dynamic application library (`cel_dll.dll`) and displays hot-reload confirmation with exit code 0.

---

## 6. Hot Reload vs. Monolithic Build Modes

CELS adapts automatically based on the `CELS_HOT_RELOAD` CMake option:

### Hot Reload Mode (`CELS_HOT_RELOAD=ON`, default in Debug)
- Application builds as a shared dynamic library (`cel_dll.dll` / `libcel_dll.so`).
- Host monitors the file write timestamp via `CelsAppRuntimeCheck`.
- When you edit and recompile `cel_dll`, the host:
  1. Detects the new timestamp.
  2. Copies the new DLL to a shadow temporary file (avoiding Windows file locks).
  3. Reloads the symbols and refreshes lifecycle function pointers (`CelsSessionUpdateLifecycle`).
  4. Triggers an immediate recompose, preserving the entire session state and slot memory!
- On host exit, temporary shadow `.dll` files are automatically cleaned up.

### Monolithic Mode (`CELS_HOT_RELOAD=OFF`, default in Release)
- Application sources are statically linked directly into `cel_host`.
- No DLLs are created or loaded at runtime.
- Maximum performance, zero runtime dynamic loading overhead, and simple single-executable distribution.

---

## 7. Core Patterns & Anti-Patterns (Best for LLMs)

### Pattern 1: Application Pair Declaration in CMake
```cmake
# WRONG: Creating 3rd custom targets or separate app aliases
cels_add_application(HOST my_host APP my_dll ...)
add_custom_target(my_app DEPENDS my_dll) # Bad! Confuses IDE configurations and generates redundant targets

# CORRECT: Strictly two targets per application (host and dll)
cels_add_application(
    HOST cel_host
    APP cel_dll
    HOST_SOURCES src/host.c
    APP_SOURCES src/app.c
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

### Pattern 3: Application Manifest Export
```c
// WRONG: Using manual symbol exports, hidden magic macros, or custom structs
__declspec(dllexport) void* MyCustomInit() { ... }

// CORRECT: Canonical Application Manifest with root session attachment
static void App_OnStart(CelsSession *session) {
    cel_attach(session, MainWindowComposition, MainWindowEval);
}

CEL_App_Manifest(MainWindowApp,
    .title = "Window App",
    .appVersion = "1.0.0",
    .onStart = App_OnStart,
    .onReload = App_OnStart,
    .onEnd = NULL
);
```
