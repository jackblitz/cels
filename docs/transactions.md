# CELS Transactions & Cross-Thread Staging Buffer

## 1. Overview & Mental Model

In high-performance interactive applications (game engines, simulations, graphical editors), logic is divided into two distinct responsibilities:
- **Declarative Tree & State (CELS)**: Recomposes hierarchies, evaluates reactive conditions, manages lifecycles, and executes coroutine tasks.
- **Data-Oriented Backends (ECS, Vulkan, Audio, Physics)**: Run across thread pools, executing contiguous memory iterations, draw calls, or audio mixes.

Directly executing external mutations (such as ECS component additions, GPU buffer writes, or thread-pool commands) in the middle of a composable tree walk causes cache thrashing, partial updates, and severe lock contention.

**CELS Transactions** provide a lock-free, zero-allocation staging pipeline:
1. During recomposition, composables and tasks stage mutations into the active session's transaction buffer via `cel_stage_set`, `cel_stage_remove`, `cel_stage_delete`, and `cel_stage_custom`.
2. Recomposition completes and converges with zero lock contention.
3. At frame completion (via `postRecomposeHook` or external phase synchronization), the batch is either:
   - **Committed directly** to the backend in a single atomic pass, or
   - **Swapped locklessly** to worker threads (`CelsSessionSwapTransactionBatches`) so external systems can process the batch in parallel.

```
[ Composable Tree Walk ]
         │
         ├── cel_stage_set(targetId, Position, { ... })
         ├── cel_stage_remove(targetId, RigidBody)
         └── cel_stage_delete(targetId)
         │
         ▼
[ Active CelsTransactionBatch (Linear Buffer) ]
         │
         ▼ (CelsSessionRecompose finishes)
         │
 ┌───────┴───────────────────────────────┐
 │                                       │
 ▼ (Mode A: Immediate Commit)            ▼ (Mode B: Cross-Thread Handoff)
CelsSessionCommitTransactions(...)      CelsSessionSwapTransactionBatches(...)
  └── Executes atomic backend batch       └── Ready batch consumed by worker thread
```

---

## 2. Core Staging API

Include `<cels/transaction.h>` (or `<cels/cels.h>`).

### 2.1 Staging Component / Data Mutations: `cel_stage_set`
Appends a `CELS_OP_SET` operation with a typed payload to the active session batch:

```c
cel_stage_set(entityId, Position, { .x = 10.0f, .y = 20.0f, .z = 0.0f });
cel_stage_set(entityId, Health,   { .hp = 100, .maxHp = 100 });
```

### 2.2 Staging Component Removal: `cel_stage_remove`
Appends a `CELS_OP_REMOVE` operation for a specific component type key:

```c
cel_stage_remove(entityId, RigidBody);
```

### 2.3 Staging Target Deletion: `cel_stage_delete`
Appends a `CELS_OP_DELETE` operation to destroy the target entity or native resource:

```c
cel_stage_delete(entityId);
```

### 2.4 Staging Custom Opcodes: `cel_stage_custom`
Developers can define application-specific opcodes starting from `CELS_OP_CUSTOM`:

```c
#define CMD_PLAY_SFX   (CELS_OP_CUSTOM + 1)
#define CMD_SPAWN_VFX  (CELS_OP_CUSTOM + 2)

cel_stage_custom(CMD_PLAY_SFX, soundChannelId, SoundEffect, { .soundId = SFX_EXPLODE, .volume = 0.8f });
```

---

## 3. Session User Data & Post-Recomposition Hook

### 3.1 Session User Data (`session->userData`)
Attach backend contexts (e.g. `ecs_world_t*`, `RenderContext*`) directly to the session:

```c
/* Host Initialization */
CelsSessionSetUserData(session, ecsWorld);

/* Inside any Composable or Task */
ecs_world_t *world = cel_user_data(ecs_world_t);
```

