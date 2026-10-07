# Flecs ECS Integration Guide

This guide details how **CELS** declarative UI and composition trees integrate with **Flecs** (or any external Entity Component System). It explains how developers write clean, declarative composables using `cel_has`, `cel_has_tag`, `cel_get`, and `cel_is`, and how engine hosts bridge these operations to live Flecs entities without locking or boilerplate.

---

## 1. Architectural Mental Model

CELS and Flecs serve complementary roles:
- **Flecs**: Authoritative simulation backend, cache-dense archetype tables, multi-threaded systems, queries, and physics.
- **CELS**: Reactive declarative frontend, hierarchical trees, UI layout, temporal easing, and declarative component state.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                             CELS Composable Tree                            │
│  CEL_Composable(EnemyView, id, const EnemyState *enemy) {                   │
│      cel_has(Position, { enemy->x, enemy->y, enemy->z });                   │
│      if (enemy->isBurning) cel_has(BurnDebuff, { .dps = 25.0f });           │
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
│                              Flecs ECS World                                │
│  - ecs_set_id(world, 0x1001, ecs_id(Position), sizeof(Position), &pos)     │
│  - ecs_remove_id(world, 0x1001, ecs_id(BurnDebuff))                         │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Writing Declarative Composables with Components

Developers building application UI or entity views **never** interact directly with Flecs handles, mutexes, or manual remove calls. Instead, they use declarative composables:

### 2.1 Keyed Composables (`CEL_Composable(Name, id, ...)`)

When declaring a composable that represents an ECS entity, declare `id` directly in the signature:

```c
#include "cels.h"
#include "combat_components.h"

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

### 2.3 List Reconciliation with Flecs Entities

For lists of entities, iterate over items and invoke each keyed composable:

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

In traditional imperative ECS programming, omitting a component requires remembering to manually remove it:
```c
// IMPERATIVE (Error-Prone):
if (ent->hasShield) {
    ecs_set(world, e, EnergyShield, { ... });
} else {
    ecs_remove(world, e, EnergyShield); // Forgetting this causes stale component leaks!
}
```

In CELS, component presence is **declarative**:
1. **Mount (Frame 1)**: When `cel_has` executes, it records the component type in the active composable's `currTypes` set and stages `CELS_OP_SET`.
2. **Recomposition Diff (Frame 2)**: If `ent->hasShield` becomes `false`, the `if` block does not execute. On exiting the composable scope, CELS diffs `prevTypes` against `currTypes`. Any type present in `prevTypes` but omitted from `currTypes` is **automatically staged as `CELS_OP_REMOVE`**.
3. **Unmount (Frame 3)**: When `ent->isAlive` becomes `false` or an item is deleted from a dynamic list, the composable node is pruned from the slot table. CELS invokes the entity tracker unmount destructor, which **automatically stages `CELS_OP_DELETE`** to destroy the Flecs entity!

---

## 4. Host Engine Integration: Bridging CELS to Flecs

To connect CELS to your Flecs world in the host executable, configure two hooks during session setup:

### 4.1 Step 1: Register the ECS Lookup Hook (Tier 2 Queries)

Register a callback so `cel_get` and `cel_is` can inspect live Flecs components when intra-frame staged data is not present:

```c
static const void *FlecsLookupHook(uint64_t entityId, uint64_t typeKey, void *userData)
{
    ecs_world_t *world = (ecs_world_t*)userData;
    ecs_entity_t e = (ecs_entity_t)entityId;

    if (!ecs_is_alive(world, e)) return NULL;

    /* Map 64-bit typeKey hash to Flecs component ID */
    if (typeKey == CelsHashKey("Position")) {
        return ecs_get(world, e, Position);
    } else if (typeKey == CelsHashKey("Health")) {
        return ecs_get(world, e, Health);
    } else if (typeKey == CelsHashKey("EnergyShield")) {
        return ecs_get(world, e, EnergyShield);
    }

    return NULL;
}

// In host setup:
CelsSessionSetEcsLookupHook(&session, FlecsLookupHook, ecsWorld);
```

### 4.2 Step 2: Commit Staged Transactions Post-Recompose

At the frame synchronization boundary (after `CelsSessionRecompose`), commit all staged operations directly to Flecs:

```c
static void FlecsCommitHandler(CelsOpCode op,
                               uint64_t entityId,
                               uint64_t typeKey,
                               const void *data,
                               size_t size,
                               void *userData)
{
    ecs_world_t *world = (ecs_world_t*)userData;
    ecs_entity_t e = (ecs_entity_t)entityId;

    switch (op) {
        case CELS_OP_SET: {
            ecs_id_t compId = ResolveFlecsComponentId(world, typeKey);
            if (size > 0 && data != NULL) {
                ecs_set_id(world, e, compId, size, data);
            } else {
                /* Zero-sized tag component */
                ecs_add_id(world, e, compId);
            }
            break;
        }

        case CELS_OP_REMOVE: {
            ecs_id_t compId = ResolveFlecsComponentId(world, typeKey);
            ecs_remove_id(world, e, compId);
            break;
        }

        case CELS_OP_DELETE: {
            if (ecs_is_alive(world, e)) {
                ecs_delete(world, e);
            }
            break;
        }

        default:
            break;
    }
}

// In host tick loop:
void HostFrameTick(CelsSession *session, ecs_world_t *world)
{
    /* 1. Recompose declarative trees */
    CelsSessionRecompose(session);

    /* 2. Commit all staged operations to Flecs */
    CelsSessionCommitTransactions(session, FlecsCommitHandler, world);

    /* 3. Advance Flecs simulation */
    ecs_progress(world, deltaTime);
}
```

---

## 5. Summary of Best Practices

| Rule | Description |
| :--- | :--- |
| **Pass `id` in `CEL_Composable(Name, id, ...)`** | Always declare entity composables with an explicit `id` parameter. |
| **Use `cel_has` for Data, `cel_has_tag` for Flags** | Components hold POD structs; tags are zero-sized marker structs. |
| **Never Imperatively Call `ecs_remove`** | Let CELS omission diffing stage removals automatically. |
| **Commit Transactions in the Host Loop** | Keep composables free of transaction hooks and Flecs world pointers. |
| **Use `cel_is` & `cel_get` for Immediate Reads** | Query staged state immediately within the active frame. |
