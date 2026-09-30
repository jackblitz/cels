# Reactive State Management

State management in CELS is designed to be **cache-friendly, lock-free, and type-safe in pure C99**.

CELS uses a **double-buffered reactive model**: reads are lock-free and hit the front buffer, while writes stage into the back buffer. Modern CELS emphasizes **State Hoisting** (`cel_state`, `cel_watch(ptr)`, `cel_mutate(ptr)`), eliminating string lookups, hash collisions, and manual key management.

---

## 1. The Double-Buffering Mental Model

Every reactive state cell in CELS maintains two memory buffers:

```
┌────────────────────────────────────────────────────────┐
│                   CELS STATE CELL                      │
│                                                        │
│  FRONT BUFFER (Committed Snapshot)                     │
│  [ x: 10, y: 20, isAlive: true ]                       │
│    ▲                                                   │
│    │ Lock-free reads during recomposition              │
│    │ `cel_watch(player)` or `cel_get_state(id, Type)`  │
│                                                        │
│  BACK BUFFER (Staged Modifications)                    │
│  [ x: 15, y: 20, isAlive: true ]                       │
│    ▲                                                   │
│    │ Staged mutations via `cel_mutate(player)`         │
│    │ (invalidates observing composables)               │
└────┼───────────────────────────────────────────────────┘
     │
     │ Frame boundary: Double buffers SWAP atomically!
     ▼
```

1. **Recomposition Phase (Reads)**: Composables call `cel_watch(ptr)` to read the stable front buffer. CELS records the calling composable group as a subscriber.
2. **Mutation Phase (Writes)**: User input, host tick, or tasks invoke `cel_mutate(ptr)`. The mutation applies to the back buffer, and all registered observers are flagged dirty.
3. **Commit Phase (Swap)**: At the frame boundary, buffers swap. Only the dirty composables recompose on the next tick.

---

## 2. Declaring State Structs (`CEL_State`)

Declare your state models at file or header scope using `CEL_State`:

```c
#pragma once
#include "cels.h"

CEL_State(PlayerState) {
    float posX;
    float posY;
    int   health;
    bool  isShieldActive;
};
```

`CEL_State(TypeName)` automatically creates a typedef and struct definition formatted for cache-alignment in CELS.

---

## 3. Idiomatic Pattern: State Hoisting (Zero String IDs)

The primary and most idiomatic pattern in CELS is **State Hoisting**:
1. Allocate state at the nearest common ancestor with `cel_state`.
2. Pass the typed pointer down to child composables via normal arguments.
3. Children subscribe by calling `cel_watch(ptr)`.
4. Anyone mutates the state directly with `cel_mutate(ptr)`.

### Step 1: Hoisting State with `cel_state`

Allocate state in the parent composition or container composable:

```c
CEL_Composition(GameView) {
    /* Pinned to this slot, auto-generated unique ID, auto-cleanup on unmount */
    PlayerState *player = cel_state(PlayerState, {
        .posX = 100.0f,
        .posY = 200.0f,
        .health = 100,
        .isShieldActive = false
    });

    /* Hoist state pointer down the tree */
    PlayerHUD(player);
    PlayerAvatar(player);
}
```

### Step 2: Subscribing with `cel_watch(ptr)`

In child composables, call `cel_watch(ptr)`. In $O(1)$ time, CELS inspects the header preceding the pointer and registers this composable to recompose whenever `player` changes:

```c
CEL_Composable(PlayerHUD, const PlayerState*, player) {
    /* Registers reactive subscription */
    cel_watch(player);
    if (!player) return;

    printf("  [HUD] Health: %d | Shield: %s\n",
           player->health, player->isShieldActive ? "ON" : "OFF");
}
```

### Step 3: Direct Mutation with `cel_mutate(ptr)`

Whenever an event occurs (keyboard press, network packet, collision), mutate the hoisted pointer directly:

```c
/* Direct mutation block: 'this' is a typed pointer to the back buffer */
cel_mutate(player) {
    this->health -= 15;
    if (this->health < 0) this->health = 0;
}
```

