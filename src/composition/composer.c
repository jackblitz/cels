#include "composition/composer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flecs.h"

#include "composition/recomposition_dispatcher.h"
#include "composition/state.h"

#define CELS_ASSERT(condition) assert(condition)

#if defined(_MSC_VER)
#define CELS_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define CELS_THREAD_LOCAL __thread
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define CELS_THREAD_LOCAL _Thread_local
#else
#define CELS_THREAD_LOCAL
#endif

static CELS_THREAD_LOCAL CelsComposer *s_currentComposer = NULL;
static CELS_THREAD_LOCAL CelsComposer s_defaultComposer;
static CELS_THREAD_LOCAL CEL_CompositionScope s_lastComposition;

/**
 * Reports whether the composer's active group must run its body whatever the
 * parameter diff says.
 *
 * The answer was latched on entering the group, from that group's
 * CelsGroupFlags plus whether it had just been mounted, because entering also
 * clears those flags. Every gated skip consults this, and every gated skip
 * jumps over the group's ENTIRE subtree — so a false negative here is exactly
 * the silent failure the flags exist to prevent: the state changes and the tree
 * does not.
 *
 * @param cmp Composer to query. NULL, or a composer with no group open, is
 *            treated as "nothing forces a run".
 * @return true if the active group's body must run regardless of diffing.
 */
static bool
ComposerActiveGroupForcesRun(const CelsComposer *cmp)
{
    if (cmp == NULL || cmp->parentStackTop == 0) {
        return false;
    }
    return cmp->forceRunStack[cmp->parentStackTop];
}

/**
 * Latches the invalidation gate for a group the composer has just entered, and
 * clears that group's flags.
 *
 * Clearing on entry is what stops a flag sticking forever: any cel_update
 * raised while the body runs lands back on the host's invalidation queue and is
 * picked up by the next drain iteration, so the flag has served its purpose the
 * moment the walk commits to descending.
 *
 * @param cmp          Target composer, positioned on the entered group.
 *                     Non-NULL.
 * @param composable   Logical group index just entered.
 * @param newlyMounted true when the group was inserted this pass, which forces
 *                     a run because there is no cached state to compare with.
 */
static void
ComposerGateLatch(CelsComposer *cmp,
                  CelsComposableId composable,
                  bool newlyMounted)
{
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(cmp->parentStackTop > 0);

    const uint16_t flags = CelsSlotTableGroupFlags(cmp->table, composable);
    cmp->forceRunStack[cmp->parentStackTop] =
        newlyMounted || (flags != (uint16_t)CELS_GROUP_FLAG_NONE);

    CelsSlotTableGroupClearFlags(cmp->table, composable);
}

/**
 * Renumbers every stored composable id after a structural change.
 *
 * A CelsComposableId is a logical group index, so inserting or removing groups
 * renumbers it — and ids are held in three places, only one of which the slot
 * table owns: group parentIndex links, reactive-cell watcher lists, and the
 * host's invalidation queue. All three must move together. Any one left stale
 * refers to whichever composable now occupies that number, which means the
 * wrong composable is invalidated, or CONTAINS_INVALIDATED lands on the wrong
 * group and the walk O(1)-skips the one that actually changed. Nothing fails
 * visibly at the moment the ids go stale, which is what makes this worth
 * centralising in one place rather than open-coding per call site.
 *
 * @param cmp       Composer performing the change. Non-NULL, with a table.
 * @param threshold Lowest logical index affected by the renumbering.
 * @param delta     Amount to add to each affected id. Zero is a no-op.
 */
static void
ComposerShiftComposableIds(CelsComposer *cmp, uint32_t threshold, int32_t delta)
{
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(cmp->table != NULL);

    if (delta == 0) {
        return;
    }

    CelsSlotTableGroupsShiftParents(cmp->table, threshold, delta);

    // Only meaningful while a host is composing; outside a walk (the composer
    // is also driven directly by tests) there is nothing external holding ids.
    CelsCompositionHost *const host = CelsInvalidationContextGet();
    if (host != NULL) {
        CelsMutableStateShiftComposables(host, threshold, delta);
        CelsCompositionHostShiftInvalidations(host, threshold, delta);
    }
}

/**
 * Prunes trailing unvisited groups and their slots from the gap buffer.
 *
 * Each pruned composable is first unsubscribed from every reactive cell it
 * read, then reported through the transaction context, and only then is its
 * space reclaimed. That order is load-bearing: ids are reused, so a
 * subscription that outlives its composable aliases onto the next occupant of
 * the slot and a later update invalidates the wrong composable.
 *
 * @param cmp        Target composer. Non-NULL.
 * @param startIndex Logical group index where pruning begins.
 * @param count      Number of consecutive groups to remove.
 */
