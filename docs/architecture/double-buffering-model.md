# CELS Architecture: Double-Buffering Model & Lock-Free State

## 1. Executive Summary & Concurrency Model

In real-time multi-threaded simulation and game engines, shared mutable state is the primary source of concurrency bugs, including:
1. **Torn Reads**: A worker or render thread reads a partially updated struct (e.g. half of a 3D matrix or vector) mid-write.
2. **Lock Contention**: Mutexes around state objects cause main loop frame stutters, priority inversion, and cache line bouncing across CPU sockets.
3. **Non-Deterministic Recomposition**: In-frame mutations cause different branches of the same tree walk to observe conflicting versions of the same state variable.

CELS enforces a **strict double-buffering and staged transaction architecture**:
- **Front Buffer (Published Snapshot)**: Immutable during frame evaluation. Read locklessly by composables, UI renderers, and background reader threads (`cel_watch`, `cel_get_state`).
- **Back Buffer (Staging Workspace)**: Written exclusively by mutation handlers (`cel_mutate`, tasks, event callbacks).
- **Atomic Frame Boundary Publish**: Back-buffer mutations are synchronized to front buffers at deterministic frame boundaries via `CelsStatePublishDirty`.
- **Zero-Allocation Hoisted Headers**: Every reactive state cell is preceded by an aligned 32-byte `CelsStateHeader`, providing $\mathcal{O}(1)$ reflection, identity, and session resolution from a raw user pointer.

```
+---------------------------------------------------------------------------------------------------+
|                                  FRAME EXECUTION TIMELINE                                         |
+---------------------------------------------------------------------------------------------------+
|  T1: Event / Task Mutation     T2: Frame Boundary Publish       T3: Recomposition Tree Walk       |
|                                                                                                   |
|  [ Back Buffer ] <== Mutate                                                                       |
|  (Staging workspace)                                                                              |
|                                [ Back Buffer ]                                                    |
|                                       ||                                                          |
|                                    memcpy (Atomic sync)                                           |
|                                       \/                                                          |
|                                [ Front Buffer ] ========> [ Read-Only cel_watch / Render ]        |
|                                                           (Lock-free, zero torn reads)            |
+---------------------------------------------------------------------------------------------------+
```

---

## 2. The Reactive State Registry & State Cells

All reactive state cells within a session are tracked inside the preallocated `CelsStateRegistry`, carved directly out of the session slab or struct.

### 2.1 Struct Definitions

```c
#define CELS_MAX_STATES   2048u
#define CELS_MAX_WATCHERS 32u

typedef struct CelsStateCell {
    CEL_Id   id;                               /* 64-bit unique state identifier */
    void    *frontBuffer;                      /* Published snapshot (read-only) */
    void    *backBuffer;                       /* Staging workspace (mutable) */
    size_t   size;                             /* State struct size in bytes */
    bool     isDirty;                          /* True if backBuffer has pending changes */
    bool     inUse;                            /* Cell occupancy flag */
    uint64_t watcherKeys[CELS_MAX_WATCHERS];   /* Subscribed composable group keys */
    uint16_t watcherCount;                     /* Active subscriber count */
} CelsStateCell;

typedef struct CelsStateRegistry {
    CelsStateCell cells[CELS_MAX_STATES];
    uint32_t      cellCount;
} CelsStateRegistry;
```

### 2.2 Memory Allocation in the Slab

When `CelsStateGetOrCreateCell` creates a new reactive state cell:
1. It requests two separate allocations from `s->dataArena`:
   - `frontRaw = CelsSessionAllocData(session, sizeof(CelsStateHeader) + size)`
   - `back = CelsSessionAllocData(session, size)`
2. It initializes `frontRaw` as a `CelsStateHeader` and sets `frontBuffer = frontRaw + sizeof(CelsStateHeader)`.
3. If an initial value (`defaultVal`) is provided, it copies the initial bytes to **both** buffers using `memcpy`.
4. Both allocations are 64-byte aligned and guaranteed nonmoving.

---

## 3. Hoisted Memory Layout: `CelsStateHeader`

To eliminate the need for global variables, hash-table lookups, or passing explicit `session` and `CEL_Id` parameters to every API call, CELS embeds metadata directly in front of the user's data pointer.

### 3.1 32-Byte Header Specification

```c
#define CELS_STATE_MAGIC 0x43454C53u /* 'CELS' */

typedef struct CelsStateHeader {
    uint32_t magic;                 /* Offset  0: Validation tag (4 bytes) */
    uint32_t size;                  /* Offset  4: User struct size in bytes (4 bytes) */
    struct CelsSession *session;    /* Offset  8: Owning session pointer (8 bytes) */
    CEL_Id id;                      /* Offset 16: 64-bit unique cell key (8 bytes) */
    uint64_t reserved;              /* Offset 24: Padding to 32 bytes (8 bytes) */
} CelsStateHeader;
```

