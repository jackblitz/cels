---
name: cels-transactions
description: Provide technical guidance on CELS transaction batch staging, lock-free double-buffering, and external backend bridging. Use when staging mutations during recomposition, integrating external ECS (Flecs) or render backends (Vulkan), passing cross-thread command buffers, or configuring post-recomposition frame commit hooks.
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.3.0"
  last-updated: '2026-09-30'
  category: transactions-and-staging
  keywords:
    - transactions
    - staging
    - commit-hooks
    - command-buffers
    - lock-free
    - double-buffering
    - ECS
    - Flecs
    - Vulkan
    - cross-thread
    - C99
---

# CELS Transactions & Cross-Thread Staging Guide

CELS provides a lock-free, zero-allocation staging pipeline for external mutations (ECS systems, GPU command buffers, audio queues, physics impulses). This allows composables and tasks to stage changes during recomposition without lock contention or cache churn.

---

## 1. Core Mental Model: Staging vs. Committing

- **Recomposition Walk**: Single-threaded and fast. Composables declare what components or commands should exist using `cel_stage_set`, `cel_stage_remove`, and `cel_stage_delete`. Operations are appended into the active `CelsTransactionBatch`.
- **Pipeline Boundary**: At the end of recomposition (or during dedicated engine phases), the batch is committed atomically to the external backend via `CelsSessionCommitTransactions` or swapped locklessly to worker threads via `CelsSessionSwapTransactionBatches`.

```
[ Composable Tree ]
       │
       ├── cel_stage_set(it, Position, { .x = 10, .y = 20 })
       ├── cel_stage_remove(it, RigidBody)
       └── cel_stage_delete(it)
       │
       ▼
[ CelsTransactionBatch (Linear Buffer) ]
       │
       ▼ (Recomposition converges)
       │
[ Commit Handler (Atomic Sync Point) ]
```

---

## 2. Core Staging Macros

Include `<cels/transaction.h>` (or `<cels/cels.h>`).

### `cel_stage_set(targetId, Type, { ... })`
Stages a component addition or value update on `targetId`:
```c
cel_stage_set(it, Position, { .x = 100.0f, .y = 0.0f, .z = 50.0f });
cel_stage_set(it, Health,   { .hp = 100, .maxHp = 100 });
```

### `cel_stage_remove(targetId, Type)`
Stages the removal of a component/tag from `targetId`:
```c
cel_stage_remove(it, RigidBody);
```

### `cel_stage_delete(targetId)`
Stages the destruction/deletion of a target entity or native resource:
```c
cel_stage_delete(it);
```

### `cel_stage_custom(opCode, targetId, Type, { ... })`
Stages an application-defined opcode transaction with typed payload:
```c
#define CMD_PLAY_AUDIO (CELS_OP_CUSTOM + 1)
cel_stage_custom(CMD_PLAY_AUDIO, channelId, AudioCue, { .clipId = 42, .gain = 1.0f });
```

---

## 3. Session User Data & Ambient Context

Store the engine context (e.g. `ecs_world_t*`, `Renderer*`) directly on the session:

```c
/* Host Setup */
CelsSessionSetUserData(session, engineWorld);

/* Inside any Composable or Task */
EngineWorld *world = cel_user_data(EngineWorld);
```

---

## 4. Post-Recomposition Commit Hook

To automatically commit transactions whenever `CelsSessionRecompose` finishes its frame without host loop boilerplate:

```c
static void BackendCommitHandler(CelsOpCode opCode,
                                 uint64_t targetId,
                                 uint64_t typeKey,
                                 const void *data,
                                 size_t size,
                                 void *userData)
{
    BackendWorld *world = (BackendWorld *)userData;
    /* Dispatch opCode: SET, REMOVE, DELETE, or CUSTOM */
}

static void OnFrameComplete(CelsSession *session, void *userData)
{
    CelsSessionCommitTransactions(session, BackendCommitHandler, userData);
}

/* Register once on session */
CelsSessionSetPostRecomposeHook(session, OnFrameComplete, backendWorld);
```

---

## 5. Lockless Cross-Thread Handoff (Double-Buffering)

To pass staged commands across threads (e.g. main thread UI -> render/physics worker thread) without locks:

1. **Main Thread**: Call `CelsSessionSwapTransactionBatches(session)` at frame end.
2. **Worker Thread**: Read and process `CelsSessionGetReadyBatch(session)`.
3. **Worker Thread**: Call `CelsSessionClearReadyBatch(session)` once drained.

---

## 6. Slot Memory Unmount Cleanups

To trigger cleanup when a composable leaves the tree, use `cel_remember` with an optional 3rd argument for the destructor callback:

```c
static void OnResourceUnmount(void *ptr, CelsSession *session CELS_UNUSED) {
    uint64_t *resId = (uint64_t *)ptr;
    cel_stage_delete(*resId);
}

/* With unmount cleanup: */
uint64_t *res = cel_remember(uint64_t, initialId, OnResourceUnmount);

/* Without unmount cleanup (omit or pass NULL): */
int *counter = cel_remember(int, 0);
int *counter_explicit = cel_remember(int, 0, NULL);
```

---

## 7. Staging Transactions from `CEL_Lifecycle`

Transactions can also be staged directly inside `CEL_Lifecycle` blocks:
- **`mount`**: Stage initial component sets or resource additions (`cel_stage_set`).
- **`unmount`**: Stage resource deletions (`cel_stage_delete`) or component removals (`cel_stage_remove`).

```c
CEL_Lifecycle(EntityTransactionLifecycle, uint64_t, entityId) {
    mount {
        /* Add/set operations staged on initial mount */
        cel_stage_set(entityId, Position, { .x = 100.0f, .y = 200.0f, .z = 0.0f });
        cel_stage_set(entityId, Health,   { .hp = 100, .maxHp = 100 });
    }
    unmount {
        /* Delete operation staged when omitted from the tree */
        cel_stage_delete(entityId);
    }
}

CEL_Composable(EnemyNode, uint64_t, entityId) {
    cel_lifecycle(EntityTransactionLifecycle, entityId);
}
```

---

## 8. Core Patterns & Anti-Patterns (Best for LLMs)

### Pattern 1: Staging vs. Direct Mutation of External Systems

```c
// WRONG: Directly modifying external engine systems during recomposition walk
CEL_Composable(PlayerEntity, uint64_t, entityId) {
    ecs_world_t *world = GetExternalEcsWorld();
    ecs_set(world, entityId, Position, { 10.0f, 20.0f, 0.0f }); // Bad! Lock contention, cache churn, non-atomic!
}

// CORRECT: Stage mutations to the linear transaction buffer
CEL_Composable(PlayerEntity, uint64_t, entityId) {
    cel_stage_set(entityId, Position, { .x = 10.0f, .y = 20.0f, .z = 0.0f }); // Lock-free, zero-allocation
}
```

### Pattern 2: Atomic Dispatch at Pipeline Boundary

```c
// WRONG: Manually committing transactions inside individual composables
CEL_Composable(PlayerEntity, uint64_t, entityId) {
    cel_stage_set(entityId, Position, { 10.0f, 20.0f, 0.0f });
    CelsSessionCommitTransactions(session, Dispatcher, NULL); // Bad! Breaks batching & partial state corruption!
}

// CORRECT: Commit atomically after recomposition via post-recompose hook
static void OnFrameComplete(CelsSession *session, void *userData) {
    // Drains the entire frame's staged operations in a single atomic batch
    CelsSessionCommitTransactions(session, BackendCommitHandler, userData);
}

CelsSessionSetPostRecomposeHook(session, OnFrameComplete, backendWorld);
```

### Pattern 3: Lock-Free Worker Thread Handoff

```c
// WRONG: Sharing the active transaction batch across threads while recomposition writes to it
void RenderThread(CelsSession *session) {
    // Race condition! session->activeBatch is concurrently being written by the UI thread!
    ProcessBatch(session->activeBatch);
}

// CORRECT: Double-buffer swap at frame boundary
// Main Thread:
CelsSessionSwapTransactionBatches(session);

// Worker / Render Thread:
CelsTransactionBatch *batch = CelsSessionGetReadyBatch(session);
if (batch) {
    ProcessBatch(batch);
    CelsSessionClearReadyBatch(session);
}
```