static void
PruneGroups(CelsComposer *cmp, uint32_t startIndex, uint32_t count)
{
    if (count == 0 || cmp == NULL || cmp->table == NULL) {
        return;
    }

    CelsSlotTable *table = cmp->table;
    const CelsResult moveRes = CelsSlotTableMoveGapTo(table, startIndex);
    CELS_ASSERT(moveRes == CELS_OK);

    const uint32_t gapEnd = table->groupGapStart + table->groupGapLen;
    const CelsSlotGroup *staleGroups = &table->groups[gapEnd];
    CelsCompositionHost *const host = CelsInvalidationContextGet();

    uint32_t staleSlots = 0;
    for (uint32_t i = 0; i < count; i++) {
        const CelsComposableId pruned = (CelsComposableId)(startIndex + i);

        CelsMutableStateUnsubscribe(host, pruned);
        CelsTransactionNotifyDestroy(pruned);

        staleSlots += (uint32_t)staleGroups[i].slotCount;
        if (cmp->stage != NULL && staleGroups[i].entityId != 0) {
            ecs_delete(cmp->stage, (ecs_entity_t)staleGroups[i].entityId);
        }
    }

    const uint32_t slotGapEnd = table->slotGapStart + table->slotGapLen;

    memset(&table->groups[gapEnd], 0, count * sizeof(CelsSlotGroup));
    memset(&table->slots[slotGapEnd], 0, staleSlots * sizeof(CelsSlotValue));

    table->groupGapLen += count;
    table->slotGapLen += staleSlots;

    // Removing count groups renumbers everything above them downward. Done
    // after the gap widened, so the removed groups are inside it and excluded,
    // and after the unsubscribe loop above, so the dying composables' own
    // subscriptions are already gone rather than being renumbered onto a
    // survivor.
    ComposerShiftComposableIds(cmp, startIndex + count, -(int32_t)count);
}

/**
 * Initializes the composer for an execution pass over the table.
 *
 * @param cmp   Target composer. Non-NULL.
 * @param table Target slot table. Non-NULL.
 */
void
CelsComposerBegin(CelsComposer *cmp, CelsSlotTable *table)
{
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(table != NULL);

    cmp->table = table;
    cmp->readerIndex = 0;
    cmp->currentSlot = 0;
    cmp->parentStackTop = 0;
    cmp->skipCount = 0;
    cmp->parentStack[0] = UINT32_MAX;
    cmp->groupEndStack[0] = CelsSlotTableGroupCount(table);
    cmp->entityStack[0] = 0;
    cmp->forceRunStack[0] = false;
}

/**
 * Enters an existing group matching id, or inserts a new group into the gap.
 * Automatically creates and parents a Flecs entity if a stage is bound.
 *
 * Both paths latch the invalidation gate for the entered group (see
 * ComposerGateLatch). The cache-miss path additionally fires onCreate before
 * returning, so a freshly mounted composable's body can rely on its own
 * onCreate having already run this same pass.
 *
 * @param cmp Target composer. Non-NULL.
 * @param id  Stable callsite ID.
 * @return true if entered successfully, false on capacity exhaustion.
 */
bool
CelsComposerGroupStartId(CelsComposer *cmp, CelsId id)
{
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(cmp->table != NULL);

    if (cmp->parentStackTop + 1u >= CELS_COMPOSER_MAX_DEPTH) {
        return false;
    }

    CelsSlotTable *table = cmp->table;
    const uint32_t key = id.id;

    // 1. Cache Hit: Group at readerIndex matches key.
    const uint32_t currentBound = cmp->groupEndStack[cmp->parentStackTop];
    if (cmp->readerIndex < currentBound) {
        uint32_t phys = 0;
        const CelsResult res =
            CelsSlotTableGroupToPhysicalIdx(table, cmp->readerIndex, &phys);
        if (res == CELS_OK && table->groups[phys].key == key) {
            CelsSlotGroup *group = &table->groups[phys];
            const CelsComposableId entered =
                (CelsComposableId)cmp->readerIndex;
            cmp->parentStackTop++;
            cmp->parentStack[cmp->parentStackTop] = cmp->readerIndex;
            cmp->groupEndStack[cmp->parentStackTop] =
                cmp->readerIndex + 1u + (uint32_t)group->groupSize;
            cmp->currentSlot = 0;
            cmp->readerIndex++;

            ComposerGateLatch(cmp, entered, false);

            if (group->entityId != 0) {
                cmp->entityStack[cmp->parentStackTop] = group->entityId;
            } else if (cmp->stage != NULL && id.stringId.chars != NULL) {
                ecs_entity_desc_t desc = { 0 };
                desc.name = id.stringId.chars;
                const ecs_entity_t parent =
                    (ecs_entity_t)cmp->entityStack[cmp->parentStackTop - 1];
                if (parent != 0) {
                    desc.parent = parent;
                }
                const ecs_entity_t e = ecs_entity_init(cmp->stage, &desc);
                group->entityId = (uint64_t)e;
                cmp->entityStack[cmp->parentStackTop] = (uint64_t)e;
            } else {
                cmp->entityStack[cmp->parentStackTop] = 0;
            }

            return true;
        }
    }

    // 2. Cache Miss: Structural divergence (new group insertion)
    if (table->groupGapLen == 0) {
        return false;
    }

    const CelsResult moveRes = CelsSlotTableMoveGapTo(table, cmp->readerIndex);
    CELS_ASSERT(moveRes == CELS_OK);

    const uint32_t newLogicalIndex = table->groupGapStart;
    const uint32_t parentIndex = (cmp->parentStackTop == 0)
        ? UINT32_MAX
        : cmp->parentStack[cmp->parentStackTop];

    uint64_t newEntityId = 0;
    if (cmp->stage != NULL && id.stringId.chars != NULL) {
        ecs_entity_desc_t desc = { 0 };
        desc.name = id.stringId.chars;
        const ecs_entity_t parent =
            (ecs_entity_t)cmp->entityStack[cmp->parentStackTop];
        if (parent != 0) {
            desc.parent = parent;
        }
        const ecs_entity_t e = ecs_entity_init(cmp->stage, &desc);
        newEntityId = (uint64_t)e;
    }

    const CelsSlotGroup newGroup = {
        .entityId = newEntityId,
        .key = key,
        .parentIndex = parentIndex,
        .slotIndex = table->slotGapStart,
        .aux = 0,
        .slotCount = 0,
        .groupSize = 0,
        .nodeCount = 0,
        .flags = 0
    };

    // Inserting here renumbers every logical index from newLogicalIndex up, so
    // every id naming one of them has to move with it. Done before the gap
    // advances, while the old numbering is still the live one; the slot about
    // to hold newGroup is inside the gap and so is skipped.
    ComposerShiftComposableIds(cmp, newLogicalIndex, 1);

    table->groups[table->groupGapStart] = newGroup;
    table->groupGapStart++;
    table->groupGapLen--;

    for (uint32_t i = 0; i <= cmp->parentStackTop; i++) {
        cmp->groupEndStack[i]++;
    }

    cmp->parentStackTop++;
    cmp->parentStack[cmp->parentStackTop] = newLogicalIndex;
    cmp->groupEndStack[cmp->parentStackTop] = newLogicalIndex + 1u;
    cmp->entityStack[cmp->parentStackTop] = newEntityId;
    cmp->currentSlot = 0;
    cmp->readerIndex++;

    ComposerGateLatch(cmp, (CelsComposableId)newLogicalIndex, true);

    // Fired here, with the composer already positioned on the new group and
    // before the caller runs the body, because a body is allowed to read
    // whatever its own onCreate set up — CEL_Composable() must already resolve.
    const CelsComposableId parentComposable = (parentIndex == UINT32_MAX)
        ? CELS_COMPOSABLE_ID_INVALID
        : (CelsComposableId)parentIndex;
    CelsTransactionNotifyCreate((CelsComposableId)newLogicalIndex,
                                parentComposable, key);

    return true;
}