### 3.2 Automatic Frame Commit (`postRecomposeHook`)
Register a post-recompose hook to automatically flush staged transactions whenever `CelsSessionRecompose` finishes:

```c
static void OnFrameComplete(CelsSession *session, void *userData) {
    ecs_world_t *world = (ecs_world_t *)userData;
    
    ecs_defer_begin(world);
    CelsSessionCommitTransactions(session, BackendCommitHandler, world);
    ecs_defer_end(world);
}

/* Register hook */
CelsSessionSetPostRecomposeHook(session, OnFrameComplete, ecsWorld);
```

Whenever `CelsSessionRecompose(session)` runs and converges, it automatically invokes this hook, committing all staged mutations with zero host loop boilerplate.

---

## 4. Double-Buffered Cross-Thread Handoff (Zero Locks)

When mutations must be executed on a dedicated render or physics worker thread:

1. **Recomposition Thread**: Stages transactions into `activeBatch`.
2. **At Frame End**: Call `CelsSessionSwapTransactionBatches(session)`.
   - The active batch becomes the `readyBatch`.
   - The previous ready batch is reset and becomes the new active batch.
3. **Worker Thread**: Consumes the ready batch locklessly:
   ```c
   const CelsTransactionBatch *batch = CelsSessionGetReadyBatch(session);
   for (uint32_t i = 0; i < batch->opCount; ++i) {
       const CelsTransactionOp *op = &batch->ops[i];
       const void *payload = (op->size > 0) ? &batch->data[op->dataOffset] : NULL;
       ProcessWorkerOp(op->opCode, op->targetId, op->typeKey, payload);
   }
   CelsSessionClearReadyBatch(session);
   ```

---

## 5. Slot Memory with Unmount Cleanups

To tie native resource or entity lifecycles to slot memory:

### Option A: `cel_on_unmount`
```c
ecs_entity_t *it = cel_remember(ecs_entity_t, 0);
if (*it == 0) {
    *it = ecs_new_id(world);
}
cel_on_unmount(OnEntityUnmount, it);
```

### Option B: `cel_remember_cleanup`
```c
static void OnEntityUnmount(void *ptr, CelsSession *session) {
    (void)session;
    ecs_entity_t *e = (ecs_entity_t *)ptr;
    cel_stage_delete(*e);
}

/* Inside composable: */
ecs_entity_t *it = cel_remember_cleanup(ecs_entity_t, 0, OnEntityUnmount);
if (*it == 0) {
    *it = ecs_new_id(world);
}
```

When the composable leaves the tree (e.g. `if (enemy->isAlive)` becomes `false`), CELS slot reconciliation calls `OnEntityUnmount`, automatically staging `cel_stage_delete` into the transaction batch.

---

## 6. Staging Transactions from `CEL_Lifecycle` (`mount` & `unmount`)

Transactions can also be staged directly within `CEL_Lifecycle` blocks:
- **`mount`**: Stages initial component sets/additions (`cel_stage_set`) when the composable first attaches to the tree.
- **`unmount`**: Stages destruction (`cel_stage_delete`) or component removal (`cel_stage_remove`) when the composable is omitted and pruned by slot-table reconciliation.

```c
CEL_Lifecycle(EntityTransactionLifecycle, uint64_t, entityId) {
    mount {
        /* Stages add/set operations on initial mount */
        cel_stage_set(entityId, Position, { .x = 100.0f, .y = 200.0f, .z = 0.0f });
        cel_stage_set(entityId, Health,   { .hp = 100, .maxHp = 100 });
    }
    unmount {
        /* Stages delete operation when omitted from tree */
        cel_stage_delete(entityId);
    }
}

CEL_Composable(EnemyNode, uint64_t, entityId) {
    cel_lifecycle(EntityTransactionLifecycle, entityId);
}
```

This guarantees that entity creation and destruction operations are staged into the transaction batch in perfect lockstep with the declarative composable tree hierarchy.
