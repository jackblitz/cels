# CELS Architecture Guide: Internal Design & Engine Mechanics

This guide provides a comprehensive technical walkthrough of the **CELS (Composition, Evaluation, Lifecycle, State)** composition engine. It covers the internal data structures, algorithms, memory layouts, traversal lifecycle, and reactivity mechanics that drive the runtime.

---

## Table of Contents

1. [Architectural Overview & Mental Model](#1-architectural-overview--mental-model)
2. [Memory Architecture: L1 Cache Slab](#2-memory-architecture-l1-cache-slab)
3. [The Slot Table & Dual Gap Buffer](#3-the-slot-table--dual-gap-buffer)
4. [Tree Traversal & Reconciliation Engine](#4-tree-traversal--reconciliation-engine)
5. [Reactive State & Invalidation Propagation](#5-reactive-state--invalidation-propagation)
6. [Persistent Memory & Lifecycle Hooks](#6-persistent-memory--lifecycle-hooks)
7. [Querying State & Decoupled Architecture](#7-querying-state--decoupled-architecture)
8. [Session Configuration & Capacity Tuning](#8-session-configuration--capacity-tuning)
9. [Error Handling & Diagnostics](#9-error-handling--diagnostics)
10. [Step-by-Step Execution Walkthrough](#10-step-by-step-execution-walkthrough)

---

## 1. Architectural Overview & Mental Model

### Declarative Composition in Pure C99
In conventional imperative C code, UI widgets, scene graphs, or entity lifecycles are created and destroyed through manual object allocations and explicit callback wiring. This creates synchronization bugs, dangling pointers, memory leaks, and cache-thrashing pointer graphs.

CELS replaces this with a **declarative composition pipeline**:
- You describe **what** components exist as a function of state.
- Components are represented as hierarchical nodes called **groups**.
- Group hierarchy, component-local state, and native resources are stored contiguously in an L1 cache-aligned **slot table**.
- When state mutates, CELS executes a **recomposition pass** that re-evaluates only the affected branches of the tree, skipping unchanged subtrees in $O(1)$ time.

```
       +-------------------------------------------------------------+
       |                     State Mutation                          |
       |  cel_mutate(&session, state) { this->width = 1024; }       |
       +-------------------------------------------------------------+
                                      |
                                      v
       +-------------------------------------------------------------+
       |               CelsStateRegistry Diff & Invalidate           |
       |  - Compares old snapshot vs new value via memcmp            |
       |  - Pushes watcher group keys to invalidationQueue           |
       +-------------------------------------------------------------+
                                      |
                                      v
       +-------------------------------------------------------------+
       |             CelsSessionRecompose() Walk                     |
       |  1. Drain Invalidation Queue                                |
       |     - Target group marked: CELS_FLAG_INVALIDATED            |
       |     - Ancestors marked:    CELS_FLAG_CONTAINS_INVALIDATED   |
       |  2. Traversal Walk                                          |
       |     - Unmarked subtrees: O(1) instant group skip            |
       |     - Contains-Invalidated: Descend into children           |
       |     - Invalidated: Re-execute composable body               |
       |  3. Structural Reconciliation                               |
       |     - Prune unvisited branches & fire OnDestroyed hooks     |
       +-------------------------------------------------------------+
```

### Core Invariants
1. **Contiguous Storage**: All structural metadata, slot offsets, and remembered state reside inside a single preallocated, cache-aligned slab.
2. **Pinned Slot Addresses**: Memory allocated via `cel_remember` or `cel_lifecycle_state` retains a stable pointer across recomposition passes as long as its owning composable remains mounted.
3. **Single-Threaded Session Core**: Each `CelsSession` operates on a single execution thread with zero internal locking overhead. Thread-safe communication is achieved by marshaling mutations to the session thread.

---

## 2. Memory Architecture: L1 Cache Slab

To eliminate CPU cache misses during tree traversal, CELS allocates or adopts a single 64-byte aligned memory slab tailored to the CPU's **L1 data cache (L1d)**.

```
+-------------------------------------------------------------------------------------------------+
|                                 CONTIGUOUS 64-BYTE ALIGNED SLAB                                 |
+------------------------------------+-----------------------------+------------------------------+
|       GROUPS GAP BUFFER            |    SLOT ALLOCATION TABLE    |       NONMOVING ARENA        |
|  CelsSlotGroup[maxGroups]          |  CelsSlotAllocation[...]    |  uint8_t[dataArenaSize]      |
|  (32 bytes each, 2 per cache line) |  (16 bytes each)            |  (Remembered & state data)   |
+------------------------------------+-----------------------------+------------------------------+
^                                    ^                             ^
s->groups                            s->slots                      s->dataArena
```

### Slab Profiles (`CelsSlabProfile`)
CELS defines four standard sizing profiles matching typical CPU L1 data cache sizes:

| Profile | Size | Groups Capacity | Intended Hardware Target |
|---|---|---|---|
| `CELS_SLAB_16K` | 16 KiB | 128 groups | Embedded MCUs, low-power IoT cores |
| `CELS_SLAB_32K` | 32 KiB | 256 groups | Standard L1d (AMD Zen 1–3, Intel E-cores, ARM Cortex) |
| `CELS_SLAB_48K` | 48 KiB | 384 groups | Modern high-performance L1d (Intel P-cores, Zen 4/5) |
| `CELS_SLAB_64K` | 64 KiB | 512 groups | Extended L1d (Apple Silicon M-series, deep trees) |

### Zero-Allocation Mode
In embedded or safety-critical environments where heap allocation (`malloc`) is restricted, developers can declare a statically allocated slab using the `CEL_SLAB` macro and pass it into `CelsSessionConfig`:

```c
// Statically allocate a 64-byte cache-line aligned slab buffer
static CEL_SLAB(g_appSlab, CELS_SLAB_32K);

CelsSession session;
CelsSessionInit(&session, &(CelsSessionConfig){
    .slab = g_appSlab,
    .slabSize = sizeof(g_appSlab),
    .maxGroups = 256
});
```
When `config->slab` is provided, `session.ownsSlab` is `false`, and `CelsSessionDestroy` will never call free.

---

## 3. The Slot Table & Dual Gap Buffer

The slab is internally carved into three distinct contiguous sections at initialization:

### 1. The Group Struct (`CelsSlotGroup`)
Every mounted composable or composition node corresponds to one `CelsSlotGroup`. It is packed ordered from largest fields to smallest fields to completely eliminate compiler padding bytes:

```c
typedef struct CelsSlotGroup {
    uint64_t key;          // 8 bytes: Stable 64-bit callsite hash
    uint64_t userData;     // 8 bytes: Opaque unique group ID (nextGroupId++)
    uint32_t parentIndex;  // 4 bytes: Logical index of parent group (0 for root)
    uint32_t slotIndex;    // 4 bytes: Arena offset for group data (dataOffset)
    uint16_t slotCount;    // 2 bytes: Size of group data in bytes (dataSize)
    uint16_t groupSize;    // 2 bytes: Transitive count of child groups in subtree
    uint16_t nodeCount;    // 2 bytes: User-defined node tally for the subtree
    uint16_t flags;        // 2 bytes: Invalidation & mount flags (CelsGroupFlags)
} CelsSlotGroup;
```
**Total Size**: Exactly **32 bytes**. Exactly **two groups** fit into a standard 64-byte CPU cache line, maximizing cache locality during tree walks.

### 2. The Slot Allocation Record (`CelsSlotAllocation`)
```c
typedef struct CelsSlotAllocation {
    uint32_t groupId;      // Owning group identity (matches group->userData)
    uint32_t slotOffset;   // Offset relative to the group's remembered slots
    uint32_t arenaOffset;  // Absolute byte offset into s->dataArena
    uint32_t size;         // Aligned byte size of the allocated slot
} CelsSlotAllocation;
```

### 3. Gap Buffer Mechanics
The groups array uses a **gap buffer** to support fast insertions and deletions of subtrees without reallocating the array:

```
Logical Index:  [ 0 ][ 1 ][ 2 ][ 3 ][ 4 ][ 5 ][ 6 ][ 7 ]
Physical Array: [ 0 ][ 1 ][ 2 ][GAP][GAP][GAP][ 3 ][ 4 ]
                                ^             ^
                         groupsGapStart   groupsGapEnd
```

- **Logical to Physical Translation**:
  ```c
  static inline uint32_t CelsGroupLogicalToPhysical(const CelsSession *s, uint32_t logical) {
      return (logical < s->groupsGapStart)
          ? logical
          : logical + (s->groupsGapEnd - s->groupsGapStart);
  }
  ```
- **Moving the Gap (`MoveGroupGap`)**:
  When inserting a new group at `targetLogical`, `memmove` shifts groups between `groupsGapStart` and `targetLogical`, preserving logical continuity.
- **$O(1)$ Subtree Skipping (`groupSize`)**:
  Every group maintains `groupSize`, which is the total count of all transitive descendant groups beneath it. During recomposition, if a group does not need to re-run, CELS skips the entire subtree by simply incrementing `logicalCursor += (1 + groupSize)`.

---

## 4. Tree Traversal & Reconciliation Engine

The tree walk is driven by three functions:
- `CelsEnterComposition(session, rootKey)`
- `CelsEnterComposable(session, key)`
- `CelsExitGroup(session)`

### Stack Context During Traversal
A session maintains parallel depth stacks capped at `CELS_MAX_DEPTH` (default 32):
- `activeStack[depth]`: `1` if the current group's body should run; `0` if skipped.
- `groupIndexStack[depth]`: The logical group index of the active parent at this depth.
- `oldGroupSizeStack[depth]`: The prior `groupSize` before entering the current pass.
- `slotOffsetStack[depth]`: Group-relative slot cursor offset.

### Composable Entry Algorithm (`CelsEnterComposable`)
When a composable function is called:

```
                       CelsEnterComposable(s, key)
                                    |
            +-----------------------+-----------------------+
            |                                               |
       Key matches cursor?                             Key differs?
            |                                               |
            v                                               v
    Check Group Flags                               Search forward among
    (INVALIDATED / CONTAINS_INVALIDATED)            siblings (cursor to parentEnd)
            |                                               |
      +-----+-----+                                   +-----+-----+
      |           |                                   |           |
    Dirty?      Clean?                              Found?      Not Found?
      |           |                                   |           |
      v           v                                   v           v
   Return       O(1) Skip:                          Move sibling Fresh Mount:
    TRUE        logicalCursor += (1+groupSize)      subtree to   Insert group at gap,
  (Run body)    Return FALSE                        cursor via   set FRESH_MOUNT flag,
                (Skip body)                         memmove      Return TRUE
```

1. **Key Generation**: If `key == 0`, CELS synthesizes a stable key derived from the parent group key and callsite sequence:
   ```c
   key = CelsKeyIndex(parentKey, (cursor - parentIdx + 1) * 0x9e3779b97f4a7c15ULL);
   ```
2. **Reordering & Permutations**: If the expected key does not match `cursor`, CELS scans sibling groups up to `parentEnd`. If found elsewhere, the existing subtree is moved to `cursor` using `memmove`, and parent indices are adjusted. This cleanly preserves state when items in a list reorder!
3. **Subtree Skipping**: If the group is found and carries neither `CELS_FLAG_INVALIDATED` nor `CELS_FLAG_CONTAINS_INVALIDATED`, `activeStack[depth]` is set to `0`, `logicalCursor` skips ahead by `1 + groupSize`, and `CelsEnterComposable` returns `false`. The composable body is not executed!
4. **Fresh Mount**: If the key was never seen before, a new `CelsSlotGroup` is inserted at the gap, marked with `CELS_FLAG_FRESH_MOUNT`, and ancestor `groupSize` values are incremented.

### Group Exit & Automatic Pruning (`CelsExitGroup`)
When `CelsExitGroup` runs:
1. It compares `logicalCursor` against `expectedEnd` (`groupIdx + 1 + groupSize`).
2. Any groups remaining in that range were **not visited** during this pass (for example, inside an `if` branch that evaluated to `false`).
3. These dead subtrees are immediately pruned via `CelsPruneSubtree`:
   - Active `onDestroy` callbacks are invoked for all resources in the subtree.
   - Watcher subscriptions in `CelsStateRegistry` are unsubscribed.
   - Arena slots are reclaimed.
   - The gap buffer is shifted to reclaim group metadata slots.

---

## 5. Reactive State & Invalidation Propagation

Reactivity in CELS is decoupled from the tree structure using the `CelsStateRegistry`.

```
                    +-------------------------------------+
                    |          CelsStateRegistry          |
                    +-------------------------------------+
                    | Cell 0: ptr -> [ Key A, Key B ]     |
                    | Cell 1: ptr -> [ Key C ]            |
                    +-------------------------------------+
```

### Dependency Tracking (`cel_watch` / `CelsStateRead`)
When a composable reads a state struct using `cel_watch(state_ptr)`:
1. CELS finds or creates a `CelsStateCell` matching `state_ptr`.
2. The current active composable's `group->key` is registered into the cell's `watcherKeys` array (up to `CELS_MAX_WATCHERS = 8`).
3. The dereferenced value `*state_ptr` is returned.

### Mutation Tracking (`cel_mutate` / `CelsStateCommitMutation`)
When `cel_mutate` executes:
```c
cel_mutate(&session, win) {
    this->width = 1024;
}
```
The macro expands to:
```c
for (__typeof__(*(win)) _cel_old_ = *(win), *this = (win);
     this != NULL;
     CelsStateCommitMutation((session), this, &_cel_old_, sizeof(*this)), this = NULL)
```
1. Before entering the block, it takes a value snapshot: `_cel_old_ = *win`.
2. The developer modifies fields via the `this->` pointer.
3. Upon loop exit, `CelsStateCommitMutation` compares `win` against `_cel_old_` using `memcmp`.
4. If identical, it does nothing.
5. If different, all keys subscribed to `win` are pushed into `session->invalidationQueue`.

### Upward Invalidation Propagation
During `CelsSessionRecompose`, `DrainInvalidationQueue` processes each queued key:
1. It finds the group matching the key and flags it with `CELS_FLAG_INVALIDATED`.
2. All descendant groups within `groupSize` also receive `CELS_FLAG_INVALIDATED` (cascading rebuild).
3. It climbs the `parentIndex` chain to the root, flagging every ancestor group with `CELS_FLAG_CONTAINS_INVALIDATED`.

```
                [ Root Group ]                 <-- flags |= CONTAINS_INVALIDATED
                      |
              [ Container Group ]              <-- flags |= CONTAINS_INVALIDATED
               /               \
       [ Unrelated Tree ]     [ Mutated Group ] <-- flags |= INVALIDATED
       (O(1) SKIPPED)                 |
                             [ Child Composable ]
```

### Convergence & Drain Bounds
If a composable's body mutates state during recomposition, additional keys are queued. `CelsSessionRecompose` loops until `invalidationQueue` is empty or until `maxDrainIterations` (default 8) is exceeded, preventing infinite recomposition cycles. If it fails to converge, it returns `CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE`.

---

## 6. Persistent Memory & Lifecycle Hooks

### Component-Local Memory (`cel_remember`)
To retain state across recompositions without global variables, composables use `cel_remember`:

```c
int *renderCount = cel_remember(int, 0);
(*renderCount)++;
```
- **On Fresh Mount**: `CelsResolveSlot` allocates `CELS_ALIGN_UP(sizeof(int))` bytes in `dataArena`, records a `CelsSlotAllocation` entry, copies the initial value `0`, and returns the pointer.
- **On Recomposition**: `CelsResolveSlot` looks up the existing allocation by `(groupId, slotOffset)` and returns the **exact same pinned memory pointer**.

### Managed Lifecycle State (`cel_lifecycle_state`)
When state owns system resources (file handles, textures, network sockets, thread pools):

```c
WindowState *win = cel_lifecycle_state(init, Window_OnCreated, Window_OnDestroyed);
```
- `onCreate(instance, session)` runs **once** when the component first mounts.
- `onDestroy(instance, session)` is stored in `s->cleanups`. When the component is pruned or the session destroyed, `onDestroy` is called to release native handles.

### Top-Level Composition Lifecycles (`CEL_LifeCycle` & `cel_destroy`)
To bind a root composition's lifetime to its state:

```c
CEL_LifeCycle(WindowLifeCycle, WindowState) {
    WindowState state = cel_watch(it);
    if (!state.isOpen) {
        cel_destroy(); // Signals the session to prune this composition tree
    }
}
```
When attached via `CEL_Attach(&session, MainWindow, WindowLifeCycle)`:
1. `WindowLifeCycle` is evaluated each recomposition pass.
2. If `cel_destroy()` is invoked, `CelsSessionRecompose` calls `CelsPruneSubtreeByKey`, pruning the composition and invoking all `OnDestroyed` callbacks!

---

## 7. Querying State & Decoupled Architecture

CELS eliminates global variables by allowing state to be retrieved directly from the session via its group key:

```c
WindowState *win = CEL_GetState(&session, CEL_KEY("MainWindow"), WindowState);
```

### Resolution Strategy of `CelsGetState`
`CelsGetState(session, key)` resolves the pointer through three tiers:
1. **Active Cleanups**: Searches active lifecycle state instances registered under `key`.
2. **Attached Compositions**: Searches attached composition root state pointers matching `key`.
3. **Group Data Arena**: Searches the group's primary slot in `dataArena`.

---

## 8. Session Configuration & Capacity Tuning

`CelsSessionInit` accepts a `CelsSessionConfig` struct:

```c
CelsSessionConfig config = {
    .root = NULL,                        // Optional root function
    .slabSize = CELS_SLAB_32K,           // Total slab bytes (default: 32 KiB)
    .slab = NULL,                        // NULL = allocate aligned; non-NULL = user buffer
    .maxGroups = 256,                    // Max groups (auto-calculated from slabSize if 0)
    .maxDrainIterations = 8              // Max recomposition cascading loops
};
CelsSessionInit(&session, &config);
```

### Capacity Sizing Formula
When carving the slab:
- Total groups: $N = \text{maxGroups}$
- Group bytes: $N \times 32\text{ bytes}$
- Slot allocation table: $N \times 16\text{ bytes}$
- Data arena: $\text{slabSize} - (N \times 48\text{ bytes})$

For a default **32 KiB slab** with 256 groups:
- Groups buffer: $256 \times 32 = 8{,}192\text{ bytes}$
- Slot records: $256 \times 16 = 4{,}096\text{ bytes}$
- Data arena: $32{,}768 - 12{,}288 = 20{,}480\text{ bytes}$ for persistent state!

### Engine Limits (`session.h` & `state.h`)

| Constant | Default | Description |
|---|---|---|
| `CELS_MAX_DEPTH` | 32 | Maximum nesting depth of composables |
| `CELS_MAX_CLEANUPS` | 128 | Maximum concurrent active lifecycle cleanup hooks |
| `CELS_MAX_STATES` | 256 | Maximum distinct reactive state pointers in registry |
| `CELS_MAX_WATCHERS` | 8 | Maximum subscriber composables per reactive state pointer |
| `CELS_MAX_QUEUE` | 256 | Maximum pending invalidations in queue |
| `CELS_MAX_ATTACHED_COMPOSITIONS` | 8 | Maximum top-level attached compositions |

---

## 9. Error Handling & Diagnostics

CELS functions return explicit `CelsResult` codes (or assert for contract violations):

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
Convert any code to a human-readable string using:
```c
const char *msg = CelsResultToString(result);
```

### Tree Inspection & Visualization
You can inspect the live slot table hierarchy using tree dump utilities (as demonstrated in `test/TreeTest.c`):

```c
void CelsDumpTree(const CelsSession *s);
```
Example visual output:
```
=== COMPOSABLE TREE DUMP (4 active nodes, 128 arena bytes) ===
[MainWindow] (0x8F0A2B1C) | descendants: 3 | slots: 32 B
├── [WindowContent] (0x129A440D) | descendants: 1 | slots: 16 B | parent: 0
│   └── [StatusBadge] (0x54BC21EE) | descendants: 0 | slots: 0 B | parent: 1
└── [Footer] (0x91F023AA) | descendants: 0 | slots: 0 B | parent: 0
```

---

## 10. Step-by-Step Execution Walkthrough

To see how all these pieces integrate in practice, let's trace the execution of the sample in `main.c`:

### Step 1: Session Initialization
1. `CelsSessionInit(&session, NULL)` allocates a 32 KiB 64-byte aligned slab.
2. Partitions `groups` (256 groups), `slots` (256 slots), and `dataArena` (20 KiB).
3. Initializes the groups gap buffer (`gapStart = 0, gapEnd = 256`).

### Step 2: Attaching Root Composition
1. `CEL_Attach(&session, MainWindow, WindowLifeCycle)` registers the composition function and evaluator.

### Step 3: Pass 1 — Initial Mount
1. `CelsSessionRecompose(&session)` begins.
2. `MainWindow` runs:
   - `cel_lifecycle_state` allocates `WindowState` in `dataArena` and calls `Window_OnCreated`.
   - Native handle `0x12345678` is stored.
3. `WindowContent(win)` runs:
   - `cel_remember(int, 0)` allocates render count slot (`*renderCount = 1`).
   - `cel_watch(win)` registers `WindowContent`'s key as a subscriber to `win`.
   - `StatusBadge()` mounts as a child group.
4. Tree sizes and parent indices are finalized in the slot table.

### Step 4: Pass 2 — Quiet Check
1. `CelsSessionRecompose(&session)` is called.
2. `hasComposedOnce == true` and `queueCount == 0`.
3. Returns `CELS_OK` **instantly** without traversing or executing any components.

### Step 5: Pass 3 — Mutation
1. `cel_mutate(&session, win) { this->width = 1024; }` modifies width.
2. `CelsStateCommitMutation` detects diff between snapshot and `win`.
3. Pushes `WindowContent`'s key into `invalidationQueue`.
4. `CelsSessionRecompose(&session)` runs:
   - `DrainInvalidationQueue` flags `WindowContent` with `CELS_FLAG_INVALIDATED` and `MainWindow` with `CELS_FLAG_CONTAINS_INVALIDATED`.
   - Traversal enters `MainWindow` (contains invalidated).
   - Traversal enters `WindowContent` (invalidated) -> re-runs body!
   - Render count increments to `2`.
   - Traversal encounters `StatusBadge` (not invalidated) -> **$O(1)$ skips**!

### Step 6: Pass 4 — Lifecycle Despawn
1. `cel_mutate(&session, win) { this->isOpen = false; }` runs.
2. `CelsSessionRecompose(&session)` executes `WindowLifeCycle`.
3. `WindowLifeCycle` observes `!state.isOpen` and calls `cel_destroy()`.
4. `CelsPruneSubtreeByKey` runs:
   - Invokes `Window_OnDestroyed`, releasing native resources.
   - Cleans up watcher subscriptions.
   - Reclaims arena slots and removes groups from the slot table.
5. `CelsSessionDestroy(&session)` frees the aligned slab and resets the session.