bool
CelsComposerGroupStart(CelsComposer *cmp, uint32_t key)
{
    return CelsComposerGroupStartId(cmp, CelsIdFromKey(key));
}

/**
 * Leaves the current group, updating subtree span and pruning vanished nodes.
 *
 * @param cmp Target composer. Non-NULL.
 */
void
CelsComposerGroupEnd(CelsComposer *cmp)
{
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(cmp->table != NULL);
    CELS_ASSERT(cmp->parentStackTop > 0);

    const uint32_t activeGroup = cmp->parentStack[cmp->parentStackTop];
    const uint32_t expectedEnd = cmp->groupEndStack[cmp->parentStackTop];

    // 1. Prune vanished child groups that were not visited in this pass
    if (cmp->readerIndex < expectedEnd) {
        const uint32_t staleCount = expectedEnd - cmp->readerIndex;
        PruneGroups(cmp, cmp->readerIndex, staleCount);

        // Mirror of the increment in CelsComposerGroupStart: every ancestor
        // still open OUTSIDE the group being closed just lost staleCount
        // descendants, so their expected subtree ends must shrink too, or a
        // later sibling lookup would use a stale, too-large bound and risk
        // re-adopting an unrelated group (the original bug this guards).
        for (uint32_t i = 0; i < cmp->parentStackTop; i++) {
            cmp->groupEndStack[i] -= staleCount;
        }
    }

    // 2. Update groupSize for the completed activeGroup
    uint32_t phys = 0;
    const CelsResult res =
        CelsSlotTableGroupToPhysicalIdx(cmp->table, activeGroup, &phys);
    if (res == CELS_OK) {
        const uint16_t newGroupSize =
            (uint16_t)(cmp->readerIndex - activeGroup - 1u);
        cmp->table->groups[phys].groupSize = newGroupSize;
    }

    // 3. Pop parent stack
    cmp->parentStackTop--;
    cmp->currentSlot = 0;
}

/**
 * Skips the active group's nested subtree in O(1) time.
 *
 * @param cmp Target composer. Non-NULL.
 */
void
CelsComposerGroupSkip(CelsComposer *cmp)
{
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(cmp->parentStackTop > 0);

    cmp->skipCount++;
    cmp->readerIndex = cmp->groupEndStack[cmp->parentStackTop];
}

/**
 * Diffs input data against cached state for the active group.
 * Overwrites cached slots in-place if changed.
 *
 * @param cmp  Target composer. Non-NULL.
 * @param data Pointer to input data. Non-NULL.
 * @param size Byte size of input data.
 * @return true if data changed or newly inserted, false if identical.
 */
