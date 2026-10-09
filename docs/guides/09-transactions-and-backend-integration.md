# Transactions & External Backend Integration Guide

This guide details how **CELS** declarative UI and composition trees integrate with external execution engines and backends (such as Entity Component Systems, GPU render pipelines, audio engines, or physics simulations). It explains how developers write clean, declarative composables using `cel_has`, `cel_has_tag`, `cel_get`, and `cel_is`, and how engine hosts bridge these operations to external backends without locking or boilerplate.

---

## 1. Architectural Mental Model

CELS and external backends serve complementary roles:
- **External Backend**: Authoritative execution backend, cache-dense arrays, multi-threaded simulation, GPU drawing, or physics.
- **CELS**: Reactive declarative frontend, hierarchical trees, UI layout, temporal easing, and declarative component state.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                             CELS Composable Tree                            │
│  CEL_Composable(EntityView, id, const EntityState *entity) {                │
│      cel_has(Position, { entity->x, entity->y, entity->z });                │
│      if (entity->isBurning) cel_has(BurnDebuff, { .dps = 25.0f });          │
│  }                                                                          │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │ Declarative Intra-Frame Diffing
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                     CELS Transaction Staging Batch                          │
│  - Op: SET    | Target: 0x1001 | Type: Position   | Data: { ... }           │
│  - Op: REMOVE | Target: 0x1001 | Type: BurnDebuff | (omitted this frame!)   │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │ CelsSessionCommitTransactions (Frame Sync)
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         External Engine Backend                             │
│  (Renderer, Physics Engine, Audio DSP Graph, or ECS Archetype Storage)      │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Writing Declarative Composables with Components

Developers building application UI or entity views **never** interact directly with backend mutexes, handles, or manual remove calls. Instead, they use declarative composables:

### 2.1 Keyed Composables (`CEL_Composable(Name, id, ...)`)

When declaring a composable that represents an external target entity, declare `id` directly in the signature:

```c
#include "cels.h"
#include "entity_components.h"

CEL_Composable(CombatEntityNode, id, const CombatEntityState*, ent)
{
    cel_watch(ent);
    if (!ent->isAlive) return;

    /* 1. Baseline components (automatically bound to id / cel_id()) */
    cel_has(Position, { .x = ent->x, .y = ent->y, .z = ent->z });
    cel_has(Health,   { .current = ent->hp, .max = ent->maxHp });

    /* 2. Zero-sized tag component */
    if (ent->isBoss) {
        cel_has_tag(BossTag);
    }

    /* 3. Conditional Component:
     * When hasShield is true: stages CELS_OP_SET.
     * When hasShield becomes false: CELS automatically stages CELS_OP_REMOVE! */
    if (ent->hasShield) {
        cel_has(EnergyShield, {
            .capacity = ent->shieldCapacity,
            .rechargeRate = 5.0f
        });
    }

    /* 4. Conditional Debuff */
    if (ent->isBurning) {
        cel_has(BurnDebuff, {
            .dps = ent->burnDps,
            .remainingSeconds = 3.0f
        });
    }

    /* 5. Intra-Frame Queries */
    const Health *hp = cel_get(Health);
    if (hp && hp->current < (hp->max * 0.25f)) {
        cel_has_tag(LowHealthAlert);
    }
}
```

### 2.2 Invocation

Callers invoke the composable by passing the entity ID as the first argument:

```c
CEL_Composition(CombatAppComposition)
{
    CombatEntityState *ent = cel_remember_state(CombatEntityState, {
        .entityId = 0x1001,
        .hp = 100, .maxHp = 100,
        .hasShield = true,
        .isAlive = true
    });

    /* Mount the entity composable node when alive, passing entityId directly */
    if (ent->isAlive) {
        CombatEntityNode(ent->entityId, ent);
    }
}
```

### 2.3 List Reconciliation with External Entities

For dynamic lists of entities, iterate over items and invoke each keyed composable:

```c
CEL_Composable(EnemyListView, const EnemyModel *model)
{
    for (int i = 0; i < model->count; i++) {
        CombatEntityNode(model->enemies[i].id, &model->enemies[i]);
    }
}
```

---

## 3. The Declarative Reconciliation Invariant

