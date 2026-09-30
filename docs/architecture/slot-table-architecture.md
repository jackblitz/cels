# CELS Architecture: Contiguous Slot Table & Dual Gap Buffer

## 1. Executive Summary & Design Philosophy

The central design goal of CELS (Composition, Evaluation, Lifecycle, and State) is to provide declarative, reactive hierarchical UI and state management in pure ISO C99 with **zero runtime heap allocations**, **provable asymptotic bounds**, and **cache-optimal data-oriented memory layouts**.

In conventional object-oriented or virtual-DOM architectures (such as React, Qt, or DOM trees), component trees are represented as networks of individually allocated heap nodes connected by pointers. This pattern produces catastrophic memory fragmentation, cache misses (pointer chasing through non-contiguous memory), heap contention across threads, and unpredictable garbage collection or reference-counting overheads.

CELS fundamentally rejects this paradigm. Instead, CELS models the entire hierarchical composition tree, its execution topology, and its persistent component-local variables inside a **single, contiguous, 64-byte cache-line aligned memory slab**. Within this slab, structural hierarchy and payload state are managed via dual gap buffers and nonmoving arena partitions.

```
+===================================================================================================+
|                               CONTIGUOUS 64-BYTE ALIGNED MEMORY SLAB                             |
+====================================+==============================+===============================+
|        GROUPS GAP BUFFER           |    SLOT ALLOCATION TABLE     |        NONMOVING ARENA        |
|      CelsSlotGroup[maxGroups]      |    CelsSlotAllocation[...]   |     uint8_t[dataArenaSize]    |
|   (32 B each, 2 per cache line)    |      (16 B each, 4 per CL)   |   (Pinned state, 8B aligned)  |
+====================================+==============================+===============================+
^                                    ^                              ^
slabBase                             slabBase + (maxGroups * 32)    slabBase + (maxGroups * 48)
```

---

## 2. Slab Partitioning & Cache-Line Alignment

Modern x86-64 and ARM64 CPUs organize L1 data caches into 64-byte cache lines. Accessing data that straddles a 64-byte boundary incurs a penalty (split loads/stores or multiple line fetches). False sharing occurs if separate threads mutate data residing on the same line.

CELS guarantees strict 64-byte cache alignment across all allocations.

### 2.1 Memory Allocation & Alignment Invariants

The slab can be allocated dynamically at initialization or statically embedded in `.bss` for safety-critical and embedded bare-metal systems:

```c
#define CELS_CACHE_LINE_SIZE 64u

#ifndef CEL_SLAB
#if defined(_MSC_VER)
#define CEL_SLAB(name, size) __declspec(align(64)) uint8_t name[size]
#else
#define CEL_SLAB(name, size) __attribute__((aligned(64))) uint8_t name[size]
#endif
#endif
```

When dynamically allocated via `CelsAllocAlignedSlab(size)`:
- On Windows: `_aligned_malloc(size, CELS_CACHE_LINE_SIZE)`
- On POSIX: `posix_memalign(&ptr, CELS_CACHE_LINE_SIZE, size)` or `aligned_alloc(64, size)`
- Alignment assertion: `((uintptr_t)slabMemory % CELS_CACHE_LINE_SIZE) == 0`

### 2.2 Mathematical Sizing & Slab Partitioning

Given a total slab capacity $S$ (in bytes) and a maximum group capacity $N = \text{maxGroups}$ (clamped to multiples of 4 and $\ge 16$):

$$\text{GroupBytes} = N \times \operatorname{sizeof}(\text{CelsSlotGroup}) = N \times 32$$

$$\text{SlotBytes} = N \times \operatorname{sizeof}(\text{CelsSlotAllocation}) = N \times 16$$

$$\text{DataArenaSize} = S - (\text{GroupBytes} + \text{SlotBytes}) = S - (48 \times N)$$

