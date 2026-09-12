# CELS (Composition, Evaluation, Lifecycle, State)

> A fast, declarative composition and reactive state engine for C99.

**CELS** (**C**omposition, **E**valuation, **L**ifecycle, **S**tate) brings declarative component composition and fine-grained reactivity to C99. It runs inside a single, L1 cache-aligned memory slab with zero heap allocations at runtime and $O(1)$ subtree skipping.

---

## Quick Example

```c
#define CELS_IMPLEMENTATION
#include "cels.h"
#include <stdio.h>

/* 1. Define reactive state */
CEL_State(CounterState) {
    int count;
};

/* 2. Define a composable component */
CEL_Composable(CounterWidget, CounterState*, counter) {
    // Persistent local memory across recompositions
    int *renders = cel_remember(int, 0);
    (*renders)++;

    // Reactive subscription: re-runs when 'counter' mutates
    CounterState state = cel_watch(counter);
    printf("Count: %d (Rendered %d times)\n", state.count, *renders);
}

/* 3. Define composition root and lifecycle */
CEL_Composition(AppRoot, key) {
    CounterState init = { .count = 0 };
    CounterState *counter = cel_lifecycle_state(init, NULL, NULL);

    CounterWidget(counter);
}

CEL_LifeCycle(AppLifeCycle, CounterState) {
    if (it != NULL) {
        CounterState state = cel_watch(it);
        if (state.count < 0) {
            cel_destroy(); // Clean up and prune component tree
        }
    }
}

/* 4. Run and mutate */
int main(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);

    // Initial Mount
    CEL_Attach(&session, AppRoot, AppLifeCycle);
    CelsSessionRecompose(&session); // Prints: Count: 0 (Rendered 1 times)

    // Retrieve state by key and mutate it
    CounterState *c = CEL_GetState(&session, CEL_KEY("AppRoot"), CounterState);
    cel_mutate(&session, c) {
        this->count = 10;
    }

    // Recompose only updates affected components
    CelsSessionRecompose(&session); // Prints: Count: 10 (Rendered 2 times)

    CelsSessionDestroy(&session);
    return 0;
}
```

---

## What Things Do

| Feature | What It Does |
|---|---|
| `CEL_State(Name)` | Declares a reactive data struct. |
| `CEL_Composable(Name, ...)` | Defines a reusable component that can read state and remember local variables. |
| `CEL_Composition(Name, key)` | Defines a root composition boundary and entry point. |
| `cel_watch(state_ptr)` | Reads state and automatically subscribes the current component to updates. |
| `cel_mutate(session, state_ptr) { ... }` | Modifies state and marks subscribed components for recomposition. |
| `cel_remember(Type, init)` | Preserves component-local memory across recomposition passes. |
| `cel_lifecycle_state(init, onCreate, onDestroy)` | Allocates managed state with mount and unmount resource callbacks. |
| `CEL_LifeCycle(Name, Type)` | Defines a lifecycle evaluator to despawn/prune components via `cel_destroy()`. |
| `CEL_GetState(&session, key, Type)` | Queries live state directly from the session without global variables. |
| `CEL_Attach(&session, Comp, Lifecycle)` | Registers a root composition and lifecycle evaluator to a session. |
| `CelsSessionRecompose(&session)` | Runs a recomposition pass over invalidated components. |

---

## How It Works (High Level)

1. **Mount**: You compose your tree of components. CELS stores structural groups and remembered variables contiguously in an L1 cache-aligned memory slab.
2. **Mutate**: Modifying data inside `cel_mutate` diffs the memory against a snapshot and marks only the subscribed components as dirty.
3. **Recompose**: `CelsSessionRecompose()` re-runs only the dirty components. Unchanged components and entire subtrees are skipped in $O(1)$ time. If nothing changed, it does 0 work.

---

## Build & Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

---

## Architecture Guide

For in-depth details on the dual gap-buffer slot table, memory slab partitioning, reactivity diffing, and traversal mechanics, see [docs/architecture-guide.md](docs/architecture-guide.md).
