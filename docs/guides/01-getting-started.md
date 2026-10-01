# Getting Started with CELS

Welcome to **CELS** (C99 Composition, Evaluation, Lifecycle, and State engine)! CELS is a lightweight, high-performance declarative composition framework for pure C99. Inspired by slot-table architectures (such as Jetpack Compose) and double-buffered reactive systems, CELS brings modern declarative UI and state-driven architecture to C without requiring C++, external runtimes, or heavy dependencies.

In this 5-minute guide, you will set up a minimal CELS project, configure CMake with `cels_add_application`, write a minimal host and application, and see live hot-reloading in action.

---

## 1. Mental Model: Host vs. Application

CELS separates your software into two distinct components:

```
┌────────────────────────────────────────────────────────────┐
│ HOST EXECUTABLE (cel_host)                                 │
│ - Entry point (`main`) & platform loop                     │
│ - Owns `CelsEngine` and ticks sessions                     │
│ - Checks for rebuilds with `CelsAppRuntimeCheck`           │
└─────────────────────────────┬──────────────────────────────┘
                              │ loads & swaps at runtime
┌─────────────────────────────▼──────────────────────────────┐
│ APPLICATION MODULE (cel_dll)                               │
│ - Exports `CEL_App` manifest                               │
│ - Defines Compositions & Composable UI trees               │
│ - Mounts lifecycles, states, and tasks                     │
└────────────────────────────────────────────────────────────┘
```

During development (`CELS_HOT_RELOAD=ON`), the application compiles into a dynamic library (`.dll` / `.so`). The host automatically detects code updates, reloads the library, and recomposes without resetting runtime state. In release builds (`CELS_HOT_RELOAD=OFF`), everything compiles into a single monolithic binary.

---

## 2. Directory Layout

A standard CELS application is structured cleanly:

```text
my_cels_app/
├── CMakeLists.txt
├── src/
│   ├── host.c           # Host runner & main loop
│   ├── app.c            # Application entry & manifest
│   ├── app_state.h      # Shared state definitions
│   └── main_view.c      # Root composition & widgets
```

---

## 3. Step 1: CMake Configuration

CELS provides a helper function `cels_add_application` that configures both the host executable and the reloadable application library in one shot.

Create your `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_cels_app C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find or include CELS
find_package(CELS REQUIRED)

# Declare host executable and dynamic application target
cels_add_application(
    HOST cel_host
    DLL  cel_dll
    HOST_SOURCES
        src/host.c
    APP_SOURCES
        src/app.c
        src/main_view.c
    INCLUDES
        src
)
```

> [!NOTE]
> `cels_add_application` automatically configures output directories, compiler export flags, and sets up shadow-copy hot-reloading targets for your IDE.

---

## 4. Step 2: The Application (`src/app.c`)

Your application module declares its root composition cleanly using the declarative `CEL_App` macro:

```c
#include "cels.h"

/* Forward declaration of your root composition */
void MainView(void *userData);

/* Declarative root: binds MainView as the application root composition */
CEL_App(MyApplication, MainView);
```

> [!TIP]
> If your application root requires a lifecycle evaluation predicate (to control when it mounts or despawns), pass it as the second argument: `CEL_App(MyApplication, MainView, MainEval)`. For advanced custom manifest hooks (such as `.continuousCompose = true` or cleanup callbacks), use `CEL_App_Manifest(MyApplication, ...)`.

Now create `src/main_view.c` to declare your root composition and a simple composable:

```c
#include "cels.h"
#include <stdio.h>

/* A reusable composable node */
CEL_Composable(GreetingWidget, const char*, name) {
    int *renderCount = cel_remember(int, 0);
    (*renderCount)++;

    printf("  Hello, %s! (Rendered %d times)\n", name, *renderCount);
}

/* The top-level composition root */
CEL_Composition(MainView) {
    GreetingWidget("Developer");
}
```

---

## 5. Step 3: The Host Process (`src/host.c`)

The host manages process startup, the frame tick loop, and clean shutdown:

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

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* 1. Initialize engine (loads application manifest) */
    CelsEngine engine;
    if (CelsEngineInit(&engine, CELS_APP_TARGET) != CELS_OK) {
        fprintf(stderr, "[Host] Failed to initialize engine.\n");
        return 1;
    }

    printf("[Host] Engine running. Press Ctrl+C to stop.\n");

    /* 2. Main frame tick loop */
    while (!engine.shouldQuit) {
        /* Check if application DLL was recompiled on disk and hot-swap */
        CelsAppRuntimeCheck(&engine);

        /* Recompose active sessions */
        CelsEngineRecompose(&engine);

        SleepMs(16); /* ~60 FPS */
    }

    /* 3. Clean teardown */
    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);
    return 0;
}
```

---

## 6. Building and Running

Configure and build with CMake:

```bash
cmake -B build -S .
cmake --build build --config Debug
```

Run your host:
```bash
./build/cel_host
```

Output:
```text
[App] Application starting, mounting MainView...
[Host] Engine running. Press Ctrl+C to stop.
  Hello, Developer! (Rendered 1 times)
```

Now edit `GreetingWidget` in `src/main_view.c` to print `"Greetings, Commander!"` and recompile only the `cel_dll` target:

```bash
cmake --build build --target cel_dll --config Debug
```

Watch the running host console—without restarting the process:
```text
[Host] Detected module change on disk. Reloading cel_dll...
[Host] Hot-reload complete. Recomposing...
  Greetings, Commander! (Rendered 2 times)
```

Notice that `renderCount` kept its state across the reload! CELS slot tables preserve local state seamlessly across DLL reloads.

---

## 7. Best Practices & Pitfalls

### Do This
- **Keep Host Minimal**: Keep platform glue, OS message pumps, and window management in `host.c`. Place all visual, layout, and UI logic in the application.
- **Use `cel_remember` for Persisted Data**: Variables declared inside composables should use `cel_remember` to survive recomposition and hot reloads.
- **Pass Context via Composition References or Ambient Session**: Pass host resources (like GPU contexts or custom memory allocators) through `userData` in `CelsCompositionRef` / `cel_root_with_data` or retrieve ambient session context using `cel_user_data(Type)`.

### Don't Do That
- **Don't Allocate Heap Memory in Composables without Cleanup**: Never call raw `malloc()` in a composable body without pairing it with an unmount destructor or `cel_lifecycle`.
- **Don't Block the Host Tick**: Never call `sleep()` or synchronous network calls inside a composable. Use `CEL_Task` with `cel_wait(ms)` instead.

---

## Next Steps

Now that your project compiles and runs:
- Head over to [02. Compositions and the Tree Hierarchy](02-compositions-and-tree.md) to understand how composables nest, take properties, and handle conditional rendering.
