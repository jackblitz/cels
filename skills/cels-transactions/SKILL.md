---
name: cels-transactions
description: Provide technical guidance on CELS declarative reconciliation, transaction batch staging, lock-free double-buffering, and external backend bridging. Use when binding composables to external entities, attaching declarative components (cel_has, cel_has_tag), performing 2-tier intra-frame queries (cel_get, cel_is), managing entity identity (cel_id, cel_key), or staging transactions to external backends like renderers, physics, audio, or simulation systems (cel_stage_*, commit hooks).
license: Apache-2.0
compatibility: ANSI C99, CMake 3.20+, GCC/Clang/MSVC
metadata:
  author: CELS Authors
  version: "0.5.0"
  last-updated: '2026-10-07'
  category: transactions-and-staging
  keywords:
    - transactions
    - staging
    - reconciliation
    - cel_has
    - cel_has_tag
    - cel_get
    - cel_is
    - cel_id
    - cel_key
    - cel_stage_set
    - commit-hooks
    - double-buffering
    - backends
    - C99
---

# CELS Declarative Reconciliation & Transaction Staging Guide

CELS acts as a declarative orchestrator. It bridges declarative UI, scene hierarchies, and simulation nodes to external engines—such as GPU renderers, physics simulations, audio graphs, or Entity Component Systems—via an atomic, lock-free transaction staging pipeline.

---

## 1. Core Mental Model: Staging vs. Committing

```
[ Composable Tree ]
       │
       ├── cel_has(Transform, { .x = 10, .y = 20 })   ──> stages CELS_OP_SET
       ├── (omitted conditional block)               ──> stages CELS_OP_REMOVE (auto-diff)
       └── (node unmounted from tree)                ──> stages CELS_OP_DELETE
       │
       ▼
[ CelsTransactionBatch (Contiguous Linear Buffer) ]
       │
       ▼ (Recomposition completes)
       │
[ Commit Point / Frame Hook ]
       ├── Direct Commit: CelsSessionCommitTransactions -> External Backend Handlers
       │   (Renderer, Physics Engine, Audio Queue, or ECS)
       │
       └── Cross-Thread Handoff: CelsSessionSwapTransactionBatches -> Worker Thread
```

### Architectural Separation:
1. **Recomposition Pass**: Pure, single-threaded, cache-friendly CELS execution. Composables declaratively express what properties, components, or commands *should* exist on target entities.
2. **Transaction Batch**: Operations are appended to a contiguous `CelsTransactionBatch` without calling external backend APIs mid-walk (preventing lock contention, cache churn, and ordering bugs).
3. **Commit Phase**: Transactions are committed atomically to external backends at the frame boundary, or swapped locklessly to dedicated worker threads.

---

## 2. Composable Identity (`cel_id`, `cel_key`, `cel_call`)

Every composable node in CELS is an identified entity with a 64-bit ID representing an external object, render node, or simulation entity:

### 1. Auto-Generated Identity (`cel_id`)
When unkeyed (`CEL_Composable(MyWidget)`), CELS assigns a unique stable 64-bit ID based on slot address and callsite hash. Read it inside the composable via `cel_id()`:
```c
CEL_Composable(DefaultWidget) {
    uint64_t targetId = cel_id();
    cel_has(Transform, { .x = 0, .y = 0 });
}
```

### 2. Explicit Keyed Composable Signature
Keyed composables declare `id` directly as the first parameter:
```c
CEL_Composable(RenderObjectNode, id, const ObjectState*, obj) {
    cel_watch(obj);
    // cel_id() evaluates to 'id' passed by caller
    cel_has(Transform,  { obj->x, obj->y, obj->z });
    cel_has(RenderMesh, { obj->meshId, obj->materialId });
}

// Callers invoke with explicit ID:
RenderObjectNode(entityId, objectState);
```

### 3. Dynamic List Reconciliation (`cel_key` & `cel_call`)
In dynamic loops where item order changes between frames, preserve slot memory and component bindings with `cel_key` or `cel_call`:
```c
CEL_Composable(EntityHierarchy, const EntityItem *items, int count) {
    for (int i = 0; i < count; i++) {
        /* Block form */
        cel_key(items[i].id) {
            cel_has(Transform, { items[i].x, items[i].y, 0.0f });
            EntitySprite(&items[i]);
        }

        /* Or single-line form */
        cel_call(EntitySprite, items[i].id, &items[i]);
    }
}
```

---

## 3. Declarative Component Reconciliation (`cel_has` & `cel_has_tag`)

Instead of writing imperative glue code with manual update and removal checks, declare components directly inside composables:

### Attaching Components: `cel_has`
```c
cel_has(Transform, { .x = obj->x, .y = obj->y, .z = 0.0f });
cel_has(Health,    { .current = obj->hp, .max = obj->maxHp });
```

### Attaching Zero-Sized Tags: `cel_has_tag`
```c
if (obj->isSelected) {
    cel_has_tag(SelectedTag);
}
```

### The Declarative Reconciliation Invariants
1. **Mount / First Declaration**: Stages a `CELS_OP_SET` transaction.
2. **Automatic Omission Removal**: If a component was declared in frame $N$, but is **omitted** in frame $N+1$ (e.g. an `if` condition evaluates to `false`), CELS **automatically stages a `CELS_OP_REMOVE` transaction** on scope exit!
3. **Automatic Unmount Cleanup**: If the composable or keyed entity leaves the slot table, CELS **automatically stages a `CELS_OP_DELETE` transaction**.