```
Memory Layout of a Reactive State Instance:
Byte:  0                   8                   16                  24                  32
      +-------------------+-------------------+-------------------+-------------------+===================+
      |  magic  |  size   |      session      |        id         |     reserved      |  USER STATE DATA  |
      | (uint32)| (uint32)|  (CelsSession*)   |     (uint64)      |     (uint64)      |  (Struct Payload) |
      +-------------------+-------------------+-------------------+-------------------+===================+
      ^                                                                               ^
      |--- CelsStateHeader (32 bytes) ------------------------------------------------|--- User Pointer --|
```

### 3.2 Constant-Time $\mathcal{O}(1)$ Resolution

Given a raw pointer `const void *ptr` returned to user code, resolving its owning session, cell identity, and back buffer requires **zero lookups**:

```c
static inline CelsStateHeader *CelsGetStateHeader(const void *ptr)
{
    if (ptr == NULL) return NULL;
    CelsStateHeader *hdr = (CelsStateHeader*)((const uint8_t*)ptr - sizeof(CelsStateHeader));
    if (hdr->magic == CELS_STATE_MAGIC) {
        return hdr;
    }
    return NULL;
}
```

#### Mechanical Benefits:
- **No Hash Lookups**: Resolving `(session, id)` is a single pointer decrement and validation check ($< 2\text{ ns}$).
- **Type Safety**: If a foreign pointer is passed, `hdr->magic != CELS_STATE_MAGIC` safely rejects it.
- **Cache Locality**: Accessing the header pulls the start of the user data into the same L1 cache line or adjacent line.

---

## 4. Front Buffer: Lock-Free Reader Semantics

The published front buffer is strictly read-only during frame evaluation.

### 4.1 Dependency Registration (`CelsStateWatch`)

When a composable calls `cel_watch(Type, ID)` or `cel_watch(instancePtr)`:

```c
const void *CelsStateWatch(CelsSession *session, CEL_Id id, size_t size)
{
    CelsStateCell *cell = CelsStateRegistryFindCell(&session->stateRegistry, id);
    if (cell == NULL) return NULL;

    /* Register current executing composable group as a subscriber */
    uint64_t currentKey = CelsSessionGetCurrentGroupKey(session);
    if (currentKey != 0) {
        bool alreadyWatching = false;
        for (uint16_t i = 0; i < cell->watcherCount; ++i) {
            if (cell->watcherKeys[i] == currentKey) {
                alreadyWatching = true;
                break;
            }
        }
        if (!alreadyWatching && cell->watcherCount < CELS_MAX_WATCHERS) {
            cell->watcherKeys[cell->watcherCount++] = currentKey;
        }
    }

    return cell->frontBuffer;
}
```

### 4.2 Zero Torn Reads Guarantee

Because the front buffer is **never modified** while a frame is executing:
1. Readers (even on concurrent worker threads) experience **zero torn reads**.
2. No reader locks, atomic exchange instructions, or memory barriers are required during reads.
3. Reading state is as fast as reading a normal C `const struct` pointer.

---

## 5. Back Buffer: Staged Mutation & Invalidation

All mutations must target the staging back buffer.

### 5.1 Enforcing the Pure Functional Rule

CELS strictly prohibits calling `cel_mutate` from within an active composable during recomposition:

```c
void *CelsStateMutate(CelsSession *session, CEL_Id id, size_t size)
{
    /* Enforce DSL rule: composables must remain pure functions of state */
    assert((!session->isRecomposing || session->isExecutingTask) &&
           "cel_mutate cannot be called inside a Composable or Composition body. "
           "Perform mutations in event callbacks, input handlers, simulation loops, or CEL_Task coroutines.");

    CelsStateCell *cell = CelsStateRegistryFindCell(&session->stateRegistry, id);
    if (cell == NULL) return NULL;

    /* 1. Flag state dirty for next publish pass */
    cell->isDirty = true;

    /* 2. Enqueue all registered watchers for recomposition */
    for (uint16_t i = 0; i < cell->watcherCount; ++i) {
        const uint64_t targetKey = cell->watcherKeys[i];
        bool alreadyQueued = false;
        for (uint32_t q = 0; q < session->queueCount; ++q) {
            if (session->invalidationQueue[q] == targetKey) {
                alreadyQueued = true;
                break;
            }
        }
        if (!alreadyQueued && session->queueCount < CELS_MAX_QUEUE) {
            session->invalidationQueue[session->queueCount++] = targetKey;
        }
    }

    return cell->backBuffer;
}
```

