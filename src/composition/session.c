#include "composition/session.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "composition/state.h"

#define CELS_ASSERT(condition) assert(condition)

/* ========================================================================= */
/* Composition Host API                                                      */
/* ========================================================================= */
/* ========================================================================= */
/* Composition Host API                                                      */
/* ========================================================================= */

CelsResult
CelsCompositionHostInit(CelsCompositionHost *host,
                        void *slabMemory,
                        size_t slabSize,
                        uint32_t maxGroups,
                        CelsComposableFn rootFn)
{
    if (host == NULL || slabMemory == NULL || slabSize < 4096 ||
        rootFn == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    memset(host, 0, sizeof(*host));
    host->slabMemory = (uint8_t *)slabMemory;
    host->rootFn = rootFn;
    host->isDirty = true;

    const CelsResult res = CelsSlotTableInit(&host->slotTable,
                                            slabMemory,
                                            slabSize,
                                            maxGroups);
    if (res != CELS_OK) {
        return res;
    }

    return CELS_OK;
}

void
CelsCompositionHostSetDirty(CelsCompositionHost *host, bool dirty)
{
    if (host != NULL) {
        host->isDirty = dirty;
    }
}

bool
CelsCompositionHostIsDirty(const CelsCompositionHost *host)
{
    return host != NULL && host->isDirty;
}
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

static void
CelsSessionRootTrampoline(CelsComposer *cmp, void *props)
{
    (void)cmp;
    CelsCompositionScopeFn compositionScope = NULL;
    if (props != NULL) {
        memcpy(&compositionScope, props, sizeof(CelsCompositionScopeFn));
    }
    if (compositionScope != NULL) {
        compositionScope();
    }
}

/**
 * Derives slab geometry from a composable budget.
 *
 * Each composable costs CELS_BYTES_PER_COMPOSABLE bytes — 32 of structural
 * bookkeeping (one CelsSlotGroup) plus a 96-byte slot budget shared by
 * everything it remembers and everything it watches — rounded up to a page. The
 * group capacity is therefore the budget itself, and whatever the rounding adds
 * lands in the slot buffer.
 *
 * @param maxComposables How many composables the session expects at once (> 0).
 * @param outSlabSize    Receives the page-rounded byte size. Non-NULL.
 * @param outMaxGroups   Receives the group capacity. Non-NULL.
 */
static void
SlabGeometryDerive(uint32_t maxComposables,
                   size_t *outSlabSize,
                   uint32_t *outMaxGroups)
{
    CELS_ASSERT(maxComposables > 0u);
    CELS_ASSERT(outSlabSize != NULL);
    CELS_ASSERT(outMaxGroups != NULL);

    const size_t rawBytes =
        (size_t)maxComposables * (size_t)CELS_BYTES_PER_COMPOSABLE;
    const size_t pageMask = (size_t)CELS_SLAB_PAGE_SIZE - 1u;
    const size_t rounded = (rawBytes + pageMask) & ~pageMask;

    *outSlabSize =
        (rounded < CELS_SLAB_PAGE_SIZE) ? CELS_SLAB_PAGE_SIZE : rounded;
    *outMaxGroups = maxComposables;
}

/* ========================================================================= */
/* Session API                                                               */
/* ========================================================================= */

CelsResult
CelsSessionInit(CelsSession *session, const CelsSessionConfig *config)
{
    if (session == NULL || config == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    memset(session, 0, sizeof(*session));
    session->compositionScope = config->compositionScope;
    session->transactionContext = config->transactionContext;
    session->maxDrainIterations = config->maxDrainIterations > 0
        ? config->maxDrainIterations
        : CELS_DEFAULT_MAX_DRAIN_ITERATIONS;

    if (config->compositionScope == NULL) {
        return CELS_OK;
    }

    // A composable budget derives both numbers, so neither has to be sized by
    // hand; slabSize/maxGroups are ignored whenever it is set.
    size_t derivedSlabSize = 0;
    uint32_t derivedMaxGroups = 0;
    if (config->maxComposables > 0u) {
        SlabGeometryDerive(config->maxComposables,
                           &derivedSlabSize,
                           &derivedMaxGroups);
    }

    const size_t slabSize = (config->maxComposables > 0u)
        ? derivedSlabSize
        : (config->slabSize >= CELS_SLAB_PAGE_SIZE ? config->slabSize
                                                   : CELS_SLAB_PAGE_SIZE);
    const uint32_t maxGroups = (config->maxComposables > 0u)
        ? derivedMaxGroups
        : (config->maxGroups > 0 ? config->maxGroups : 32u);

    session->rootSlab = CelsAlignedAlloc(slabSize, CELS_CACHE_LINE_SIZE);
    if (session->rootSlab == NULL) {
        return CELS_ERROR_OUT_OF_MEMORY;
    }

    const CelsResult hostRes = CelsCompositionHostInit(&session->rootHost,
                                                       session->rootSlab,
                                                       slabSize,
                                                       maxGroups,
                                                       CelsSessionRootTrampoline);
    if (hostRes != CELS_OK) {
        CelsAlignedFree(session->rootSlab);
        session->rootSlab = NULL;
        return hostRes;
    }

    session->slabSize = slabSize;
    memcpy(session->rootHost.props,
           &config->compositionScope,
           sizeof(CelsCompositionScopeFn));

    // Nothing has composed yet, so the first pass must run the whole tree.
    session->rootHost.isDirty = true;

    return CELS_OK;
}

void
CelsSessionDestroy(CelsSession *session)
{
    if (session == NULL) {
        return;
    }

    // Cells outlive sessions, so a watch record surviving this call would hold
    // a pointer into a freed slab and dirty whatever next occupies the address.
    // This is why "frees everything in one shot, no per-composable cleanup" is
    // a claim about callbacks only, not about bookkeeping.
    CelsMutableStateUnsubscribeHost(&session->rootHost);

    if (session->rootSlab != NULL) {
        (void)CelsSlotTableReset(&session->rootHost.slotTable);
        CelsAlignedFree(session->rootSlab);
        session->rootSlab = NULL;
    }
    memset(session, 0, sizeof(*session));
}

void
CelsSessionMarkDirty(CelsSession *session)
{
    if (session == NULL) {
        return;
    }

    // The coarse escape hatch is not a separate mechanism: it invalidates the
    // root composable, so it travels the same queue as any cell write and just
    // covers the whole tree. Before anything has composed there is no root
    // group to name, and whole-host dirty is exactly the right granularity.
    const uint32_t groupCount =
        (session->rootSlab != NULL)
            ? CelsSlotTableGroupCount(&session->rootHost.slotTable)
            : 0u;
    const CelsComposableId rootComposable =
        (groupCount > 0u) ? (CelsComposableId)0 : CELS_COMPOSABLE_ID_INVALID;

    (void)CelsCompositionHostInvalidate(&session->rootHost, rootComposable);
}

bool
CelsSessionIsDirty(const CelsSession *session)
{
    if (session == NULL) {
        return false;
    }
    return session->rootHost.isDirty || session->rootHost.invalidationCount > 0u
        || session->pendingDestroyCount > 0u;
}

size_t
CelsSessionGetSlabSize(const CelsSession *session)
{
    return session != NULL ? session->slabSize : 0;
}

CEL_CompositionScope
CelsSessionGetCompositionScope(const CelsSession *session)
{
    CEL_CompositionScope scope;
    memset(&scope, 0, sizeof(scope));
    if (session == NULL || session->rootSlab == NULL) {
        return scope;
    }

    const CelsCompositionHost *const host = &session->rootHost;
    const uint32_t groupCount = CelsSlotTableGroupCount(&host->slotTable);
    scope.groupCount = groupCount;

    if (groupCount > 0u) {
        uint32_t physical = 0;
        if (CelsSlotTableGroupToPhysicalIdx(&host->slotTable, 0, &physical)
            == CELS_OK) {
            scope.id = CelsIdFromKey(host->slotTable.groups[physical].key);
        }
    }

    return scope;
}

/* ========================================================================= */
/* Recompose pass                                                            */
/* ========================================================================= */

void
CelsCompositionHostShiftInvalidations(CelsCompositionHost *host,
                                      CelsComposableId threshold,
                                      int32_t delta)
{
    if (host == NULL || delta == 0) {
        return;
    }

    for (uint32_t i = 0; i < host->invalidationCount; i++) {
        CelsComposableId *const queued = &host->invalidationQueue[i];
        if (*queued == CELS_COMPOSABLE_ID_INVALID || *queued < threshold) {
            continue;
        }
        *queued = (CelsComposableId)((int64_t)*queued + delta);
    }
}

CelsResult
CelsCompositionHostInvalidate(CelsCompositionHost *host,
                              CelsComposableId composable)
{
    if (host == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    // Marking the host is the whole of the work here. No flag is set and no
    // parent chain is walked: propagation is charged to the drain, once per
    // invalidated node per iteration, so the walk can stay a pure O(1) skip
    // everywhere invalidation did not reach.
    host->isDirty = true;

    if (composable == CELS_COMPOSABLE_ID_INVALID) {
        // "Whole host dirty" names no group, so it has nothing to queue.
        return CELS_OK;
    }

    if (host->invalidationCount >= CELS_HOST_INVALIDATION_CAPACITY) {
        // Degrade to the coarser grain rather than drop the invalidation. A
        // dropped one fails silently — the state changes and the tree does not.
        return CELS_OK;
    }

    host->invalidationQueue[host->invalidationCount] = composable;
    host->invalidationCount++;
    return CELS_OK;
}

void
CelsSessionSetTransactionContext(CelsSession *session,
                                 const CelsTransactionContext *context)
{
    if (session == NULL) {
        return;
    }
    if (context == NULL) {
        memset(&session->transactionContext, 0,
               sizeof(session->transactionContext));
        return;
    }
    session->transactionContext = *context;
}

CelsResult
CelsLifecycleMarkForDestroy(CelsSession *session, uint32_t key)
{
    if (session == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    if (session->pendingDestroyCount >= CELS_LIFECYCLE_PENDING_CAPACITY) {
        return CELS_ERROR_CAPACITY_EXCEEDED;
    }
    session->pendingDestroy[session->pendingDestroyCount++] = key;
    return CELS_OK;
}

/**
 * Reports whether the session still owes composition work.
 *
 * This is the whole of the quiet path: a queue check and two flags, never a
 * tree walk. It is also the drain loop's own condition, so an invalidation
 * raised while a body was running keeps the same call iterating.
 *
 * @param session Session to inspect. Non-NULL.
 * @return true if anything is queued, dirty or marked for destruction.
 */
static bool
SessionWorkOutstanding(const CelsSession *session)
{
    CELS_ASSERT(session != NULL);

    return session->rootHost.isDirty ||
           session->rootHost.invalidationCount > 0u ||
           session->pendingDestroyCount > 0u;
}

/**
 * Applies every queued invalidation to the slot table and empties the queue.
 *
 * Each id gets CELS_GROUP_FLAG_INVALIDATED, and every group between it and the
 * root gets CELS_GROUP_FLAG_CONTAINS_INVALIDATED, so no ancestor can O(1)-skip
 * past a subtree the queue reached.
 *
 * @param host Host whose queue should be drained. Non-NULL.
 */
static void
HostInvalidationQueueDrain(CelsCompositionHost *host)
{
    CELS_ASSERT(host != NULL);

    const uint32_t count = host->invalidationCount;
    for (uint32_t i = 0; i < count; i++) {
        // An id whose group was pruned between the queue write and this drain
        // is a legitimate outcome, not a bug: ids are reused and the queue is
        // append-only. The host is dirty regardless, so the walk still covers
        // whatever replaced it.
        (void)CelsSlotTableGroupInvalidate(&host->slotTable,
                                           host->invalidationQueue[i]);
    }

    host->invalidationCount = 0;
}

/**
 * Shrinks the recorded subtree size of every ancestor of a removed subtree.
 *
 * @param table       Table holding the groups. Non-NULL.
 * @param parentIndex Logical index of the removed subtree's parent, or
 *                    UINT32_MAX when it had none.
 * @param count       Number of groups removed.
 */
static void
GroupAncestorsSubtreeShrink(CelsSlotTable *table,
                            uint32_t parentIndex,
                            uint32_t count)
{
    CELS_ASSERT(table != NULL);

    const uint32_t limit = CelsSlotTableGroupCount(table);
    uint32_t ancestor = parentIndex;

    for (uint32_t step = 0; step < limit && ancestor != UINT32_MAX; step++) {
        uint32_t phys = 0;
        if (CelsSlotTableGroupToPhysicalIdx(table, ancestor, &phys)
            != CELS_OK) {
            return;
        }

        CelsSlotGroup *const group = &table->groups[phys];
        group->groupSize = (group->groupSize > (uint16_t)count)
            ? (uint16_t)(group->groupSize - (uint16_t)count)
            : 0u;
        ancestor = group->parentIndex;
    }
}

/**
 * Reclaims a contiguous run of groups and the slots they own.
 *
 * Slides the group gap onto the run, absorbs it and its slots into the two
 * gaps, then repairs the parentIndex of everything that shifted down. That
 * repair is not optional: logical indices past the hole all move, and a
 * parentIndex left naming its old index would silently point at an unrelated
 * group.
 *
 * @param table      Table to reclaim from. Non-NULL.
 * @param startIndex Logical index of the first group to remove.
 * @param count      Number of consecutive groups to remove.
 */
static void
SlotTableSubtreeRemove(CelsSlotTable *table,
                       CelsComposableId startIndex,
                       uint32_t count)
{
    CELS_ASSERT(table != NULL);

    if (count == 0u) {
        return;
    }
    if (CelsSlotTableMoveGapTo(table, startIndex) != CELS_OK) {
        return;
    }

    const uint32_t groupGapEnd = table->groupGapStart + table->groupGapLen;
    const CelsSlotGroup *const removed = &table->groups[groupGapEnd];

    uint32_t removedSlots = 0;
    for (uint32_t i = 0; i < count; i++) {
        removedSlots += (uint32_t)removed[i].slotCount;
    }

    const uint32_t slotGapEnd = table->slotGapStart + table->slotGapLen;

    memset(&table->groups[groupGapEnd], 0, count * sizeof(CelsSlotGroup));
    memset(&table->slots[slotGapEnd], 0,
           removedSlots * sizeof(CelsSlotValue));

    table->groupGapLen += count;
    table->slotGapLen += removedSlots;

    CelsSlotTableGroupsShiftParents(table, startIndex + count,
                                    -(int32_t)count);
}
/**
 * Destroys every root Composition marked via CelsLifecycleMarkForDestroy.
 *
 * Runs before anything composes this pass, so a Composition marked for death
 * never executes its body again. Two claims hold here at once, and they are
 * different claims: callbacks do NOT cascade — onDestroy fires exactly once,
 * for the Composition's own root composable — while cleanup DOES, silently, so
 * that no watch record in the subtree outlives the ids it names.
 *
 * Marks appended by an onDestroy callback are left queued for the next pass
 * rather than swallowed by the reset.
 *
 * @param session Session whose marks should be consumed. Non-NULL.
 */
static void
SessionPendingDestroyRun(CelsSession *session)
{
    CELS_ASSERT(session != NULL);

    const uint32_t pending = session->pendingDestroyCount;
    if (pending == 0u) {
        return;
    }

    CelsCompositionHost *const host = &session->rootHost;

    for (uint32_t i = 0; i < pending; i++) {
        const uint32_t key = session->pendingDestroy[i];

        uint32_t rootIndex = 0;
        const CelsSlotGroup *const rootGroup =
            CelsSlotTableFindGroup(&host->slotTable, key, &rootIndex);
        if (rootGroup == NULL) {
            // Already gone — marking a dead Composition twice is not an error.
            continue;
        }

        const uint32_t count = 1u + (uint32_t)rootGroup->groupSize;
        const uint32_t parentIndex = rootGroup->parentIndex;

        // Cleanup cascades and fires nothing. Ids are reused the moment the
        // space is reclaimed, so a subscription outliving its composable would
        // not leak harmlessly — it would dirty the next occupant of the slot.
        for (uint32_t offset = 0; offset < count; offset++) {
            CelsMutableStateUnsubscribe(host,
                                        (CelsComposableId)(rootIndex + offset));
        }

        // Callbacks do not cascade: one onDestroy, for the root composable.
        CelsTransactionNotifyDestroy((CelsComposableId)rootIndex);

        GroupAncestorsSubtreeShrink(&host->slotTable, parentIndex, count);
        SlotTableSubtreeRemove(&host->slotTable, rootIndex, count);
    }

    if (session->pendingDestroyCount > pending) {
        const uint32_t carried = session->pendingDestroyCount - pending;
        memmove(&session->pendingDestroy[0],
                &session->pendingDestroy[pending],
                (size_t)carried * sizeof(session->pendingDestroy[0]));
        session->pendingDestroyCount = carried;
    } else {
        session->pendingDestroyCount = 0;
    }
}

/**
 * Runs the host's root composable once, on the calling thread.
 *
 * Publishes the ambient invalidation context first, so a cell read from inside
 * a body subscribes this host, and clears it afterwards so the same read from a
 * network handler stays a plain read. The host is marked clean BEFORE the body
 * runs, never after: an invalidation raised during the walk must survive into
 * the next drain iteration, and a clear afterwards would swallow it.
 *
 * @param session Session whose root host should compose. Non-NULL.
 */
static void
SessionHostCompose(CelsSession *session)
{
    CELS_ASSERT(session != NULL);

    CelsCompositionHost *const host = &session->rootHost;
    host->isDirty = false;

    if (host->rootFn == NULL) {
        return;
    }

    CelsComposer composer;
    memset(&composer, 0, sizeof(composer));
    CelsComposerBegin(&composer, &host->slotTable);

    CelsComposerSetCurrent(&composer);
    CelsInvalidationContextSet(host);

    host->rootFn(&composer, host->props);

    CelsInvalidationContextSet(NULL);
    CelsComposerSetCurrent(NULL);

    // Backstop. The composer clears each group's flags as it enters, so this
    // is normally a no-op; it exists for groups the walk never reached, such
    // as an ancestor left marked when its invalidated descendant was pruned
    // out from under it. Cheap insurance against a flag sticking forever.
    CelsSlotTableClearAllFlags(&host->slotTable);
}

CelsResult
CelsSessionRecompose(CelsSession *session)
{
    if (session == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    if (!SessionWorkOutstanding(session)) {
        return CELS_OK;
    }

    const uint32_t maxIterations = session->maxDrainIterations > 0u
        ? session->maxDrainIterations
        : CELS_DEFAULT_MAX_DRAIN_ITERATIONS;

    // Published for the whole pass, cleared on every exit: the composer fires
    // through it inline, at the moment a composable mounts or is pruned.
    CelsTransactionContextSet(&session->transactionContext);

    CelsResult result = CELS_OK;
    uint32_t iterations = 0;

    while (SessionWorkOutstanding(session)) {
        iterations++;
        if (iterations > maxIterations) {
            // Abandoned, not corrupted. The queue is left exactly as the last
            // walk left it and the host stays dirty, so the next call resumes
            // from here and nothing is lost.
            result = CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE;
            goto cleanup;
        }

        HostInvalidationQueueDrain(&session->rootHost);
        SessionPendingDestroyRun(session);
        SessionHostCompose(session);
    }

cleanup:
    CelsTransactionContextSet(NULL);
    return result;
}