For a **64 KiB slab** ($S = 65{,}536\text{ B}$) with $N = 512$ groups:
- **Groups Partition**: $512 \times 32\text{ B} = 16{,}384\text{ B}$ (exactly 256 cache lines)
- **Slots Partition**: $512 \times 16\text{ B} = 8{,}192\text{ B}$ (exactly 128 cache lines)
- **Data Arena Partition**: $65{,}536 - 24{,}576 = 40{,}960\text{ B}$ (exactly 640 cache lines)

Every partition starts at a multiple of 64 bytes. No structural header or record straddles an L1 cache line boundary.

### 2.3 Intent-Driven Workload Capacity Profiles (`CelsSessionProfile`)

Rather than forcing developers to calculate raw byte offsets and partition boundaries manually, CELS provides declarative **workload capacity profiles** (`CelsSessionProfile`). Developers define session scale by **how many composables they expect to use**:

| Profile | Target Capacity | Slab Size | Data Arena Partition | Best Used For |
| :--- | :--- | :--- | :--- | :--- |
| `CELS_PROFILE_DEFAULT` | 4,096 composables | 512 KiB | ~320 KiB | Default standard capacity if unspecified |
| `CELS_PROFILE_128` | 128 composables | 16 KiB | ~10 KiB | Micro-dialogs, tiny popups, embedded widgets |
| `CELS_PROFILE_256` | 256 composables | 32 KiB | ~20 KiB | HUD overlays, tooltips, sub-panels |
| `CELS_PROFILE_512` | 512 composables | 64 KiB | ~40 KiB | L1/L2 cache-resident UI panels & focused subtrees |
| `CELS_PROFILE_1K` | 1,024 composables | 128 KiB | ~80 KiB | Standard application windows & forms |
| `CELS_PROFILE_2K` | 2,048 composables | 256 KiB | ~160 KiB | Complex screens with many active lists & controls |
| `CELS_PROFILE_4K` | 4,096 composables | 512 KiB | ~320 KiB | Broad applications with multiple concurrent modules |
| `CELS_PROFILE_8K` | 8,192 composables | 1 MiB | ~640 KiB | Heavy simulation trees & large hierarchies |

**Context-Neutral Design**: CELS powers UI trees, audio DSP graphs, simulation actor hierarchies, and network state machines alike. Numeric capacity names describe the workload scale without artificially binding the engine to graphical interfaces.

**Sizing Priority in `CelsSessionInit`**:
1. Explicit `.slab` buffer or `.slabSize > 0`: Direct user override.
2. Estimated composable count `.maxComposables > 0`: CELS automatically selects the smallest optimal power-of-two profile slab that safely accommodates the count.
3. Named profile `.profile != CELS_PROFILE_DEFAULT`: Uses `CelsSlabSizeFromProfile(profile)`.
4. Default: Falls back to `CELS_DEFAULT_SLAB_SIZE` (512 KiB, 4,096 groups).

---

## 3. Structural Node Topology: `CelsSlotGroup`

Every composable or composition node mounted in the hierarchy is represented by a single `CelsSlotGroup` record.

### 3.1 Field Packing and 32-Byte Geometry

Struct fields are arranged in descending order of size ($64\text{-bit} \to 32\text{-bit} \to 16\text{-bit}$) to eliminate compiler padding entirely:

```c
typedef struct CelsSlotGroup {
    uint64_t key;          /* Offset  0: Stable callsite hash / key (8 bytes) */
    uint64_t userData;     /* Offset  8: Dense group identity / unique GID (8 bytes) */
    uint32_t parentIndex;  /* Offset 16: Logical index of parent group (4 bytes) */
    uint32_t slotIndex;    /* Offset 20: Absolute byte offset into data arena (4 bytes) */
    uint16_t slotCount;    /* Offset 24: Cumulative payload bytes in arena (2 bytes) */
    uint16_t groupSize;    /* Offset 26: Transitive child group count in subtree (2 bytes) */
    uint16_t nodeCount;    /* Offset 28: Caller-defined materialized node tally (2 bytes) */
    uint16_t flags;        /* Offset 30: Invalidation & lifecycle bitmask (2 bytes) */
} CelsSlotGroup;
```