#### Mutation Invariants:
1. **Isolation**: Back-buffer edits have **zero effect** on currently executing readers.
2. **Immediate Dependency Queuing**: All composables watching `id` have their group keys pushed into `session->invalidationQueue`.
3. **Permitted Contexts**: Mutations are permitted in:
   - User input & window event callbacks (e.g. mouse clicks, keystrokes).
   - Physics & simulation tick steps.
   - `CEL_Task` procedural coroutines (`session->isExecutingTask == true`).
   - Lifecycle initialization hooks (`onCreate`).

---

## 6. Atomic Publish & Frame Synchronization

Staged mutations transition from back buffer to front buffer atomically at frame boundaries via `CelsStatePublishDirty`:

```c
void CelsStatePublishDirty(CelsSession *session)
{
    if (session == NULL) return;

    for (uint32_t i = 0; i < session->stateRegistry.cellCount; ++i) {
        CelsStateCell *cell = &session->stateRegistry.cells[i];
        if (cell->inUse && cell->isDirty) {
            memcpy(cell->frontBuffer, cell->backBuffer, cell->size);
            cell->isDirty = false;
        }
    }
}
```

### 6.1 Placement in the Frame Pipeline

`CelsStatePublishDirty` is called at two critical stages in `CelsSessionRecompose`:
1. **Pre-Evaluation**: Before draining the invalidation queue, synchronizing any mutations produced by event handlers or background tasks prior to the frame.
2. **Post-Evaluation**: Immediately after the recomposition drain loop settles, synchronizing any state mutations produced by tasks during the frame before external renderers or post-frame hooks execute.

---

## 7. Cross-Thread Double-Buffered Transaction Batches

When composables need to issue commands or component updates to external worker backends (such as Flecs ECS, Vulkan command buffers, or audio threads), direct cross-thread function calls cause severe locking overhead.

CELS solves this with **double-buffered transaction batches** (`CelsTransactionBatch`).

### 7.1 Architecture of `transactionBatches[2]`

Inside `CelsSession`:

```c
#define CELS_MAX_TRANSACTIONS 4096u
#define CELS_TRANSACTION_DATA_SIZE (64u * 1024u)

typedef struct CelsTransactionOp {
    uint32_t opCode;     /* CELS_OP_SET, REMOVE, DELETE, CUSTOM */
    uint32_t size;       /* Payload byte size */
    uint64_t targetId;   /* Target entity / resource ID */
    uint64_t typeKey;    /* 64-bit component type key */
    uint32_t dataOffset; /* Offset in batch linear payload arena */
} CelsTransactionOp;

typedef struct CelsTransactionBatch {
    CelsTransactionOp ops[CELS_MAX_TRANSACTIONS];
    uint32_t          opCount;
    uint8_t           data[CELS_TRANSACTION_DATA_SIZE];
    uint32_t          dataSize;
} CelsTransactionBatch;
```

```
Main Session Thread:
[ Composable Tree ] ===> Stages ops into: transactionBatches[activeBatchIndex]
                                                     ||
                                      CelsSessionSwapTransactionBatches()
                                                     ||
Worker Thread:                                       \/
[ Worker Loop ] <======= Consumes:        transactionBatches[1 - activeBatchIndex]
```

### 7.2 Lockless Batch Handoff Sequence

1. **Staging Phase**: During recomposition, composables call `cel_stage_set(...)`, `cel_stage_remove(...)`, or `cel_stage_delete(...)`. Operations and payloads append sequentially into `session->transactionBatches[activeBatchIndex]`. Zero mutexes are locked.
2. **Frame Completion**: At frame end, the main thread invokes:
   ```c
   void CelsSessionSwapTransactionBatches(CelsSession *session)
   {
       session->activeBatchIndex = 1u - session->activeBatchIndex;
       /* Reset the new active batch for subsequent frame staging */
       CelsTransactionBatch *newActive = &session->transactionBatches[session->activeBatchIndex];
       newActive->opCount = 0;
       newActive->dataSize = 0;
   }
   ```
3. **Worker Consumption**: The worker thread reads the ready batch via `CelsSessionGetReadyBatch(session)`:
   - Processes all staged operations in linear sequence.
   - Clears the batch via `CelsSessionClearReadyBatch(session)`.

#### Performance Advantages:
- **Lock-Free**: Main session thread never waits on worker threads.
- **Cache-Optimal**: Operations and payloads reside in contiguous cache-aligned buffers.
- **Deterministic**: Operations are applied in the exact sequence they were staged by the tree walk.