bool
CelsComposerChanged(CelsComposer *cmp, const void *data, size_t size)
{
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(data != NULL);
    CELS_ASSERT(size > 0);
    CELS_ASSERT(cmp->parentStackTop > 0);

    CelsSlotTable *table = cmp->table;
    const uint32_t activeGroup = cmp->parentStack[cmp->parentStackTop];

    uint32_t phys = 0;
    const CelsResult res =
        CelsSlotTableGroupToPhysicalIdx(table, activeGroup, &phys);
    if (res != CELS_OK) {
        return true;
    }

    CelsSlotGroup *group = &table->groups[phys];
    const uint32_t neededWords =
        (uint32_t)((size + sizeof(CelsSlotValue) - 1u) / sizeof(CelsSlotValue));
    const uint32_t slotOffsetInGroup = cmp->currentSlot;

    // Check if cached slots already exist for this parameter in this group
    if (slotOffsetInGroup + neededWords <= group->slotCount) {
        const uint32_t physicalSlot = group->slotIndex + slotOffsetInGroup;
        const void *cachedData = &table->slots[physicalSlot];

        bool match = false;
        if (size == sizeof(uint64_t)) {
            match = (*(const uint64_t *)cachedData == *(const uint64_t *)data);
        } else if (size == sizeof(uint32_t)) {
            match = (*(const uint32_t *)cachedData == *(const uint32_t *)data);
        } else {
            match = (memcmp(cachedData, data, size) == 0);
        }

        if (match) {
            cmp->currentSlot += neededWords;
            return false;
        }

        // Mutated: update cached slot in place
        memcpy(&table->slots[physicalSlot], data, size);
        cmp->currentSlot += neededWords;
        return true;
    }



    // First time encountering this slot parameter: allocate into slot gap
    if (table->slotGapLen < neededWords) {
        return true;
    }

    const CelsResult moveRes = CelsSlotTableMoveGapTo(table, activeGroup + 1u);
    CELS_ASSERT(moveRes == CELS_OK);

    const uint32_t slotIndex = table->slotGapStart;
    memcpy(&table->slots[slotIndex], data, size);

    table->slotGapStart += neededWords;
    table->slotGapLen -= neededWords;

    group->slotCount += (uint16_t)neededWords;
    cmp->currentSlot += neededWords;

    return true;
}

/**
 * Returns the number of O(1) subtree skips performed by this composer.
 *
 * @param cmp Pointer to composer. Non-NULL.
 * @return Total skip count.
 */
uint32_t
CelsComposerSkipCount(const CelsComposer *cmp)
{
    CELS_ASSERT(cmp != NULL);
    return cmp->skipCount;
}

/**
 * Returns the active ambient composer for the calling thread.
 *
 * @return Pointer to current composer, or NULL if outside a composition pass.
 */
CelsComposer *
CelsComposerGetCurrent(void)
{
    return s_currentComposer;
}

/**
 * Sets the active ambient composer for the calling thread.
 *
 * @param cmp Pointer to the new active composer, or NULL.
 */
void
CelsComposerSetCurrent(CelsComposer *cmp)
{
    s_currentComposer = cmp;
}

/**
 * Returns a thread-local static composer instance for default passes.
 *
 * @return Pointer to default thread-local composer. Never NULL.
 */
CelsComposer *
CelsComposerGetDefault(void)
{
    return &s_defaultComposer;
}

/**
 * Returns the Flecs stage associated with the composer, if any.
 *
 * @param cmp Pointer to composer. If NULL, queries current ambient composer.
 * @return Flecs stage handle (ecs_world_t*), or NULL if no stage is bound.
 */
struct ecs_world_t *
CelsComposerGetStage(const CelsComposer *cmp)
{
    if (cmp == NULL) {
        cmp = s_currentComposer;
    }
    return cmp != NULL ? cmp->stage : NULL;
}

/**
 * Returns the Flecs stage associated with the current ambient composer.
 *
 * @return Flecs stage handle (struct ecs_world_t*), or NULL if outside pass or no stage.
 */
struct ecs_world_t *
CelsComposerGetCurrentStage(void)
{
    return s_currentComposer != NULL ? s_currentComposer->stage : NULL;
}

uint64_t
CelsComposerGetEntity(const CelsComposer *cmp)
{
    if (cmp == NULL) {
        cmp = s_currentComposer;
    }
    if (cmp == NULL || cmp->parentStackTop == 0) {
        return 0;
    }
    return cmp->entityStack[cmp->parentStackTop];
}

uint64_t
CelsComposerGetCurrentEntity(void)
{
    return CelsComposerGetEntity(NULL);
}

CEL_CompositionScope
CelsComposerGetLastCompositionScope(void)
{
    return s_lastComposition;
}

CEL_CompositionScope
CelsComposerGetLastComposition(void)
{
    return s_lastComposition;
}

/* ========================================================================= */
/* Table Registry & Lifecycle (Automatic Table Assignment)                   */
/* ========================================================================= */

#define CELS_MAX_REGISTERED_TABLES 128
#define CELS_DEFAULT_TABLE_SLAB_SIZE 16384

typedef struct CelsTableRegistryEntry {
    uint32_t key;
    CelsSlotTable table;
    void *slab;
    size_t slabSize;
    uint32_t lastVisitedPass;
    bool inUse;
} CelsTableRegistryEntry;

static CelsTableRegistryEntry s_tableRegistry[CELS_MAX_REGISTERED_TABLES];

static void *
CelsAlignedAlloc(size_t size, size_t alignment)
{
    void *raw = malloc(size + alignment + sizeof(void *));
    if (raw == NULL) {
        return NULL;
    }
    uintptr_t rawAddr = (uintptr_t)raw + sizeof(void *);
    uintptr_t alignedAddr = (rawAddr + alignment - 1u) & ~(alignment - 1u);
    void **header = (void **)(alignedAddr - sizeof(void *));
    *header = raw;
    return (void *)alignedAddr;
}

static void
CelsAlignedFree(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    void **header = (void **)((uintptr_t)ptr - sizeof(void *));
    free(*header);
}

