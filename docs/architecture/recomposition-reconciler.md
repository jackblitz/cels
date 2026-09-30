# CELS Architecture: Recomposition Reconciler & Tree Traversal

## 1. Architectural Motivation & Problem Statement

In reactive UI frameworks and game engine scene graphs, the cost of re-evaluating the entire component tree every frame scales with the total number of mounted nodes: $\mathcal{O}(N)$. In a game scene or complex HUD containing $10{,}000$ elements, re-evaluating every node on every $16.6\text{ ms}$ ($60\text{ FPS}$) or $4.16\text{ ms}$ ($240\text{ FPS}$) tick quickly consumes the entire frame budget in CPU overhead, branch mispredictions, and cache eviction.

CELS solves this through a **monotonic, multi-pass hierarchical reconciler** that guarantees:
1. **$\mathcal{O}(1)$ Subtree Skipping**: If a component and its transitive descendants have not been invalidated, the entire subtree is bypassed in a single CPU instruction without descending or inspecting child nodes.
2. **Fine-Grained Upward Invalidation**: State mutations mark target leaf nodes dirty and propagate a single `CONTAINS_INVALIDATED` bit upward along the parent chain to the root, guaranteeing that the traversal descends *only* along paths leading directly to dirty nodes.
3. **In-Place Structural Reordering**: Reordered siblings are recognized by stable 64-bit keys and relocated within the gap buffer via memory blocks, preserving all child state and slot allocations without remounting.
4. **Deterministic Convergence**: Cascading state mutations occurring during recomposition are iteratively drained up to a bounded limit ($M \le \text{maxDrainIterations}$), preventing infinite recomposition loops.

```
                     +---------------------------------------+
                     |         CelsSessionRecompose          |
                     +---------------------------------------+
                                         |
                                         v
                     +---------------------------------------+
                     |         DrainInvalidationQueue        |
                     |  - Targets:   INVALIDATED             |
                     |  - Ancestors: CONTAINS_INVALIDATED    |
                     +---------------------------------------+
                                         |
                                         v
                     +---------------------------------------+
                     |        Tree Traversal Iteration       |
                     +---------------------------------------+
                      /                 |                   \
                     /                  |                    \
       [ Clean Subtree ]      [ Ancestor Path ]       [ Dirty Node ]
              |                         |                    |
        logicalCursor +=          Descend into         Execute body,
        (1 + groupSize)           children             clear flags
        Return FALSE              Return TRUE          Return TRUE
        (O(1) Bypassed!)                |                    |
                                        +--------------------+
                                         |
                                         v
                     +---------------------------------------+
                     |            CelsExitGroup              |
                     |  - Detect unvisited groups            |
                     |  - Prune dead subtrees & fire cleanups|
                     |  - Finalize groupSize & nodeCount     |
                     +---------------------------------------+
```

---

## 2. Invalidation Topology & Upward Signal Propagation

Tree evaluation in CELS moves **downward** (depth-first pre-order from the root to leaves). However, reactive mutations occur at **leaves** or arbitrary internal nodes. To allow the downward traversal to skip clean branches, dirty signals must travel **upward** prior to traversal.

### 2.1 The Invalidation Flags Bitmask

Stored inside `CelsSlotGroup.flags` (16-bit field):

```c
typedef enum CelsGroupFlags {
    CELS_GROUP_FLAG_NONE                 = 0u,
    CELS_GROUP_FLAG_INVALIDATED          = 1u << 0, /* This node's body must re-run */
    CELS_GROUP_FLAG_CONTAINS_INVALIDATED = 1u << 1, /* Descendant is dirty; do not skip */
    CELS_FLAG_FRESH_MOUNT                = 1u << 2  /* First time mounting in tree */
} CelsGroupFlags;
```

### 2.2 Upward Propagation Algorithm (`DrainInvalidationQueue`)

When a mutation occurs (e.g. `cel_mutate`), the subscriber group keys are queued into `s->invalidationQueue`. Before the traversal starts, `DrainInvalidationQueue` processes each target key:

```c
static void DrainInvalidationQueue(CelsSession *s)
{
    const uint32_t totalGroups = CelsGetLogicalGroupCount(s);

    while (s->queueCount > 0) {
        const uint64_t targetKey = s->invalidationQueue[--s->queueCount];

        for (uint32_t i = 0; i < totalGroups; ++i) {
            CelsSlotGroup *const g = CelsGetGroup(s, i);
            if (g->key == targetKey) {
                /* 1. Mark target group and all transitive descendants */
                g->flags |= CELS_FLAG_INVALIDATED;
                for (uint32_t c = i + 1; c <= i + g->groupSize && c < totalGroups; ++c) {
                    CelsGetGroup(s, c)->flags |= CELS_FLAG_INVALIDATED;
                }

                /* 2. Propagate CONTAINS_INVALIDATED upward to root */
                if (i > 0) {
                    uint32_t curr = g->parentIndex;
                    while (true) {
                        CelsSlotGroup *const p = CelsGetGroup(s, curr);
                        if ((p->flags & CELS_FLAG_CONTAINS_INVALIDATED) != 0) {
                            break; /* Early termination: ancestor already marked */
                        }
                        p->flags |= CELS_FLAG_CONTAINS_INVALIDATED;
                        if (curr == 0) break;
                        curr = p->parentIndex;
                    }
                }
                break;
            }
        }
    }
}
```

```
               [ Root: Group 0 ]                  flags |= CONTAINS_INVALIDATED
                      |
              [ Panel: Group 1 ]                 flags |= CONTAINS_INVALIDATED
               /              \
     [ Menu: Group 2 ]     [ List: Group 4 ]     flags |= CONTAINS_INVALIDATED
     (Clean, flags = 0)           |
                          [ Item: Group 5 ]      flags |= INVALIDATED (Target!)
                                  |
                          [ Text: Group 6 ]      flags |= INVALIDATED (Cascaded)
```

#### Asymptotic Complexity & Early Termination:
- **Upward Chain**: $\mathcal{O}(D)$ where $D \le \text{depth} \le 32$.
- **Early Termination**: When an ancestor is reached that *already* carries `CELS_FLAG_CONTAINS_INVALIDATED`, propagation halts immediately because that ancestor's parent chain is already marked.
- **Descending Walk Guarantees**: Any branch that does *not* carry `CONTAINS_INVALIDATED` or `INVALIDATED` is provably free of dirty nodes.

---

## 3. Fast-Path Quiet Frame Bypass

In games and real-time simulations, most frames experience zero state mutations. Executing tree iterations during quiet frames wastes CPU cycles.

CELS implements an $\mathcal{O}(1)$ early-out check at the start of `CelsSessionRecompose`:

```c
if (s->hasComposedOnce && s->queueCount == 0 && !s->isHotReloadPending) {
    if (hasDirtyState) {
        CelsStatePublishDirty(s);
    }
    return CELS_OK;
}
```

When no state mutations occurred and no hot-reload is pending:
- Zero nodes are traversed.
- Zero function pointers are called.
- The reconciler completes in $< 20\text{ nanoseconds}$, consuming effectively 0% CPU.

---

## 4. The Traversal Machine & Stack Topology

Traversal is tracked through parallel static depth stacks inside `CelsSession`, avoiding heap allocation or recursion:

```c
#define CELS_MAX_DEPTH 64u

struct CelsSession {
    ...
    uint32_t currentDepth;
    uint32_t currentGroupIndex;
    uint32_t currentSlotOffset;
    uint32_t logicalCursor;

    uint8_t  activeStack[CELS_MAX_DEPTH];        /* 1 = body active, 0 = skipped */
    uint32_t groupIndexStack[CELS_MAX_DEPTH];    /* Logical group index at depth */
    uint32_t oldGroupSizeStack[CELS_MAX_DEPTH];  /* Subtree size prior to pass */
    uint32_t slotOffsetStack[CELS_MAX_DEPTH];    /* Slot memory cursor at depth */
    ...
};
```

---

## 5. $\mathcal{O}(1)$ Subtree Skipping Mechanics

The central performance feature of CELS is the $\mathcal{O}(1)$ subtree bypass. When `CelsEnterComposable` encounters an existing group matching `key`:

```c
CelsSlotGroup *const cached = CelsGetGroup(s, cursor);

/* Check if the group or any descendant was invalidated */
if (!(cached->flags & (CELS_FLAG_INVALIDATED | CELS_FLAG_CONTAINS_INVALIDATED))) {
    s->activeStack[depth] = 0;
    s->groupIndexStack[depth] = cursor;
    s->logicalCursor += (1u + (uint32_t)cached->groupSize);
    return false; /* Skip execution of the composable body! */
}
```

### 5.1 Proof of $\mathcal{O}(1)$ Complexity

Let a clean subtree $T$ contain $K$ descendant groups ($K = \text{cached->groupSize}$).

1. **Inspection**: The reconciler inspects `cached->flags` ($\mathcal{O}(1)$ memory read).
2. **Decision**: Both `INVALIDATED` and `CONTAINS_INVALIDATED` are zero.
3. **Skip**: The reconciler executes:
   $$\text{logicalCursor} \leftarrow \text{logicalCursor} + 1 + K$$
4. **Return**: The function returns `false`. The C preprocessor macro `if (CelsEnterComposable(...))` skips the compound statement containing the component body.

The reconciler executes exactly **zero** reads or writes on the $K$ descendant groups. Whether $K = 5$ or $K = 50{,}000$, the skip executes in constant time $\mathcal{O}(1)$.

---

## 6. Sibling Reordering & Permutation Mechanics

In dynamic lists (inventories, leaderboards, entity tables), items often reorder without changing internal state. In traditional systems, reordering either destroys and recreates components (dropping focus and animations) or requires complex LCS (Longest Common Subsequence) diffing algorithms running in $\mathcal{O}(N \times M)$ time.

CELS achieves **in-place block relocation** in linear time $\mathcal{O}(N)$ using its gap buffer.

### 6.1 Sibling Search & Match

When `CelsEnterComposable` executes at `cursor` under parent `parentIdx`:

```c
const uint32_t parentEnd = parentIdx + 1 + CelsGetGroup(s, parentIdx)->groupSize;
uint32_t matchIdx = UINT32_MAX;

for (uint32_t i = cursor; i < parentEnd; ) {
    CelsSlotGroup *const candidate = CelsGetGroup(s, i);
    if (candidate->key == key) {
        matchIdx = i;
        break;
    }
    i += 1u + (uint32_t)candidate->groupSize; /* Jump across sibling subtrees */
}
```

By incrementing $i$ by $1 + \text{groupSize}$, the loop checks only sibling roots, skipping their children.

### 6.2 In-Place Subtree Relocation

If `matchIdx != cursor`, the matching group exists further ahead in the sibling list. CELS relocates the entire subtree forward to `cursor` in a single contiguous block copy:

```c
if (matchIdx != UINT32_MAX && matchIdx != cursor) {
    const uint32_t movedCount = 1u + (uint32_t)CelsGetGroup(s, matchIdx)->groupSize;
    CelsSlotGroup stackMoved[64];
    CelsSlotGroup *moved = (movedCount <= 64)
        ? stackMoved
        : (CelsSlotGroup *)malloc(movedCount * sizeof(CelsSlotGroup));

    MoveGroupGap(s, totalGroups);

    /* 1. Extract moving subtree */
    memcpy(moved, &s->groups[matchIdx], movedCount * sizeof(moved[0]));

    /* 2. Shift intervening siblings right */
    memmove(&s->groups[cursor + movedCount],
            &s->groups[cursor],
            (matchIdx - cursor) * sizeof(moved[0]));

    /* 3. Insert subtree at cursor */
    memcpy(&s->groups[cursor], moved, movedCount * sizeof(moved[0]));

    if (moved != stackMoved) free(moved);

    /* 4. Fixup parentIndex links for moved and shifted nodes */
    for (uint32_t i = 1; i < totalGroups; ++i) {
        const uint32_t parent = s->groups[i].parentIndex;
        if (parent >= matchIdx && parent < matchIdx + movedCount) {
            s->groups[i].parentIndex = cursor + (parent - matchIdx);
        } else if (parent >= cursor && parent < matchIdx) {
            s->groups[i].parentIndex = parent + movedCount;
        }
    }
}
```