> [!TIP]
> Inside `cel_mutate(ptr) { ... }`, the compiler provides a typed `this` pointer pointing directly to the cell's mutable back buffer. You don't need to specify the type or pass a session pointer!

---

## 4. Local Slot Memory: `cel_remember`

When a composable needs local memory that persists across recompositions without being shared across the tree (e.g. click counts, UI scroll offsets, scratch buffers), use `cel_remember`:

```c
CEL_Composable(ClickCounter) {
    /* Preserved across frames in the composable's slot table gap buffer */
    int *clicks = cel_remember(int, 0);

    printf("Button clicked %d times\n", *clicks);
}
```

### Optional Destruction Hook

You can pass a third argument to `cel_remember` to clean up resources when the composable unmounts:

```c
static void FreeBuffer(void *instance, CelsSession *session CELS_UNUSED) {
    free(*(char**)instance);
}

CEL_Composable(DynamicWidget) {
    char **buffer = cel_remember(char*, malloc(256), FreeBuffer);
    /* buffer will be freed automatically if DynamicWidget is unmounted */
}
```

---

## 5. Session State & Type-Based Registry (`cel_remember_state`)

When state represents global application singleton data (e.g. window status, global audio settings, cross-session bridges), use session state.

### Initializing Singleton Session State (Zero IDs)
`cel_remember_state(Type, ...)` automatically derives a stable 64-bit key from the Type name via compile-time FNV-1a hashing:
```c
WindowState *win = cel_remember_state(WindowState, {
    .isOpen = true,
    .showBadge = true,
    .width = 800,
    .height = 600
});
```

### Reading Without Subscribing: `cel_get_state(Type)`
If you only need to inspect state without triggering recomposition on change (e.g. in evaluation predicates or host checks):
```c
CEL_Evaluation(WindowEval) {
    /* Does NOT subscribe WindowEval to future mutations */
    const WindowState *win = cel_get_state(WindowState);
    return (win == NULL || win->isOpen);
}
```

### Mutating From Host Loop: `cel_mutate(session, Type)`
From the host tick loop outside the DLL, address the state cell directly by its Type:
```c
cel_mutate(&engine.session, WindowState) {
    this->isOpen = false;
}
```

### Dynamic Keyed State: `cel_remember_state_keyed(id, Type, ...)`
When you have **multiple dynamic instances** of the same struct type that external systems need to address by ID (e.g. entity network sync):
```c
uint64_t player101 = CEL_ID("player_101");
PlayerState *p = cel_remember_state_keyed(player101, PlayerState, { .health = 100 });

cel_mutate_keyed(&engine.session, player101, PlayerState) {
    this->health -= 25;
}
```


---

## 6. Best Practices & Pitfalls

### Do This
- **Prefer State Hoisting**: Use `cel_state` and pass pointers down. It is faster, type-safe, eliminates string typos, and scopes state lifetime to the declaring composable.
- **Mutate in Event Callbacks**: Mutate state in response to inputs, task yields, or external callbacks. Avoid mutating state directly in the render/composition body to prevent infinite recomposition loops.
- **Read Constantly, Mutate Once**: Read data freely with `cel_watch`. Multiple reads in the same frame are lock-free and hit the front buffer.

### Don't Do That
- ❌ **Never Cast Away `const` to Write Directly**:
  ```c
  /* DANGEROUS: Bypasses double-buffering, causes tearing, drops reactivity! */
  PlayerState *p = (PlayerState*)cel_watch(player);
  p->health = 50; 
  ```
  *Always use `cel_mutate(player) { this->health = 50; }`.*
- ❌ **Don't Create Global IDs for Local Widgets**:
  Avoid creating `#define CEL_MyButton1 CEL_ID(...)` for every minor widget. Use `cel_remember` for private component state or `cel_state` for parent-child groups.

---

## Next Steps

Now that you've mastered state:
- Proceed to [04. Resource Lifecycles](04-lifecycles.md) to manage native GPU handles, audio voices, and system resources tied to composable mounting and unmounting.