CelsSlotTable *
CelsTableRegistryAcquire(uint32_t key)
{
    // 1. Check if table already registered for key (recomposition across frames)
    for (size_t i = 0; i < CELS_MAX_REGISTERED_TABLES; i++) {
        if (s_tableRegistry[i].inUse && s_tableRegistry[i].key == key) {
            return &s_tableRegistry[i].table;
        }
    }

    // 2. Allocate new persistent table entry for this key
    for (size_t i = 0; i < CELS_MAX_REGISTERED_TABLES; i++) {
        if (!s_tableRegistry[i].inUse) {
            void *slab = CelsAlignedAlloc(CELS_DEFAULT_TABLE_SLAB_SIZE, 64);
            if (slab == NULL) {
                return NULL;
            }

            const CelsResult res = CelsSlotTableInit(
                &s_tableRegistry[i].table, slab, CELS_DEFAULT_TABLE_SLAB_SIZE, 32);
            if (res != CELS_OK) {
                CelsAlignedFree(slab);
                return NULL;
            }

            s_tableRegistry[i].key = key;
            s_tableRegistry[i].slab = slab;
            s_tableRegistry[i].slabSize = CELS_DEFAULT_TABLE_SLAB_SIZE;
            s_tableRegistry[i].inUse = true;
            return &s_tableRegistry[i].table;
        }
    }

    return NULL;
}

void
CelsTableRegistryRelease(uint32_t key)
{
    for (size_t i = 0; i < CELS_MAX_REGISTERED_TABLES; i++) {
        if (s_tableRegistry[i].inUse && s_tableRegistry[i].key == key) {
            CelsSlotTableReset(&s_tableRegistry[i].table);
            CelsAlignedFree(s_tableRegistry[i].slab);
            memset(&s_tableRegistry[i], 0, sizeof(s_tableRegistry[i]));
            return;
        }
    }
}

void
CelsTableRegistryReset(void)
{
    for (size_t i = 0; i < CELS_MAX_REGISTERED_TABLES; i++) {
        if (s_tableRegistry[i].inUse) {
            CelsSlotTableReset(&s_tableRegistry[i].table);
            CelsAlignedFree(s_tableRegistry[i].slab);
            memset(&s_tableRegistry[i], 0, sizeof(s_tableRegistry[i]));
        }
    }
}

CelsSlotGroup *
CelsTableRegistryFindGroup(uint32_t key, CelsSlotTable **outTable)
{
    for (size_t i = 0; i < CELS_MAX_REGISTERED_TABLES; i++) {
        if (s_tableRegistry[i].inUse) {
            CelsSlotGroup *grp =
                CelsSlotTableFindGroup(&s_tableRegistry[i].table, key, NULL);
            if (grp != NULL) {
                if (outTable != NULL) {
                    *outTable = &s_tableRegistry[i].table;
                }
                return grp;
            }
        }
    }
    return NULL;
}

/**
 * Initializes ambient context and begins root composition for CEL_CompositionScope.
 * If table is NULL, automatically assigns and acquires a persistent table for id.
 *
 * @param cmp   Optional explicit composer (NULL uses thread-local default).
 * @param table Optional target slot table (NULL automatically acquires from registry).
 * @param id    Root callsite ID.
 * @return Initialized scope tracking ambient restoration and group status.
 */
CelsComposerScope
CelsComposerScopeEnter(CelsComposer *cmp, CelsSlotTable *table, CelsId id)
{
    CelsComposerScope scope;
    scope.prev = s_currentComposer;
    scope.curr = (cmp != NULL)
        ? cmp
        : (s_currentComposer != NULL ? s_currentComposer : &s_defaultComposer);

    s_currentComposer = scope.curr;

    if (table == NULL) {
        if (scope.curr != &s_defaultComposer && scope.curr->table != NULL) {
            table = scope.curr->table;
        } else {
            table = CelsTableRegistryAcquire(id.id);
        }
    }
    CELS_ASSERT(table != NULL);

    struct ecs_world_t *const savedStage = scope.curr->stage;
    CelsComposerBegin(scope.curr, table);
    scope.curr->stage = savedStage;

    if (!CelsComposerGroupStartId(scope.curr, id)) {
        s_currentComposer = scope.prev;
        scope.curr = NULL;
        scope.active = 0;
        return scope;
    }

    scope.active = 1;

    s_lastComposition.id = id;
    s_lastComposition.entity = scope.curr->entityStack[scope.curr->parentStackTop];
    s_lastComposition.groupCount = CelsSlotTableGroupCount(table);

    return scope;
}

CelsComposerScope
CelsComposerScopeEnterKey(CelsComposer *cmp, CelsSlotTable *table, uint32_t key)
{
    return CelsComposerScopeEnter(cmp, table, CelsIdFromKey(key));
}

/**
 * Concludes root composition pass and restores previous ambient context.
 *
 * @param scope Pointer to active composition scope. Non-NULL.
 */
void
CelsComposerScopeExit(CelsComposerScope *scope)
{
    CELS_ASSERT(scope != NULL);

    if (scope->curr != NULL && scope->active != 0) {
        if (scope->curr->table != NULL) {
            s_lastComposition.groupCount =
                CelsSlotTableGroupCount(scope->curr->table);
        }
        CelsComposerGroupEnd(scope->curr);
        if (scope->curr == &s_defaultComposer) {
            scope->curr->table = NULL;
        }
    }
    s_currentComposer = scope->prev;
    scope->active = 0;
}