```
Byte Offset:
 0                   8                  16        20        24   26   28   30   32
+-------------------+-------------------+---------+---------+----+----+----+----+
|        key        |     userData      |parentIdx|slotIndex|slot|grp |node|flag|
|      (uint64)     |     (uint64)      | (uint32)| (uint32)| cnt|size| cnt|bits|
+-------------------+-------------------+---------+---------+----+----+----+----+
|                  First Group                  |                  Second Group                 |
+===============================================+===============================================+
|                                64-BYTE CPU CACHE LINE                                         |
+===============================================================================================+
```

### 3.2 Cache Line Packing

Because $\operatorname{sizeof}(\text{CelsSlotGroup}) = 32\text{ bytes}$:
- Exactly **two groups** fit into a single 64-byte cache line.
- A sequential traversal of 100 composable nodes requires fetching exactly 50 cache lines.
- No split cache lines occur during sequential tree iterations.

---

## 4. Dual Gap Buffer Architecture

CELS utilizes two distinct gap buffer mechanisms within the same slab:
1. **The Structural Groups Gap Buffer**: Accommodates inserting, reordering, and deleting subtree nodes in linear traversal order.
2. **The Nonmoving Data Arena**: Employs an inward-growing linear allocator to guarantee pinned memory addresses for remembered values and state cells.

### 4.1 Structural Groups Gap Buffer Mechanics

The group array stores the depth-first pre-order traversal of the composable tree. To avoid $O(N)$ copies on every dynamic node insertion or deletion, the array is partitioned by a moving gap:

```
Physical Slab Array:
[ Group 0 ][ Group 1 ][ Group 2 ][  GAP  ][  GAP  ][  GAP  ][ Group 3 ][ Group 4 ]
^                                ^                          ^
0                         groupsGapStart               groupsGapEnd
```

#### Invariants:
- `maxGroups`: Total physical capacity of the groups buffer.
- `groupsGapStart`: Physical array index marking the start of the unallocated gap.
- `groupsGapEnd`: Physical array index marking the end of the unallocated gap.
- `groupGapLen = groupsGapEnd - groupsGapStart`: Available capacity for new groups.
- `activeGroupCount = maxGroups - groupGapLen`.

#### Logical to Physical Index Translation:
Logical indices $[0, \text{activeGroupCount})$ expose a continuous virtual array to the tree reconciler:

```c
static inline uint32_t CelsGroupLogicalToPhysical(const CelsSession *s, uint32_t logical)
{
    return (logical < s->groupsGapStart)
        ? logical
        : logical + (s->groupsGapEnd - s->groupsGapStart);
}
```
**Complexity**: $\mathcal{O}(1)$ time, branch predictor-friendly (two integer arithmetic operations).

### 4.2 Gap Movement Algorithm

When the traversal cursor or reconciler requires inserting or compacting nodes at `targetLogical`, the gap is shifted using `memmove`:

```c
static void MoveGroupGap(CelsSession *s, uint32_t targetLogical)
{
    if (targetLogical == s->groupsGapStart) {
        return;
    }

    if (targetLogical < s->groupsGapStart) {
        /* Gap moves LEFT: elements in [targetLogical, groupsGapStart) shift RIGHT */
        const uint32_t delta = s->groupsGapStart - targetLogical;
        memmove(&s->groups[s->groupsGapEnd - delta],
                &s->groups[targetLogical],
                delta * sizeof(CelsSlotGroup));
        s->groupsGapStart -= delta;
        s->groupsGapEnd -= delta;
    } else {
        /* Gap moves RIGHT: elements in [groupsGapEnd, groupsGapEnd + delta) shift LEFT */
        const uint32_t delta = targetLogical - s->groupsGapStart;
        memmove(&s->groups[s->groupsGapStart],
                &s->groups[s->groupsGapEnd],
                delta * sizeof(CelsSlotGroup));
        s->groupsGapStart += delta;
        s->groupsGapEnd += delta;
    }
}
```