In traditional imperative backend programming, omitting a component requires remembering to manually remove it:
```c
// IMPERATIVE (Error-Prone):
if (ent->hasShield) {
    backend_set(world, e, EnergyShield, { ... });
} else {
    backend_remove(world, e, EnergyShield); // Forgetting this causes stale leaks!
}
```

In CELS, component presence is **declarative**:
1. **Mount (Frame 1)**: When `cel_has` executes, it records the component type in the active composable's `currTypes` set and stages `CELS_OP_SET`.
2. **Recomposition Diff (Frame 2)**: If `ent->hasShield` becomes `false`, the `if` block does not execute. On exiting the composable scope, CELS diffs `prevTypes` against `currTypes`. Any type present in `prevTypes` but omitted from `currTypes` is **automatically staged as `CELS_OP_REMOVE`**.
3. **Unmount (Frame 3)**: When `ent->isAlive` becomes `false` or an item is deleted from a dynamic list, the composable node is pruned from the slot table. CELS invokes the entity tracker unmount destructor, which **automatically stages `CELS_OP_DELETE`** to destroy the backend entity.

---

## 4. Host Engine Integration: Bridging CELS to Backends

To connect CELS to your external backend in the host executable, configure two hooks during session setup:

### 4.1 Step 1: Register the Backend Lookup Hook (Tier 2 Queries)

Register a callback so `cel_get` and `cel_is` can inspect live backend components when intra-frame staged data is not present:

```c
static const void *BackendLookupHook(uint64_t entityId, uint64_t typeKey, void *userData)
{
    AppBackend *backend = (AppBackend*)userData;

    if (!backend->isEntityAlive(backend->world, entityId)) return NULL;

    /* Map 64-bit typeKey hash to backend component memory */
    return backend->getComponent(backend->world, entityId, typeKey);
}

// In host setup:
CelsSessionSetEcsLookupHook(&session, BackendLookupHook, backendWorld);
```

### 4.2 Step 2: Commit Staged Transactions Post-Recompose

At the frame synchronization boundary (after `CelsSessionRecompose`), commit all staged operations to the backend:

```c
static void BackendCommitHandler(CelsOpCode op,
                                 uint64_t entityId,
                                 uint64_t typeKey,
                                 const void *data,
                                 size_t size,
                                 void *userData)
{
    AppBackend *backend = (AppBackend*)userData;

    switch (op) {
        case CELS_OP_SET: {
            backend->setComponent(backend->world, entityId, typeKey, data, size);
            break;
        }

        case CELS_OP_REMOVE: {
            backend->removeComponent(backend->world, entityId, typeKey);
            break;
        }

        case CELS_OP_DELETE: {
            backend->destroyEntity(backend->world, entityId);
            break;
        }

        default:
            if (op > CELS_OP_CUSTOM) {
                backend->dispatchCustom(backend, op, entityId, data, size);
            }
            break;
    }
}

// In host tick loop:
void HostFrameTick(CelsSession *session, AppBackend *backend)
{
    /* 1. Recompose declarative trees */
    CelsSessionRecompose(session);

    /* 2. Commit all staged operations to the backend */
    CelsSessionCommitTransactions(session, BackendCommitHandler, backend);

    /* 3. Advance backend simulation or render frame */
    backend->tick(backend->world, deltaTime);
}
```

*(Note: When integrating with an ECS framework like Flecs, `BackendCommitHandler` simply maps `CELS_OP_SET` to `ecs_set_id`, `CELS_OP_REMOVE` to `ecs_remove_id`, and `CELS_OP_DELETE` to `ecs_delete`.)*

---

## 5. Summary of Best Practices

| Rule | Description |
| :--- | :--- |
| **Pass `id` in `CEL_Composable(Name, id, ...)`** | Always declare entity composables with an explicit `id` parameter. |
| **Use `cel_has` for Data, `cel_has_tag` for Flags** | Components hold POD structs; tags are zero-sized marker structs. |
| **Never Imperatively Call Backend Remove** | Let CELS omission diffing stage removals automatically. |
| **Commit Transactions in the Host Loop** | Keep composables free of transaction hooks and external backend pointers. |
| **Use `cel_is` & `cel_get` for Immediate Reads** | Query staged state immediately within the active frame. |