/**
 * Enters child group in the current ambient composer for CEL_Compose.
 *
 * @param id Stable callsite ID.
 * @return 1 on success, 0 on failure or missing ambient context.
 */
int32_t
CelsComposerGroupEnter(CelsId id)
{
    CelsComposer *cmp = s_currentComposer;
    if (cmp == NULL) {
        return 0;
    }
    return CelsComposerGroupStartId(cmp, id) ? 1 : 0;
}

int32_t
CelsComposerGroupEnterKey(uint32_t key)
{
    return CelsComposerGroupEnter(CelsIdFromKey(key));
}

/**
 * Leaves child group in the current ambient composer for CEL_Compose.
 */
void
CelsComposerGroupLeave(void)
{
    CelsComposer *cmp = s_currentComposer;
    if (cmp != NULL) {
        CelsComposerGroupEnd(cmp);
    }
}

/**
 * Enters child group with explicit composer for CEL_Compose.
 *
 * @param cmp Target composer. Non-NULL.
 * @param id  Stable callsite ID.
 * @return 1 on success, 0 on failure.
 */
int32_t
CelsComposerGroupEnterExplicit(CelsComposer *cmp, CelsId id)
{
    if (cmp == NULL) {
        return 0;
    }
    return CelsComposerGroupStartId(cmp, id) ? 1 : 0;
}

/**
 * Leaves child group with explicit composer for CEL_Compose.
 *
 * @param cmp Target composer. Non-NULL.
 */
void
CelsComposerGroupLeaveExplicit(CelsComposer *cmp)
{
    if (cmp != NULL) {
        CelsComposerGroupEnd(cmp);
    }
}

/**
 * Diffs data against active group; skips subtree in O(1) if unchanged.
 *
 * The diff is only one input to the gate. Skipping here jumps the cursor past
 * the whole subtree, so an invalidated descendant would never be reached; the
 * active group's latched gate vetoes the skip in exactly that case.
 *
 * @param cmp  Optional composer (NULL uses ambient composer).
 * @param data Pointer to input data. Non-NULL.
 * @param size Byte size of input data.
 * @return true if changed or the gate forces a run, false if skipped.
 */
bool
CelsComposerWatchEnter(CelsComposer *cmp, const void *data, size_t size)
{
    if (cmp == NULL) {
        cmp = s_currentComposer;
    }
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(cmp->parentStackTop > 0);

    const bool changed = CelsComposerChanged(cmp, data, size);
    if (!changed && !ComposerActiveGroupForcesRun(cmp)) {
        CelsComposerGroupSkip(cmp);
        return false;
    }

    return true;
}

/**
 * Diffs an ECS query against active group's slot memory; skips subtree in O(1) if unchanged.
 *
 * @param cmp       Optional composer (NULL uses ambient composer).
 * @param queryData Pointer to query descriptor, handle, or tick struct. Non-NULL.
 * @param querySize Size of query descriptor in bytes.
 * @return true if query results changed (enter block), false if unchanged (skip in O(1)).
 */
bool
CelsComposerQueryEnter(CelsComposer *cmp,
                       const void *queryData,
                       size_t querySize)
{
    if (cmp == NULL) {
        cmp = s_currentComposer;
    }
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(cmp->parentStackTop > 0);
    CELS_ASSERT(queryData != NULL);

    /*
     * Flecs Integration Notes:
     * - When queryData represents an ecs_query_t* or query wrapper, we check:
     *     uint64_t currentTick = ecs_query_changed(query) ? query->match_tick : cachedTick;
     * - In the slot table, we diff currentTick against cachedSlot.
     * - If unchanged: CelsComposerGroupSkip(cmp) skips the entire query in O(1).
     * - If changed: CelsComposerChanged updates the slot and returns true.
     */
    const bool changed = CelsComposerChanged(cmp, queryData, querySize);
    if (!changed && !ComposerActiveGroupForcesRun(cmp)) {
        CelsComposerGroupSkip(cmp);
        return false;
    }

    return true;
}

/**
 * Diffs an observable state or Flecs query against the slot table cache.
 * If unchanged: skips the active group in O(1) time and returns false.
 * If changed: copies the BEFORE-recomposition data from the slot table into
 * outPrevious (if non-NULL), updates the slot table cache in-place with the new
 * data, and returns true.
 *
 * @param cmp         Optional composer (NULL uses ambient composer).
 * @param data        Pointer to current data snapshot. Non-NULL.
 * @param size        Byte size of data.
 * @param outPrevious Optional destination to receive the data from BEFORE recomposition.
 * @return true if data changed (enter block), false if unchanged (skipped in O(1)).
 */