#### Algorithmic Complexity:
- **Time Complexity**: $\mathcal{O}(k)$, where $k = |\text{groupsGapStart} - \text{targetLogical}|$.
- In steady-state frame evaluation where the tree structure does not mutate, $\text{targetLogical} == \text{groupsGapStart}$, resulting in $\mathcal{O}(1)$ zero-cost passes.
- When insertions happen at the active cursor during depth-first walks, the gap moves strictly in tandem with the cursor, maintaining amortized $\mathcal{O}(1)$ behavior.

### 4.3 Slot Table Primitive Gap Buffer (`CelsSlotTable`)

In the standalone runtime layer (`src/runtime/slot_table.c`), both groups and 64-bit word slots maintain synchronized gap buffers:

```c
typedef struct CelsSlotTable {
    CelsSlotGroup *groups;
    CelsSlotValue *slots;
    uint32_t groupCapacity;
    uint32_t groupGapStart;
    uint32_t groupGapLen;
    uint32_t slotCapacity;
    uint32_t slotGapStart;
    uint32_t slotGapLen;
    uint32_t readerCount;
    bool writerActive;
} CelsSlotTable;
```

When shifting the gap in `CelsSlotTableMoveGapTo`:
1. The group gap shifts by $\Delta_{\text{groups}}$.
2. The slot gap shifts by the exact cumulative `slotCount` of the shifted groups.
3. Every group shifted across the slot gap has its `slotIndex` adjusted by $\pm \text{slotGapLen}$.

```c
/* Adjust slotIndex for shifted groups moving to the right of the slot gap */
for (uint32_t i = 0; i < shiftGroups; i++) {
    dstGroups[i].slotIndex += table->slotGapLen;
}
```

---

## 5. Nonmoving Arena & Pinned Address Guarantees

In user composables, values allocated via `cel_remember(Type, default)` or `cel_lifecycle_state` must have **pinned memory addresses**. If an insertion caused memory pointers to move, any user pointers, callbacks, or hardware buffers retaining references to those structures would instantly become dangling pointers.

CELS solves this through a dedicated, nonmoving data arena partition.

### 5.1 The `CelsSlotAllocation` Record

```c
typedef struct CelsSlotAllocation {
    uint32_t groupId;      /* Owning group identity (matches group->userData) */
    uint32_t slotOffset;   /* Offset relative to the group's remembered slots */
    uint32_t arenaOffset;  /* Absolute byte offset into s->dataArena */
    uint16_t size;         /* Aligned allocation size in data arena (8-byte aligned) */
    uint16_t userSize;     /* Exact requested user type size for schema evolution check */
} CelsSlotAllocation;
```

Each allocation entry maps an abstract `(groupId, slotOffset)` coordinate to an absolute byte offset in `dataArena`.

### 5.2 Allocation & Address Stability Guarantee

```
s->dataArena:
[ Slot A (Group 1) ][ Slot B (Group 2) ][ Slot C (Group 1) ] ... [ Available Space ]
^                                                          ^                       ^
0                                                    dataGapStart             dataGapEnd
```

When `CelsResolveSlot` is invoked:
1. **Fresh Mount (`CELS_FLAG_FRESH_MOUNT`)**:
   - Computes aligned size: $\text{alignedSize} = (\text{size} + 7) \ \& \ \sim 7$.
   - Appends to the end of the active arena region at `s->dataGapStart` (or fills an unfragmented gap).
   - Records the `CelsSlotAllocation` entry.
   - Advances `s->dataGapStart += alignedSize`.
   - Copies `initVal` (if non-NULL) or zeroes memory.
   - Returns pinned pointer: `&s->dataArena[offset]`.

2. **Recomposition Passes**:
   - Searches `s->slots` for a matching `(groupId, currentSlotOffset)` pair.
   - Verifies struct size schema integrity (`slot->userSize == size`).
   - Returns the exact same pointer: `&s->dataArena[slot->arenaOffset]`.

Because `dataArena` byte offsets never shift when groups gap-buffer moves, **pointers returned by `cel_remember` are 100% address-stable** throughout the entire lifecycle of the composable.

