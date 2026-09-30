# Live Code Hot-Reloading

One of the standout features of CELS is **instantaneous, state-preserving live code hot-reloading in pure C99**.

You can modify UI layouts, adjust styling, tweak transition durations, and alter composable hierarchies in your editor, hit save and recompile, and see your running application update immediately—**without losing state, without restarting the process, and without resetting the window or simulation**.

---

## 1. How Hot-Reloading Works in CELS

Hot-reloading in CELS relies on a strict boundary between the **Host** and the **Application Module**:

```
┌────────────────────────────────────────────────────────────┐
│ HOST PROCESS (cel_host)                                    │
│  - Statically owns CelsEngine, CelsSession, and Memory     │
│  - Slot Table, Gap Buffer, and State Registry stay pinned! │
│  - Frame tick loop runs continuously                       │
│                                                            │
│   1. Recompile cel_dll                                     │
│         │                                                  │
│   2. CelsAppRuntimeCheck detects new timestamp             │
│         │                                                  │
│   3. Shadow copy created (avoids Windows OS file locks)    │
│         │                                                  │
│   4. dlopen / LoadLibrary loads new binary                 │
│         │                                                  │
│   5. Lifecycle destructor pointers updated                 │
│         │                                                  │
│   6. Immediate recomposition with existing slot memory!    │
└────────────────────────────────────────────────────────────┘
```

Because your **Slot Table** and **State Registry** live in host memory, reloading the application code simply replaces the execution logic. The existing state slots remain untouched!

---

## 2. Setting Up the Host Loop

In your `host.c`, invoke `CelsAppRuntimeCheck` on every iteration of the main loop before recomposing:

```c
#include "cels.h"
#include "cels/engine.h"

int main(void) {
    CelsEngine engine;
    if (CelsEngineInit(&engine, NULL) != CELS_OK) return 1;

    while (!engine.shouldQuit) {
        /* 1. Poll disk for updated application binary */
        CelsAppRuntimeCheck(&engine);

        /* 2. Recompose active sessions */
        CelsEngineRecompose(&engine);

        SleepMs(16);
    }

    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);
    return 0;
}
```

- **`CelsAppRuntimeCheck(&engine)`**: Inspects the write timestamp of `cel_dll.dll` / `libcel_dll.so`. If newer than the loaded module, it pauses recomposition, swaps the library, refreshes symbols, and marks all active compositions as dirty.
- **Shadow Copy Mechanism**: On Windows, the OS locks active DLL files from being overwritten. CELS automatically copies the newly built DLL into a temporary shadow file before loading, allowing your compiler to overwrite the target `.dll` at any time!

---

## 3. Preserving State Across Swaps

Consider this composable with a click counter:

```c
CEL_Composable(CounterWidget) {
    int *count = cel_remember(int, 0);
    (*count)++;

    printf("  [Counter] Current count: %d\n", *count);
}
```

1. Run the host. `CounterWidget` counts: `1`, `2`, `3`, `4`, `5`...
2. Edit `CounterWidget` in your editor to change the format string:
   ```c
   printf("  >>> UPDATED COUNT: %d <<<\n", *count);
   ```
3. Recompile the DLL.
4. The host detects the change and prints:
   ```text
   [Host] Module update detected. Reloading cel_dll...
   [Host] Hot-reload complete.
     >>> UPDATED COUNT: 6 <<<
   ```

`*count` preserved its value (`6`)! It did **not** reset to `0`.

---

## 4. Lifecycle Pointer Safety

When a shared library is reloaded, function code is mapped to new memory addresses. If your slot table held pointers to old unmount destructors, executing them would trigger a segmentation fault or access violation.

CELS eliminates this problem via `CelsSessionUpdateLifecycle`:
- During reload, CELS traverses the active slot table.
- Any registered lifecycle handlers (`CEL_Lifecycle`) are automatically re-bound to the new function addresses in the freshly loaded DLL.
- Subsequent unmount operations safely execute the updated destructors.

---

## 5. Build Modes: Development vs. Release

CELS uses CMake options to switch between dynamic hot-reloading and monolithic release binaries:

```cmake
# In your CMakeLists.txt (or via CLI)
option(CELS_HOT_RELOAD "Enable dynamic hot-reload mode" ON)
```

| Feature | `CELS_HOT_RELOAD=ON` (Debug) | `CELS_HOT_RELOAD=OFF` (Release) |
| :--- | :--- | :--- |
| **Application Target** | Shared Library (`cel_dll.dll` / `.so`) | Static object linked into host |
| **Hot-Reloading** | Yes (sub-second swaps) | No (monolithic binary) |
| **Distribution** | Requires host executable + DLL | Single standalone executable |
| **Performance** | Dynamic dispatch | Direct static inlining & LTO |

---

## 6. Developer Workflow (CLion / VS Code / Command Line)

### Command-Line Iteration
In terminal 1, run the host:
```bash
./build/cel_host
```

In terminal 2, after making edits in `src/`:
```bash
cmake --build build --target cel_dll
```
The host instantly reloads the code.

### CLion Setup
`cels_add_application` automatically generates two targets in your IDE:
1. **`cel_host`**: Click **Run** or **Debug** to start the main process.
2. **`cel_dll`**: Set up a keyboard shortcut (e.g. `Ctrl+F9` or `Ctrl+B`) to build this target. As soon as the build finishes, your running window reflects the changes.

---

## 7. Best Practices & Pitfalls

### Do This
- **Keep Subsystem Singletons in the Host**: Store heavy host resources (like `SDL_Window`, `GLFWwindow`, Vulkan devices, or audio device contexts) in the host or register them via `CEL_Module`.
- **Use Fixed Struct Layouts for State**: Keep state structs in shared headers (`.h`). If you add or remove fields from a `CEL_State` struct while the app is running, restart the host to ensure memory alignment matches.
- **Keep Application Code in `APP_SOURCES`**: Place all UI, tasks, transitions, and compositions in `APP_SOURCES` so they live in the reloadable module.

### Don't Do That
- ❌ **Don't Store Static Global Function Pointers**: Avoid storing raw function pointers in `cel_remember` slots without updating them. Use `cel_lifecycle` for resource destructors.
- ❌ **Don't Put Host Loop Code in the Application**: Keep `main()` and OS event pumps in the host executable.

---

## Conclusion & Further Reading

Congratulations! You now have a complete understanding of CELS architecture:
1. [01. Getting Started](01-getting-started.md) — 5-minute setup and minimal host
2. [02. Compositions & Tree](02-compositions-and-tree.md) — Structural slot hierarchy
3. [03. Reactive State](03-reactive-state.md) — State Hoisting and double-buffering
4. [04. Resource Lifecycles](04-lifecycles.md) — Native resource mount/unmount
5. [05. Tasks & Coroutines](05-tasks-and-coroutines.md) — Cooperative fibers and async flows
6. [06. Transitions & Motion](06-transitions-and-motion.md) — Declarative interpolation and easing
7. [07. Live Code Hot-Reloading](07-hot-reloading.md) — Instant zero-loss iterations
