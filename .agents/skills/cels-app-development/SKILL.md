---
name: cels-app-development
description: "Application and Host creation guide for CELS. Use when building a new CELS application, setting up a host process, configuring CMake build targets, or working with hot-reloading."
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
|  - Declares CEL_App manifest                                       |
|  - Implements onStart / onEnd hooks                                |
|  - Defines Compositions & Composable UI trees                      |
+--------------------------------------------------------------------+
```

---

## 2. Creating an Application (`cel_app`)

An application requires:
1. One or more Compositions (e.g. `Window_GetComposition()`).
2. An `onStart` hook returning the primary composition reference.
3. An optional `onEnd` hook for application teardown.
4. The `CEL_App` manifest macro.

### Minimal Application Example (`app.c`)
```c
#include "cels.h"
#include "composition/window.h"
#include <stdio.h>

static CelsCompositionRef App_OnStart(CelsEngine *engine, CelsSession *session)
{
    (void)engine;
    (void)session;
    printf("[App] Application starting, mounting root window.\n");
    return Window_GetComposition();
}

static void App_OnEnd(CelsEngine *engine, CelsSession *session)
{
    (void)engine;
    (void)session;
    printf("[App] Application shutting down.\n");
}

/* CEL_App defines the exported application manifest */
CEL_App(WindowApp,
    .onStart = App_OnStart,
    .onEnd = App_OnEnd
);
```

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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* 1. Initialize engine (loads application manifest) */
    CelsEngine engine;
    if (CelsEngineInit(&engine, NULL) != CELS_OK) {
        fprintf(stderr, "[Host] Failed to initialize engine\n");
        return 1;
    }

    printf("[Host] Engine started. Entering main loop...\n");

    /* 2. Main loop */
    while (!engine.shouldQuit) {
        /* Check if application DLL was rebuilt and hot-swap if needed */
        CelsAppRuntimeCheck(&engine);

        /* Recompose all active sessions */
        CelsEngineRecompose(&engine);

        /* Example: Mutate state based on events / input */
        // cel_mutate(&engine.session, CEL_Window, WindowState) {
        //     this->someField = newValue;
        // }

        SleepMs(16); /* ~60 FPS */
    }

    /* 3. Clean teardown */
    printf("[Host] Shutting down...\n");
    CelsEngineEnd(&engine);
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
- **`**_host`**: Launches the main engine executable (`cel_host.exe`, `cel_task_host.exe`).
- **`**_dll`**: Recompiles the dynamic application library (`cel_dll.dll`, `cel_task_dll.dll`) and displays hot-reload confirmation with exit code 0.

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