```
Before Relocation:
Cursor: 1
[ P ][ S0 ][ S1 ][ S2 (moved) ][ S3 ]
     ^           ^
     cursor      matchIdx (Key matches S2)

After memmove & memcpy:
[ P ][ S2 (moved) ][ S0 ][ S1 ][ S3 ]
     ^
     cursor (S2 is now at cursor; all internal state preserved!)
```

#### Guarantees:
- Subtrees $\le 64$ groups require **zero heap allocations** (`stackMoved[64]`).
- Internal descendant relationships, slot memory offsets, and cleanups remain intact.
- Pointers returned by `cel_remember` inside the moved component do **not** invalidate.

---

## 7. Group Exit & Unvisited Subtree Pruning

When execution of a composable body finishes, `CelsExitGroup` is invoked.

### 7.1 Detecting Unvisited (Dead) Branches

In declarative C, conditional rendering is expressed via standard C `if` statements:

```c
if (player->isAlive) {
    HealthBar();
    InventoryView();
}
```

When `player->isAlive` becomes `false`, the execution skips the calls to `HealthBar()` and `InventoryView()`.

During `CelsExitGroup`:

```c
const uint32_t depth = --s->currentDepth;
const uint32_t groupIdx = s->groupIndexStack[depth];

if (s->activeStack[depth]) {
    uint32_t expectedEnd = groupIdx + 1 + CelsGetGroup(s, groupIdx)->groupSize;
    while (s->logicalCursor < expectedEnd && s->logicalCursor < CelsGetLogicalGroupCount(s)) {
        CelsSlotGroup *const dead = CelsGetGroup(s, s->logicalCursor);
        const uint32_t removed = 1u + (uint32_t)dead->groupSize;
        CelsPruneSubtree(s, s->logicalCursor);
        expectedEnd -= removed;
    }
}
```

If `s->logicalCursor < expectedEnd`, there are trailing child nodes from the previous frame that were **not visited** in this frame.

### 7.2 Pruning Mechanics (`CelsPruneSubtree`)

`CelsPruneSubtree(s, rootLogicalIndex)` executes a structured 5-stage teardown:
1. **Fire Cleanups**: Iterates all descendant groups in reverse order ($\text{groupsToRemove} \to 1$) and executes any registered `onDestroy` callbacks (e.g. closing network sockets, releasing textures, stopping audio).
2. **Unsubscribe State Watchers**: Removes the dead group keys from all `CelsStateCell.watcherKeys` arrays, preventing dangling callbacks or stale invalidations.
3. **Reclaim Slot Memory**: Releases data arena allocations associated with the pruned groups.
4. **Update Ancestor `groupSize`**: Climbs `parentIndex` chains to the root, decrementing `groupSize` by `groupsToRemove`.
5. **Compact Gap Buffer**: Calls `memmove` to close the group array gap and shifts `parentIndex` values.

---

## 8. Multi-Pass Recomposition & Convergence Guarantees

State mutations triggered during a recomposition pass (e.g. cascading reactive updates) push new keys into `invalidationQueue`.

To ensure system stability, `CelsSessionRecompose` wraps traversal in a drain loop:

```c
uint32_t iterations = 0;
s->isRecomposing = true;

do {
    if (++iterations > s->maxDrainIterations) {
        s->isRecomposing = false;
        s_currentSession = prevSession;
        return CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE;
    }

    DrainInvalidationQueue(s);

    s->currentDepth = 0;
    s->currentSlotOffset = 0;
    s->logicalCursor = 0;

    /* Execute all attached compositions */
    for (uint32_t i = 0; i < s->attachedCount; ++i) {
        ...
    }
} while (s->queueCount > 0);
```

### Convergence Theorem:
Let $\mathcal{G}$ be the set of mounted composables, and $M = \text{maxDrainIterations}$ (default 8).
- If state dependencies form a Directed Acyclic Graph (DAG), the loop converges in $k \le \operatorname{depth}(\text{DAG})$ iterations.
- If circular dependencies exist (e.g. Composable A mutates State B, which invalidates Composable A), the loop aborts deterministically at iteration $M + 1$, returning `CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE` without hanging the game thread.