bool
CelsComposerObservableEnter(CelsComposer *cmp,
                            const void *data,
                            size_t size,
                            void *outPrevious)
{
    if (cmp == NULL) {
        cmp = s_currentComposer;
    }
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(cmp->parentStackTop > 0);
    CELS_ASSERT(data != NULL);
    CELS_ASSERT(size > 0);

    CelsSlotTable *table = cmp->table;
    const uint32_t activeGroup = cmp->parentStack[cmp->parentStackTop];

    uint32_t phys = 0;
    const CelsResult res =
        CelsSlotTableGroupToPhysicalIdx(table, activeGroup, &phys);
    if (res != CELS_OK) {
        return true;
    }

    CelsSlotGroup *group = &table->groups[phys];
    const uint32_t neededWords =
        (uint32_t)((size + sizeof(CelsSlotValue) - 1u) / sizeof(CelsSlotValue));
    const uint32_t slotOffsetInGroup = cmp->currentSlot;

    // Case 1: Group already has cached slots from previous pass
    if (slotOffsetInGroup + neededWords <= group->slotCount) {
        const uint32_t physicalSlot = group->slotIndex + slotOffsetInGroup;
        const void *cachedData = &table->slots[physicalSlot];

        bool match = false;
        if (size == sizeof(uint64_t)) {
            match = (*(const uint64_t *)cachedData == *(const uint64_t *)data);
        } else if (size == sizeof(uint32_t)) {
            match = (*(const uint32_t *)cachedData == *(const uint32_t *)data);
        } else {
            match = (memcmp(cachedData, data, size) == 0);
        }

        if (match) {
            cmp->currentSlot += neededWords;

            if (!ComposerActiveGroupForcesRun(cmp)) {
                // Unchanged and nothing below is invalidated: skip in O(1).
                CelsComposerGroupSkip(cmp);
                return false;
            }

            // Forced to run so the walk can reach an invalidated descendant.
            // Nothing about the observable itself moved, so "before" and "now"
            // are the same value.
            if (outPrevious != NULL) {
                memcpy(outPrevious, cachedData, size);
            }
            return true;
        }

        // Mutated: copy BEFORE-recomposition data to outPrevious
        if (outPrevious != NULL) {
            memcpy(outPrevious, cachedData, size);
        }

        // Update cached slot in place with new data
        memcpy(&table->slots[physicalSlot], data, size);
        cmp->currentSlot += neededWords;
        return true;
    }

    // Case 2: Initial mount - allocate into slot gap and write data
    if (outPrevious != NULL) {
        memset(outPrevious, 0, size);
    }

    const CelsResult moveRes = CelsSlotTableMoveGapTo(table, activeGroup + 1u);
    CELS_ASSERT(moveRes == CELS_OK);
    const uint32_t slotIndex = table->slotGapStart;
    memcpy(&table->slots[slotIndex], data, size);

    table->slotGapStart += neededWords;
    table->slotGapLen -= neededWords;

    group->slotCount += (uint16_t)neededWords;
    cmp->currentSlot += neededWords;
    return true;
}

/**
 * Enters child group and diffs input state in the current ambient composer.
 * If identical, skips subtree in O(1) and leaves group immediately.
 *
 * This is the Style B gate in full: run the body if the group is INVALIDATED,
 * or CONTAINS_INVALIDATED, or its parameters changed, or it was just mounted;
 * otherwise skip the subtree. Two outcomes, not three — descending without
 * running is impossible when the child call sites are the parent's body.
 *
 * @param id        Stable callsite ID.
 * @param stateData Pointer to state struct/variable to diff. Non-NULL.
 * @param stateSize Size of state struct/variable in bytes.
 * @return 1 if changed, invalidated or first mount (enter block), 0 if skipped
 *         or failed.
 */
int32_t
CelsComposerGroupEnterStateful(CelsId id,
                               const void *stateData,
                               size_t stateSize)
{
    CelsComposer *cmp = s_currentComposer;
    if (cmp == NULL) {
        return 0;
    }
    if (!CelsComposerGroupStartId(cmp, id)) {
        return 0;
    }
    const bool changed = CelsComposerChanged(cmp, stateData, stateSize);
    if (!changed && !ComposerActiveGroupForcesRun(cmp)) {
        CelsComposerGroupSkip(cmp);
        CelsComposerGroupEnd(cmp);
        return 0;
    }
    return 1;
}

/**
 * Explicit composer variant of CelsComposerGroupEnterStateful.
 *
 * @param cmp       Target composer. Non-NULL.
 * @param id        Stable callsite ID.
 * @param stateData Pointer to state struct/variable to diff. Non-NULL.
 * @param stateSize Size of state struct/variable in bytes.
 * @return 1 if changed, invalidated or first mount (enter block), 0 if skipped
 *         or failed.
 */
int32_t
CelsComposerGroupEnterStatefulExplicit(CelsComposer *cmp,
                                       CelsId id,
                                       const void *stateData,
                                       size_t stateSize)
{
    if (cmp == NULL) {
        return 0;
    }
    if (!CelsComposerGroupStartId(cmp, id)) {
        return 0;
    }
    const bool changed = CelsComposerChanged(cmp, stateData, stateSize);
    if (!changed && !ComposerActiveGroupForcesRun(cmp)) {
        CelsComposerGroupSkip(cmp);
        CelsComposerGroupEnd(cmp);
        return 0;
    }
    return 1;
}

/**
 * Memoizes ephemeral UI state in the active group's slot table storage.
 * On initial mount, allocates slots into the slot gap and seeds with initialData.
 * On subsequent recompositions, returns a pointer to the existing slot without
 * overwriting changes.
 *
 * Flecs Architecture Notes:
 * - Ephemeral UI state (e.g. isHovered, scrollOffset) is retained in the slot table
 *   without polluting the Flecs world with throwaway entities.
 * - When bridging to Flecs-backed entities, CelsSlotGroup.entityId stores the
 *   Flecs ecs_entity_t handle. When the group vanishes from the slot table,
 *   CelsComposerGroupEnd can automatically trigger ecs_delete on that handle.
 *
 * @param cmp         Optional composer (NULL uses ambient composer).
 * @param initialData Pointer to initial seed data, or NULL for zero-init.
 * @param size        Size of data in bytes.
 * @return Pointer to the persistent slot memory, or NULL on capacity error.
 */