```c
CEL_Composable(SceneEntity, id, const EntityState*, entity) {
    cel_watch(entity);

    cel_has(Transform, { entity->x, entity->y, 0.0f });

    /* When isHighlighted transitions true -> false, CELS AUTO-REMOVES HighlightEffect! */
    if (entity->isHighlighted) {
        cel_has(HighlightEffect, { .color = 0xFFFFCC00, .intensity = 1.5f });
    }

    if (entity->hasPhysics) {
        cel_has(RigidBody, { .mass = 10.0f, .isStatic = false });
    }
}
```

---

## 4. Intra-Frame Queries & External Fallback (`cel_get` & `cel_is`)

During recomposition, a composable or child may need to inspect attached components before transactions are committed to the external backend.

CELS provides **2-Tier Resolution**:
- **Tier 1 (Intra-Frame)**: Checks the active frame's staged transaction batch. If a component was staged in this frame, returns the staged payload immediately.
- **Tier 2 (External Backend Fallback)**: If not present in the staged batch, queries the external backend via `CelsSessionSetEcsLookupHook`.

```c
// Intra-frame query on current node (implicit cel_id()):
const Health *hp = cel_get(Health);
if (hp && hp->current < 25) {
    cel_has_tag(LowHealthAlert);
}

// Intra-frame query on explicit target ID:
const Transform *targetTransform = cel_get(targetEntityId, Transform);

// Boolean tag check:
if (cel_is(SelectedTag)) {
    cel_has(SelectionBox, { .lineWidth = 2.0f });
}
```

---

## 5. Low-Level Transaction Staging Pipeline (`cel_stage_*`)

When dispatching custom commands, audio cues, or GPU drawing primitives without composables:

```c
#include "cels/runtime/transaction.h"

/* Stage component set */
cel_stage_set(targetId, Transform, { .x = 100.0f, .y = 50.0f });

/* Stage component remove */
cel_stage_remove(targetId, RigidBody);

/* Stage entity destruction */
cel_stage_delete(targetId);

/* Stage custom application opcode */
#define CMD_PLAY_AUDIO (CELS_OP_CUSTOM + 1)
cel_stage_custom(CMD_PLAY_AUDIO, channelId, AudioCue, { .clipId = 42, .gain = 1.0f });
```

---

## 6. Post-Recomposition Commit Hook

To automatically commit transactions to your target backend whenever `CelsSessionRecompose` finishes:

```c
static void GenericBackendCommitHandler(CelsOpCode opCode,
                                        uint64_t targetId,
                                        uint64_t typeKey,
                                        const void *data,
                                        size_t size,
                                        void *userData)
{
    AppBackend *backend = (AppBackend *)userData;

    switch (opCode) {
        case CELS_OP_SET:
            backend->setComponent(backend->world, targetId, typeKey, data, size);
            break;
        case CELS_OP_REMOVE:
            backend->removeComponent(backend->world, targetId, typeKey);
            break;
        case CELS_OP_DELETE:
            backend->destroyEntity(backend->world, targetId);
            break;
        default:
            if (opCode > CELS_OP_CUSTOM) {
                backend->dispatchCustomCommand(backend, opCode, targetId, data, size);
            }
            break;
    }
}

static void OnFrameComplete(CelsSession *session, void *userData)
{
    CelsSessionCommitTransactions(session, GenericBackendCommitHandler, userData);
}

/* Register once on session during host initialization */
CelsSessionSetPostRecomposeHook(session, OnFrameComplete, appBackend);
```

*(Note: If integrating with an ECS library such as Flecs, `GenericBackendCommitHandler` simply maps `CELS_OP_SET` to `ecs_set_id`, `CELS_OP_REMOVE` to `ecs_remove_id`, and `CELS_OP_DELETE` to `ecs_delete`.)*

---

## 7. Lockless Cross-Thread Double-Buffering Handoff

To hand off staged transactions across threads without lock contention (e.g. UI main thread -> render or physics worker thread):

1. **Main Thread**: Call `CelsSessionSwapTransactionBatches(session)` at frame end.
2. **Worker Thread**: Read and process `CelsSessionGetReadyBatch(session)`.
3. **Worker Thread**: Call `CelsSessionClearReadyBatch(session)` once drained.

```c
/* In Main Thread Loop (at end of frame): */
CelsSessionSwapTransactionBatches(&engine.session);

/* In Worker Thread Loop: */
const CelsTransactionBatch *batch = CelsSessionGetReadyBatch(&engine.session);
if (batch && batch->count > 0) {
    for (uint32_t i = 0; i < batch->count; ++i) {
        const CelsTransactionHeader *tx = &batch->headers[i];
        ApplyWorkerCommand(tx);
    }
    CelsSessionClearReadyBatch(&engine.session);
}
```

---

## 8. Anti-Patterns to Avoid

### ❌ Anti-Pattern 1: Calling External Backend APIs Directly During Recomposition
```c
// BAD: Mutating external backend directly inside composable
CEL_Composable(MyNode) {
    MyExternalRenderer_Draw(mesh); // Bad! Violates pipeline purity
}
```
**Fix**: Use `cel_has` or `cel_stage_*`. Let the staged transaction batch commit at the frame sync point.

### ❌ Anti-Pattern 2: Manual Imperative Removal Branches
```c
// BAD: Manually writing imperative removal branches
if (entity->isHighlighted) {
    backend->addHighlight(entity->id);
} else {
    backend->removeHighlight(entity->id); // Easily forgotten!
}
```
**Fix**: Use `cel_has`. When the condition evaluates to `false`, CELS automatically detects the omitted component and stages `CELS_OP_REMOVE` for you!

### ❌ Anti-Pattern 3: Trying to Overwrite `cel_id()`
```c
// BAD: Trying to assign to cel_id()
cel_id() = 1234; // Compiler error! cel_id() is strictly a query macro
```
**Fix**: Use keyed composables `CEL_Composable(Name, id, ...)` or scoped blocks `cel_key(id) { ... }`.
