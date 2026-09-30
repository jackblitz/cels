<p align="center">
  <h1 align="center">CELS</h1>
  <p align="center"><strong>Declarative, Reactive Composition for Pure ANSI C99.</strong></p>
  <p align="center">Describe <strong>what</strong> should exist. CELS handles creation, destruction, state changes, and reconciliation automatically.</p>
  <p align="center">
    <img src="https://img.shields.io/badge/version-v0.3.0-blue" alt="version">
    <img src="https://img.shields.io/badge/license-Apache%202.0-green" alt="license">
    <img src="https://img.shields.io/badge/standard-C99-orange?logo=c" alt="C99">
    <img src="https://img.shields.io/badge/tests-61%20passing-brightgreen" alt="tests">
    <img src="https://img.shields.io/badge/allocations-0%20runtime%20heap-blueviolet" alt="zero-alloc">
  </p>
</p>

---

CELS reads like a DSL, compiles as pure C99, and runs anywhere. Define reactive states, compose hierarchical component trees, declare when nodes despawn with `CEL_Lifecycle`, and let fine-grained recomposition process your updates. No classes, no vtables, no runtime heap allocations. Just four concepts and a single `#include <cels/cels.h>`.

- **C** omposition — declare what components exist and how they're hierarchically structured
- **E** valuation — react to state changes with fine-grained recomposition and $\mathcal{O}(1)$ subtree skipping
- **L** ifecycle — automate creation, unmounting, and cascading cleanup; release native handles in lifecycle callbacks
- **S** tate — manage reactive data in cache-aligned memory slabs with snapshot diffing and persistent local memory (`cel_remember`)

```text
 State (CEL_State)               defines DATA
 Compositions (CEL_Composition)  defines STRUCTURE    (what exists)
 Reactivity (cel_watch/mutate)   detects CHANGE       (when things happen)
 Lifecycles (CEL_Lifecycle)      controls LIFETIME    (when things live and die)
 Composables (CEL_Composable)    evaluates RENDERING  (how nodes update)
```

Data flows in one direction:

```text
cel_mutate updates ──> cel_watch detects ──> Recomposition reacts (O(1) skip for unchanged)
```

```c
CEL_State(PlayerState) { float health; bool hasShield; };

CEL_Composable(HealthHUD, const PlayerState*, player) {
    cel_watch(player); // Automatically subscribes to changes

    // Smoothly converges visual display toward player->health over 300ms
    float visualHp = cel_transition(player->health, 300, CEL_EASE_OUT_QUAD);
    DrawHealthBar(visualHp);

    // Standard C control flow! When hasShield becomes false,
    // ShieldBadge unmounts and releases its GPU resources automatically.
    if (player->hasShield) {
        ShieldBadge();
    }
}
```

---

## What Makes CELS Different

Unlike traditional GUI and state frameworks that require manual dirty-flag bookkeeping, callback spaghetti, and event cascades, CELS introduces **declarative compositions**. You describe **what** should exist, and the engine reconciles the tree automatically.

Think React's or Jetpack Compose's component and slot-table model, engineered for native systems in **pure C99** with:

- ⚡ **Zero Runtime Heap Allocations**: All component structural groups, remembered variables, and reactive slots live inside an L1/L2 cache-aligned dual gap-buffer slab. Zero calls to `malloc()` or `free()` during frame recomposition.
- ⏭️ **$\mathcal{O}(1)$ Subtree Skipping**: If a parent state changes but a child's watched dependencies remain untouched, CELS skips entire subtrees in constant time.
- 🔥 **Sub-50ms Hot-Reloading in Development**: Run your host engine while editing application code live in CLion or your favorite IDE. Rebuilds swap in `<50ms` with **zero Windows DLL file locks** and full state retention.
- 📦 **Single Monolithic Binary in Production**: Switch to Release mode to compile host and app into a single standalone `.exe` with **zero `.dll` dependencies**.
- 🎯 **Type-Safe State Hoisting**: Pass state instances down component trees with standard C arguments—zero global string IDs or hash map lookups.
- 🧵 **Native Fiber Tasks & Coroutines**: Cooperative asynchronous workflows (`CEL_Task`, `cel_wait(ms)`) execute across frames without OS thread overhead.
- 🌊 **Declarative Motion & Transitions**: Continuous mathematical smoothing with built-in easing curves (`cel_transition`). Retargets mid-flight with zero visual popping and drops to 0% CPU when settled.
- 🛡️ **Zero C++ / Zero Vtables**: Pure ANSI C99 macros that expand to inline functions and static structs.

---

## Quickstart

### 1. Add to CMake
```cmake
include(FetchContent)
FetchContent_Declare(cels GIT_REPOSITORY https://github.com/jackblitz/cels.git GIT_TAG v0.3.0)
FetchContent_MakeAvailable(cels)

# In Debug: builds cel_host (.exe) and cel_dll (.dll) with sub-50ms live hot-reloading
# In Release: compiles everything into a single standalone monolithic .exe
cels_add_application(
    HOST cel_host
    DLL  cel_dll
    HOST_SOURCES src/host.c
    APP_SOURCES  src/app.c
)
```

### 2. Write Your Application (`src/app.c`)
```c
#include <cels/cels.h>

CEL_Composable(Greeting) {
    printf("Hello from live-reloaded CELS!\n");
}

CEL_Composition(AppRoot) {
    Greeting();
}

CEL_OnStart(App_OnStart) {
    cel_attach(session, AppRoot);
}

CEL_App(MyApp,
    .onStart = App_OnStart
);
```

### 3. Run and Live-Reload
```bash
# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Launch the host
./build/windows/cel_host.exe
```

In CLion or your IDE, select **`cel_dll`** and hit **Run** (<kbd>Shift</kbd>+<kbd>F10</kbd>) or **Build** (<kbd>Ctrl</kbd>+<kbd>F9</kbd>). Edit `src/app.c` and watch the live host update instantly!

---

## Examples & Documentation

Explore the included example applications:
- **[`examples/window`](examples/window)**: Multi-component desktop window with state hoisting and resource lifecycles.
- **[`examples/task`](examples/task)**: Async fiber coroutines, non-blocking delays, and network simulation.
- **[`examples/transition`](examples/transition)**: Smooth declarative transitions and easing curves.

Comprehensive guides and technical documentation are available in the **[`docs/`](docs/)** directory:
- [Developer Guides](docs/guides/): Step-by-step guides for getting started, state, tasks, and motion.
- [Architecture Deep Dives](docs/architecture/): Slot table internals, reconciliation, and lock-free double buffering.
- [API Reference](docs/reference/): Complete C99 macro and function cheat sheet.

---

## Testing & Verification

```bash
# Run all 63 tests across 11 architectural features
./build/windows/test_cli.exe all

# Run performance benchmark suite
./build/windows/benchmark.exe --quick
```

---

## License

Licensed under the [Apache License, Version 2.0](LICENSE). Copyright (c) 2026 Jack Blitz.