void *
CelsComposerRemember(CelsComposer *cmp, const void *initialData, size_t size)
{
    if (cmp == NULL) {
        cmp = s_currentComposer;
    }
    CELS_ASSERT(cmp != NULL);
    CELS_ASSERT(cmp->parentStackTop > 0);

    CelsSlotTable *table = cmp->table;
    const uint32_t activeGroup = cmp->parentStack[cmp->parentStackTop];

    uint32_t phys = 0;
    const CelsResult res =
        CelsSlotTableGroupToPhysicalIdx(table, activeGroup, &phys);
    if (res != CELS_OK) {
        return NULL;
    }

    CelsSlotGroup *group = &table->groups[phys];
    const uint32_t neededWords =
        (uint32_t)((size + sizeof(CelsSlotValue) - 1u) / sizeof(CelsSlotValue));
    const uint32_t slotOffsetInGroup = cmp->currentSlot;

    // Case 1: Slot already exists in this group (subsequent passes)
    if (slotOffsetInGroup + neededWords <= group->slotCount) {
        const uint32_t physicalSlot = group->slotIndex + slotOffsetInGroup;
        cmp->currentSlot += neededWords;
        return &table->slots[physicalSlot];
    }

    // Case 2: Initial mount - allocate into slot gap and write initialData
    if (table->slotGapLen < neededWords) {
        return NULL;
    }

    const CelsResult moveRes = CelsSlotTableMoveGapTo(table, activeGroup + 1u);
    CELS_ASSERT(moveRes == CELS_OK);

    const uint32_t slotIndex = table->slotGapStart;
    if (initialData != NULL) {
        memcpy(&table->slots[slotIndex], initialData, size);
    } else {
        memset(&table->slots[slotIndex], 0, neededWords * sizeof(CelsSlotValue));
    }

    table->slotGapStart += neededWords;
    table->slotGapLen -= neededWords;

    group->slotCount += (uint16_t)neededWords;
    cmp->currentSlot += neededWords;

    return &table->slots[slotIndex];
}

CelsComposableId
CelsComposerGetCurrentComposable(const CelsComposer *cmp)
{
    if (cmp == NULL) {
        cmp = s_currentComposer;
    }
    if (cmp == NULL || cmp->table == NULL || cmp->parentStackTop == 0) {
        return CELS_COMPOSABLE_ID_INVALID;
    }
    return (CelsComposableId)cmp->parentStack[cmp->parentStackTop];
}

uint32_t
CelsComposerGetCurrentKey(const CelsComposer *cmp)
{
    if (cmp == NULL) {
        cmp = s_currentComposer;
    }
    if (cmp == NULL || cmp->table == NULL || cmp->parentStackTop == 0) {
        return 0;
    }
    const uint32_t activeGroup = cmp->parentStack[cmp->parentStackTop];
    uint32_t phys = 0;
    if (CelsSlotTableGroupToPhysicalIdx(cmp->table, activeGroup, &phys) == CELS_OK) {
        return cmp->table->groups[phys].key;
    }
    return 0;
}

CelsCompositionRef
CelsComposerFind(const CelsComposer *cmp, uint32_t key)
{
    CelsCompositionRef ref;
    memset(&ref, 0, sizeof(ref));

    if (cmp == NULL) {
        cmp = s_currentComposer;
    }

    CelsSlotTable *targetTable = NULL;
    uint32_t logIdx = 0;
    CelsSlotGroup *grp = NULL;

    if (cmp != NULL && cmp->table != NULL) {
        grp = CelsSlotTableFindGroup(cmp->table, key, &logIdx);
        if (grp != NULL) {
            targetTable = cmp->table;
        }
    } else {
        grp = CelsTableRegistryFindGroup(key, &targetTable);
        if (grp != NULL && targetTable != NULL) {
            const uint32_t count = CelsSlotTableGroupCount(targetTable);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t p = 0;
                if (CelsSlotTableGroupToPhysicalIdx(targetTable, i, &p) == CELS_OK &&
                    &targetTable->groups[p] == grp) {
                    logIdx = i;
                    break;
                }
            }
        }
    }

    if (grp != NULL && targetTable != NULL) {
        ref.found = true;
        ref.key = grp->key;
        ref.logicalIndex = logIdx;
        ref.slotCount = grp->slotCount;
        ref.groupSize = grp->groupSize;
        ref.nodeCount = grp->nodeCount;
        ref.group = grp;
        ref.slots = (grp->slotCount > 0 && grp->slotIndex < targetTable->slotCapacity)
            ? &targetTable->slots[grp->slotIndex]
            : NULL;
    }

    return ref;
}

CelsSlotGroup *
CelsComposerFindGroup(const CelsComposer *cmp, uint32_t key, uint32_t *outLogicalIdx)
{
    CelsCompositionRef ref = CelsComposerFind(cmp, key);
    if (!ref.found) {
        return NULL;
    }
    if (outLogicalIdx != NULL) {
        *outLogicalIdx = ref.logicalIndex;
    }
    return ref.group;
}


