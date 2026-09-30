# C99 Coding Standards & Architectural Conventions

> **CELS (Composition, Evaluation, Lifecycle, State)**  
> Standards, idioms, macro preprocessor safety, and portability guidelines for high-performance C99 systems.

---

## 1. Language Standard & Compiler Toolchain

- **Target Standard**: Strict **ISO C99** (`-std=c99` or `/std:c99`).
- **Compiler Flags**:
  - GCC / Clang: `-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter`
  - MSVC: `/W4 /WX /permissive-`
- **Standard Types**:
  - Use fixed-width types from `<stdint.h>` (`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `int32_t`, `int64_t`) whenever memory width, packing, or serial layout matters.
  - Use `size_t` for memory sizes and byte lengths; `uintptr_t` for pointer-to-integer conversions; `ptrdiff_t` for pointer offsets.
  - Use `bool`, `true`, and `false` from `<stdbool.h>`.
- **Compiler Extensions**:
  - Never allow raw, unguarded compiler extensions in public headers (`include/`).
  - Extensions (e.g. `__typeof__`, `__attribute__((unused))`, `__declspec(align(64))`) must be wrapped in portability macros and guarded by preprocessor feature checks.

---

## 2. Naming Conventions

CELS employs a tiered prefix convention to clarify calling scope, API visibility, and execution context at a glance.

### 2.1 Tiered Identifier Prefixes

| Tier / Category | Prefix Convention | Example Symbols | Calling Context & Purpose |
| :--- | :--- | :--- | :--- |
| **Declarative DSL Macros** | `CEL_` (`SCREAMING_SNAKE` or `PascalCase`) | `CEL_State`, `CEL_Composable`, `CEL_Task`, `CEL_Lifecycle`, `CEL_ID` | Top-level structural declarations at file/global scope. |
| **Runtime Composable DSL** | `cel_` (`snake_case`) | `cel_remember`, `cel_watch`, `cel_mutate`, `cel_transition`, `cel_stage_set` | Invoked inside composables, tasks, or event callbacks during recomposition. |
| **Public C API Functions** | `Cels` (`PascalCase`, noun-first) | `CelsEngineInit`, `CelsSessionRecompose`, `CelsSlotTableInit`, `CelsStateWatch` | Host-level functions, session operations, and slot table plumbing. |
| **Internal Helpers & Body** | `_cels_` or `cels_` (`snake_case`) | `_cels_body_*`, `_cels_lifecycle_*`, `cels_app_runtime_check` | Private implementation functions, static trampolines, and helper macros. |
| **Constants & Enums** | `CELS_` (`SCREAMING_SNAKE`) | `CELS_OK`, `CELS_MAX_GROUPS`, `CELS_ENGINE_MAGIC`, `CELS_OP_SET` | Preprocessor limits, magic numbers, and enumeration constants. |

### 2.2 Case and Granularity Rules

- **Functions**: Read **noun-first, verb-last**:
  - `CelsSlotTableInit`, not `CelsInitSlotTable`.
  - `CelsSessionRecompose`, not `CelsRecomposeSession`.
  - *Why*: Groups all related operations for a data type together alphabetically in documentation and editor autocomplete.
- **Parameters & Local Variables**: Use `camelCase`:
  - `uint32_t logicalIndex;`, `float currentHealth;`
- **Out-Parameters**: Always prefixed with `out` in `camelCase`:
  - `uint32_t *outGroupIndex;`, `CelsSlotGroup *outGroup;`
  - *Why*: The type `uint32_t*` indicates a pointer; the `out` prefix indicates that the function mutates caller-owned memory at that address.
- **Internal Identifiers**: Entities and slots are identified by 64-bit integer hashes (`uint64_t key = CEL_ID("Name")`), never strings. String comparisons (`strcmp`) are prohibited on hot composition paths.

---

## 3. Types and Struct Layout

### 3.1 Typedef Tag Matching

Always `typedef` structs at definition, matching the struct tag name to the typedef identifier:

```c
typedef struct CelsSlotGroup {
    uint64_t key;
    uint64_t userData;
    uint32_t parentIndex;
    uint32_t slotIndex;
    uint16_t slotCount;
    uint16_t groupSize;
    uint16_t nodeCount;
    uint16_t flags;
} CelsSlotGroup;
```

Never write `struct CelsSlotGroup` in declarations once the typedef exists. Type names are single tokens.

### 3.2 Descending Field Ordering (Zero Padding)

Order struct members from **largest alignment to smallest alignment** (`uint64_t` / pointers $\rightarrow$ `uint32_t` / `float` $\rightarrow$ `uint16_t` $\rightarrow$ `uint8_t` / `bool`). This eliminates compiler-inserted structure padding bytes.

```c
/* Correct: Exactly 32 bytes (2 structs per 64-byte CPU cache line) */
typedef struct CelsTransitionState {
    uint64_t     startTimeMs;    /* 8 bytes (offset 0)  */
    CelsEasingFn easing;         /* 8 bytes (offset 8)  */
    float        current;        /* 4 bytes (offset 16) */
    float        startVal;       /* 4 bytes (offset 20) */
    float        targetVal;      /* 4 bytes (offset 24) */
    uint32_t     durationMs;     /* 4 bytes (offset 28) */
    bool         isInitialized;  /* 1 byte  (offset 32) */
    bool         isSettled;      /* 1 byte  (offset 33) */
    uint8_t      reserved[6];    /* 6 bytes (offset 34) -> aligned to 40 or 32 B */
} CelsTransitionState;
```

### 3.3 Cache-Line Alignment & Slab Storage

Hardware cache lines on x86-64 and ARM64 are 64 bytes. State memory slabs and slot tables must be aligned to 64-byte boundaries to avoid false sharing and misaligned vector loads:

```c
#if defined(_MSC_VER)
    #define CEL_SLAB(name, size) __declspec(align(64)) uint8_t name[size]
#else
    #define CEL_SLAB(name, size) __attribute__((aligned(64))) uint8_t name[size]
#endif
```

Headers preceding state instances (`CelsStateHeader`) and structural groups (`CelsSlotGroup`) are sized to exactly 32 bytes, packing precisely two entries per 64-byte cache line.

---

## 4. Macro Design & Preprocessor Safety

CELS uses declarative macros to provide modern ergonomics in pure C99. Because the C preprocessor operates on lexical tokens before compiler parsing, macro design must strictly follow comma safety and hygiene rules.

### 4.1 The Preprocessor Comma Problem

In C99, macro argument dispatch splits tokens on every comma that is not enclosed within parentheses `(...)`. A compound literal or designated initializer containing commas:

```c
/* FAILS on naive macros: Preprocessor sees 3 arguments instead of 2! */
cel_remember(Point, (Point){ .x = 10, .y = 20 });
```

The preprocessor parses `Point`, `(Point){ .x = 10`, and ` .y = 20 }` as separate arguments, triggering compilation errors such as `macro "cel_remember" passed 3 arguments, but takes just 2`.

### 4.2 Three Comma-Safety Idioms

CELS mandates three distinct strategies to ensure comma safety across all DSL macros:

#### Strategy A: Double-Parenthesized Compound Literals

When passing struct literals to macros with fixed arity, wrap the literal in outer parentheses:

```c
#define cel_remember_state(id, Type, defaultVal) \
    ((Type*)CelsSessionRememberState(CelsGetCurrentSession(), (id), sizeof(Type), &(defaultVal)))

/* Usage with outer parentheses: */
WindowState *win = cel_remember_state(
    CEL_ID("Win"),
    WindowState,
    ((WindowState){ .width = 1280, .height = 720 })
);
```

#### Strategy B: Variadic Argument Absorption (`__VA_ARGS__`)

For macros where the struct initializer is the trailing argument, define the macro as variadic (`...`) and forward `__VA_ARGS__` directly into a typed compound literal:

```c
#define cel_stage_set(targetId, Type, ...) \
    CelsSessionStageSet(CelsGetCurrentSession(), \
                        (uint64_t)(targetId), \
                        CelsHashKey(#Type), \
                        sizeof(Type), \
                        &(Type)__VA_ARGS__)

/* Usage: Commas inside the braces are safely swallowed by __VA_ARGS__ */
cel_stage_set(playerId, Transform, { .x = 10.0f, .y = 20.0f, .z = 0.0f });
```

#### Strategy C: Array Literal Casting Wrappers

When a macro initializes memory via an internal pointer without forcing dynamic allocation, use a single-element compound array literal `(const Type[]){ Init }`:

```c
#define _CEL_REMEMBER_2(Type, Init) \
    ((Type*)CelsResolveSlotWithCleanup(CelsGetCurrentSession(), sizeof(Type), (const Type[]){ Init }, NULL))
```

This guarantees that:
1. `Init` is evaluated as an initializer expression.
2. The compiler stores the default values in temporary storage.
3. Its address `(const Type[])` is safely passed to the slot initialization function.

### 4.3 Macro Arity Dispatch Idiom

CELS uses position-based dispatcher macros to support overloaded macro signatures in pure C99:

```c
#define _CEL_GET_MACRO_3(_1, _2, _3, NAME, ...) NAME

#define cel_watch(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_WATCH_3, _CEL_WATCH_2, _CEL_WATCH_1)(__VA_ARGS__)
```

- When 1 argument is supplied: `_CEL_WATCH_1` is chosen (hoisted instance pointer subscription).
- When 2 arguments are supplied: `_CEL_WATCH_2` is chosen (`cel_watch(Type, id)`).
- When 3 arguments are supplied: `_CEL_WATCH_3` is chosen (`cel_watch(session, Type, id)`).

### 4.4 Statement Macro Hygiene

- **Side-Effect Isolation**: Multi-statement macros must be enclosed in `do { ... } while(0)` so they behave as a single statement and safely integrate with `if (...) ... else ...`:
  ```c
  #define cel_yield() \
      do { \
          CelsTaskYield(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, 0); \
          CelsFiberSwitch((CelsFiber*)_cels_task_state->callerFiber); \
      } while(0)
  ```
- **Scoped Block Mutation**: For scoped block macros providing a `this` pointer (such as `cel_mutate`), use single-iteration `for` loops:
  ```c
  #define _CEL_MUTATE_1(ptr) \
      for (_CEL_MUTATE_TYPE(ptr) *this = (_CEL_MUTATE_TYPE(ptr)*)CelsMutateStateInstance((void*)(ptr)); \
           this != NULL; \
           this = NULL)
  ```
  This creates an isolated lexical scope, provides the typed `this` pointer, and automatically completes in exactly one iteration.

---

## 5. Memory Management & Ownership

### 5.1 Zero-Heap Hot Recomposition Rule

During active recomposition passes (`CelsSessionRecompose`, `CEL_Composable`), **no dynamic heap allocations (`malloc`, `calloc`, `free`) may occur**.

- All private local state is allocated from the session's slot gap buffer or nonmoving data arena via `cel_remember()` or `cel_state()`.
- All reactive shared state is resolved from the pre-allocated `CelsStateRegistry` slab.
- All transaction messages are staged into the pre-allocated `CelsTransactionBatch` byte arena.

*Why*: Eliminates allocator lock contention, heap fragmentation, and non-deterministic frame times, guaranteeing real-time 60–240 FPS frame convergence.

### 5.2 Slot Arena Nonmoving Invariant

The session's data arena (`session->dataArena`) is an append-only, nonmoving arena during active composition. Memory addresses returned by:
- `cel_remember(Type, Init)`
- `cel_state(Type)`
- `cel_remember_state(Type, ...)` / `cel_remember_state_keyed(id, Type, ...)`

are guaranteed to remain **stable and nonmoving** across recompositions, tree re-orderings, and dynamic hot-reload cycles. Pointers may safely be retained for the lifetime of the composable.

### 5.3 Destructor Invariant & Reverse Teardown Order

When a composable leaves the active composition tree (e.g. branch becomes false or session terminates):
1. All destructors registered via `cel_remember(..., OnDestroy)`, `cel_lifecycle()`, or `CelsSessionRegisterLifecycle()` are invoked.
2. Destructors **must accept `NULL` pointers safely** without crashing.
3. Destructors are executed in **strictly reverse order of registration** (LIFO: last mounted, first destroyed).

---

## 6. Error Handling & Invariants

### 6.1 Result Codes

Every fallible function returns a `CelsResult` enum:

```c
typedef enum CelsResult {
    CELS_OK = 0,
    CELS_ERROR_INVALID_ARGUMENT,
    CELS_ERROR_OUT_OF_MEMORY,
    CELS_ERROR_CAPACITY_EXCEEDED,
    CELS_ERROR_INDEX_OUT_OF_BOUNDS,
    CELS_ERROR_INVALID_STATE,
    CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE
} CelsResult;
```

Rules:
- `CELS_OK` is **strictly 0**.
- Test success explicitly: `if (result != CELS_OK) goto cleanup;`. Never test with `if (!result)` or `if (result)`.
- Fallible functions write output to an out-parameter (`outPhysicalIndex`, `outGroup`). On failure, out-parameters are left unwritten.
- Every module provides a static string converter: `const char *CelsResultToString(CelsResult result);`.

### 6.2 Assertions vs Result Codes

- **Use `assert()` for programmer contract bugs**: Passing `NULL` where a non-null pointer is required, calling `cel_remember` outside an active session, or invalid state machine transitions.
  ```c
  assert(sess != NULL && "cel_remember called outside of an active CelsSession");
  ```
- **Use `CelsResult` for operational failures**: Out of slab memory, invalid user index, queue overflow, or convergence timeout.

The test: *Could a correct application trigger this condition?* If yes $\rightarrow$ `CelsResult`. If no $\rightarrow$ `assert()`.

---

## 7. Control Flow & Cleanup

CELS follows a hybrid control flow model:
- **Plain early return** before any resources are allocated or groups entered.
- **Single-exit `goto cleanup;`** once resources, locks, or group scopes have been established.

```c
CelsResult CelsSlotTableInit(CelsSlotTable *table,
                             void *slabMemory,
                             size_t slabSize,
                             uint32_t maxGroups)
{
    if (table == NULL || slabMemory == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    CelsResult result = CELS_OK;

    if (((uintptr_t)slabMemory & (CELS_CACHE_LINE_SIZE - 1u)) != 0) {
        result = CELS_ERROR_INVALID_ARGUMENT;
        goto cleanup;
    }

    /* Configure table fields ... */

cleanup:
    return result;
}
```

Rules:
- Destructors and free functions must accept `NULL` as a no-op.
- Set transferred resource handles to `NULL` immediately upon handing off ownership.
- Never jump backwards with `goto`. `goto` is restricted to forward jumps to `cleanup:`.

---

## 8. Immutability & `const` Correctness

- **Locals are `const` unless mutated**: Default all local variables to `const` (analogous to immutable bindings). Opt into mutability only when the variable is modified:
  ```c
  const uint64_t key = CelsHashKey("StatusBar");
  const CelsSlotGroup *group = CelsGetGroup(session, logicalIdx);
  ```
- **Read-Only Front Buffer Snapshots**: `cel_watch()` and `cel_get_state()` always return `const Type*`. Modifying published front-buffer state directly is undefined behavior.
- **Mutable Back Buffers**: Modifications must occur via `cel_mutate()`, which exposes a mutable pointer to the staging back-buffer and automatically enqueues observers for recomposition.

---

## 9. Portability & Compiler Compatibility

### 9.1 Cross-Platform Alignment

```c
#if defined(_MSC_VER)
    #define CELS_ALIGN_64 __declspec(align(64))
#elif defined(__GNUC__) || defined(__clang__)
    #define CELS_ALIGN_64 __attribute__((aligned(64)))
#else
    #define CELS_ALIGN_64
#endif
```

### 9.2 Thread-Local Storage

```c
#ifndef CELS_THREAD_LOCAL
    #if defined(_MSC_VER)
        #define CELS_THREAD_LOCAL __declspec(thread)
    #elif defined(__GNUC__) || defined(__clang__)
        #define CELS_THREAD_LOCAL __thread
    #elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
        #define CELS_THREAD_LOCAL _Thread_local
    #else
        #define CELS_THREAD_LOCAL
    #endif
#endif
```

### 9.3 Typeof Resolution for Scoped Mutation

In `cel_mutate(ptr)`, resolving the target type from the pointer is supported across GCC, Clang, MSVC 2019+, and C23:

```c
#if defined(__GNUC__) || defined(__clang__) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
    #define _CEL_MUTATE_TYPE(ptr) __typeof__(*(ptr))
#elif defined(_MSC_VER) && _MSC_VER >= 1930
    #define _CEL_MUTATE_TYPE(ptr) typeof(*(ptr))
#else
    #define _CEL_MUTATE_TYPE(ptr) void
#endif
```

---

## 10. Concurrency & Thread-Safety Invariants

1. **Session Single-Thread Invariant**:
   A `CelsSession` recomposition walk (`CelsSessionRecompose`, execution of composables, slot table writes) runs **single-threaded** on its designated host thread. No locks are acquired during recomposition.
2. **Lock-Free Front-Buffer Reads**:
   Front-buffer state (`cel_watch`, `cel_get_state`, `CelsStateGet`) represents an immutable published snapshot. It is completely lock-free and safe to read across threads.
3. **Transaction Staging Buffer**:
   Cross-thread mutations (e.g. background worker threads emitting physics or network updates) do not write to the active session slab directly. They stage operations via double-buffered transaction batches (`CelsSessionSwapTransactionBatches`), which are processed locklessly at the frame boundary.
4. **Subsystem Modules**:
   Host modules registered via `CEL_RegisterModule()` that are accessed across worker threads must implement their own internal synchronization.