---

## 6. Parent Hierarchy & Parent Index Shifting

### 6.1 Logical Parent Chains

Every group maintains `parentIndex`, which points to the logical group index of its enclosing parent node (or `UINT32_MAX` for the root). This creates an upward directed acyclic graph (tree) that allows leaf nodes to propagate signals upwards to the root.

```
[ Root Node: Index 0 ] (parentIndex = UINT32_MAX, groupSize = 4)
  |-- [ Child A: Index 1 ] (parentIndex = 0, groupSize = 2)
  |     |-- [ Grandchild A1: Index 2 ] (parentIndex = 1, groupSize = 0)
  |     `-- [ Grandchild A2: Index 3 ] (parentIndex = 1, groupSize = 0)
  `-- [ Child B: Index 4 ] (parentIndex = 0, groupSize = 0)
```

### 6.2 The Renumbering Hazard & `CelsSlotTableGroupsShiftParents`

Because `parentIndex` stores a **logical** group index, whenever groups are inserted or deleted, the logical indices of all subsequent groups shift by $+1$ or $-\text{removedCount}$.

If parent indices were left unadjusted:
- An ancestor link would point to an arbitrary, unrelated group.
- Upward invalidation walks would climb corrupted chains, failing to dirty the true root.
- The tree reconciler would $O(1)$-skip past invalidated nodes.

To prevent this, structural mutations execute parent index shifting:

```c
void CelsSlotTableGroupsShiftParents(CelsSlotTable *table, uint32_t threshold, int32_t delta)
{
    if (table == NULL || delta == 0) return;

    const uint32_t gapEnd = table->groupGapStart + table->groupGapLen;
    for (uint32_t physical = 0; physical < table->groupCapacity; ++physical) {
        if (physical >= table->groupGapStart && physical < gapEnd) {
            continue; /* Skip elements residing inside the gap */
        }

        CelsSlotGroup *const group = &table->groups[physical];
        if (group->parentIndex == UINT32_MAX || group->parentIndex < threshold) {
            continue;
        }
        group->parentIndex = (uint32_t)((int64_t)group->parentIndex + (int64_t)delta);
    }
}
```

```
Physical Array:
              [ 0 ][ 1 ][ 2 ][ GAP ][ GAP ][ 3 ][ 4 ]
Threshold: 2             ^                  |
Delta: +1                |                  +-- Shift parentIndex by +1
```

By scanning physically across the array and skipping the gap, the shift updates all surviving live parent pointers in a single sequential, vectorizable pass ($\mathcal{O}(N)$ over physical group capacity).

---

## 7. Zero-Heap Allocation Proof

The entire runtime lifecycle of a CELS composition session guarantees zero calls to `malloc`, `realloc`, or `free`.

| Operation | Memory Mechanism | Allocation Count |
|---|---|---|
| Session Creation | Static slab via `CEL_SLAB(name, size)` or single pre-startup `_aligned_malloc` | 0 (static) / 1 (startup) |
| Mounting Composable Node | Carved from `s->groups` gap buffer | 0 |
| Dynamic Memory Allocation (`cel_remember`) | Carved from `s->dataArena` nonmoving arena | 0 |
| Reordering Composable Siblings | Stack buffer (`stackMoved[64]`) or in-place block `memmove` | 0 ($\le 64$ siblings) |
| Tree Pruning & Reclaim | Gap widened; arena head offset decremented | 0 |
| State Double Buffering | Carved from `s->dataArena` at session init | 0 |
| Cross-Thread Transactions | Staged into preallocated `transactionBatches[2]` linear arenas | 0 |

### Verifiable Memory Footprint:
For any embedded or game application, the maximum memory ceiling is bounded at compile-time:

$$\text{Memory Ceiling} = \operatorname{sizeof}(\text{CelsSession}) + \text{SlabCapacity}$$

There are no unbounded pointer graphs, no per-node metadata headers, and no hidden runtime allocator calls.
