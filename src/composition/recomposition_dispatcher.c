#include "composition/recomposition_dispatcher.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "flecs.h"

#include "composition/composer.h"
#include "composition/slottable/slot_table.h"
#include "composition/state.h"

#define CELS_ASSERT(cond) assert(cond)
#define CELS_MAX_DIRTY_QUERY_BUFFER 1024u

/** Bytes one composable costs: 32 structural (CelsSlotGroup) + 96 of slots. */
#define CELS_BYTES_PER_COMPOSABLE 128u

/** Page the derived slab size is rounded up to. */
#define CELS_SLAB_PAGE_SIZE 4096u

/* ========================================================================= */
/* Static Helpers & Thread Worker Function                                   */
/* ========================================================================= */

/**
 * Worker thread execution loop.
 *
 * Waits on cvStart until work is dispatched or shutdown is signaled.
 * Iterates assigned host slice, instantiates a transient CelsComposer bound to
 * the worker's private stage, executes the host's root composable function,
 * clears the isDirty flag, and signals cvDone.
 *
 * @param arg Pointer to CelsWorkerThread. Non-NULL.
 * @return NULL on thread termination.
 */
static void *
WorkerThreadLoop(void *arg)
{
    CelsWorkerThread *const worker = (CelsWorkerThread *)arg;
    CELS_ASSERT(worker != NULL);

    while (1) {
        pthread_mutex_lock(&worker->mutex);
        while (!worker->hasWork && !worker->shutdown) {
            pthread_cond_wait(&worker->cvStart, &worker->mutex);
        }

        if (worker->shutdown) {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }

        CelsWorkerSlice *const slice = &worker->slice;
        const uint32_t count = slice->hostCount;

        for (uint32_t i = 0; i < count; i++) {
            CelsCompositionHost *const host = slice->hosts[i];
            if (host == NULL || host->rootFn == NULL) {
                continue;
            }

            CelsComposer cmp;
            memset(&cmp, 0, sizeof(cmp));
            CelsComposerBegin(&cmp, &host->slotTable);
            cmp.stage = slice->stage;

            CelsComposerSetCurrent(&cmp);
            host->rootFn(&cmp, host->props);
            CelsComposerSetCurrent(NULL);

            host->isDirty = false;
        }

        worker->hasWork = false;
        pthread_cond_signal(&worker->cvDone);
        pthread_mutex_unlock(&worker->mutex);
    }

    return NULL;
}

/**
 * Comparator function for qsort ordering dirty hosts descending by complexity.
 *
 * @param a Pointer to pointer to CelsCompositionHost.
 * @param b Pointer to pointer to CelsCompositionHost.
 * @return Negative if weight(a) > weight(b), positive if weight(a) < weight(b),
 *         zero if equal.
 */
static int
CompareHostWeightDescending(const void *a, const void *b)
{
    const CelsCompositionHost *const hostA =
        *(const CelsCompositionHost *const *)a;
    const CelsCompositionHost *const hostB =
        *(const CelsCompositionHost *const *)b;

    const uint32_t weightA = CelsCompositionHostGetWeight(hostA);
    const uint32_t weightB = CelsCompositionHostGetWeight(hostB);

    if (weightA > weightB) {
        return -1;
    }
    if (weightA < weightB) {
        return 1;
    }
    return 0;
}

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

uint32_t
CelsCompositionHostGetWeight(const CelsCompositionHost *host)
{
    if (host == NULL) {
        return 1;
    }

    const uint32_t groupCount = CelsSlotTableGroupCount(&host->slotTable);
    if (groupCount > 0 && host->slotTable.groups != NULL) {
        const uint32_t rootNodeCount = host->slotTable.groups[0].nodeCount;
        if (rootNodeCount > 0) {
            return rootNodeCount;
        }
        return groupCount;
    }

    return 1;
}

/* ========================================================================= */
/* Dispatcher Lifecycle API                                                  */
/* ========================================================================= */

CelsResult
CelsDispatcherInit(CelsDispatcher *disp,
                   ecs_world_t *world,
                   uint32_t threadCount)
{
    if (disp == NULL || world == NULL || threadCount == 0 ||
        threadCount > CELS_DISPATCHER_MAX_THREADS) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    memset(disp, 0, sizeof(*disp));
    disp->world = world;
    disp->threadCount = threadCount;

    for (uint32_t i = 0; i < threadCount; i++) {
        CelsWorkerThread *const worker = &disp->workers[i];
        worker->workerIndex = i;
        worker->hasWork = false;
        worker->shutdown = false;

        worker->slice.world = world;
        worker->slice.stage = ecs_stage_new(world);
        if (worker->slice.stage == NULL) {
            CelsDispatcherDestroy(disp);
            return CELS_ERROR_OUT_OF_MEMORY;
        }

        if (pthread_mutex_init(&worker->mutex, NULL) != 0) {
            CelsDispatcherDestroy(disp);
            return CELS_ERROR_INVALID_STATE;
        }

        if (pthread_cond_init(&worker->cvStart, NULL) != 0) {
            pthread_mutex_destroy(&worker->mutex);
            CelsDispatcherDestroy(disp);
            return CELS_ERROR_INVALID_STATE;
        }

        if (pthread_cond_init(&worker->cvDone, NULL) != 0) {
            pthread_cond_destroy(&worker->cvStart);
            pthread_mutex_destroy(&worker->mutex);
            CelsDispatcherDestroy(disp);
            return CELS_ERROR_INVALID_STATE;
        }

        if (pthread_create(&worker->thread, NULL, WorkerThreadLoop, worker) !=
            0) {
            pthread_cond_destroy(&worker->cvDone);
            pthread_cond_destroy(&worker->cvStart);
            pthread_mutex_destroy(&worker->mutex);
            CelsDispatcherDestroy(disp);
            return CELS_ERROR_INVALID_STATE;
        }
    }

    const CelsResult bootRes = CelsDispatcherBootstrapPhases(disp, world);
    if (bootRes != CELS_OK) {
        CelsDispatcherDestroy(disp);
        return bootRes;
    }

    return CELS_OK;
}

void
CelsDispatcherDestroy(CelsDispatcher *disp)
{
    if (disp == NULL) {
        return;
    }

    for (uint32_t i = 0; i < disp->threadCount; i++) {
        CelsWorkerThread *const worker = &disp->workers[i];

        pthread_mutex_lock(&worker->mutex);
        worker->shutdown = true;
        pthread_cond_signal(&worker->cvStart);
        pthread_mutex_unlock(&worker->mutex);

        pthread_join(worker->thread, NULL);

        pthread_cond_destroy(&worker->cvDone);
        pthread_cond_destroy(&worker->cvStart);
        pthread_mutex_destroy(&worker->mutex);

        if (worker->slice.stage != NULL) {
            ecs_stage_free(worker->slice.stage);
            worker->slice.stage = NULL;
        }
    }

    if (disp->hostQuery != NULL) {
        ecs_query_fini(disp->hostQuery);
        disp->hostQuery = NULL;
    }

    disp->threadCount = 0;
}

/* ========================================================================= */
/* Flecs Custom Pipeline & Systems Integration                               */
/* ========================================================================= */

ecs_entity_t ecs_id(CelsCompositionHost);

CelsResult
CelsDispatcherBootstrapPhases(CelsDispatcher *disp, ecs_world_t *world)
{
    if (disp == NULL || world == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    // 1. Register CelsCompositionHost component in Flecs
    ECS_COMPONENT_DEFINE(world, CelsCompositionHost);

    // 2. Custom phases spliced before EcsPreUpdate:
    //    EcsPostLoad -> OnRecompose -> PostRecomposeSync -> EcsPreUpdate
    disp->phaseOnRecompose = ecs_new_w_id(world, EcsPhase);
    ecs_set_name(world, disp->phaseOnRecompose, "OnRecompose");
    ecs_add_pair(world, disp->phaseOnRecompose, EcsDependsOn, EcsPostLoad);

    disp->phasePostRecomposeSync = ecs_new_w_id(world, EcsPhase);
    ecs_set_name(world, disp->phasePostRecomposeSync, "PostRecomposeSync");
    ecs_add_pair(world,
                 disp->phasePostRecomposeSync,
                 EcsDependsOn,
                 disp->phaseOnRecompose);

    ecs_add_pair(world,
                 EcsPreUpdate,
                 EcsDependsOn,
                 disp->phasePostRecomposeSync);

    // 3. Register pipeline phase systems
    disp->systemRecompose = ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "SystemRunRecomposition",
            .add = ecs_ids(ecs_dependson(disp->phaseOnRecompose))
        }),
        .callback = CelsDispatcherRecompositionRun,
        .ctx = disp
    });

    disp->systemSync = ecs_system(world, {
        .entity = ecs_entity(world, {
            .name = "SystemPostRecomposeSync",
            .add = ecs_ids(ecs_dependson(disp->phasePostRecomposeSync))
        }),
        .callback = CelsDispatcherPostRecomposeSync,
        .ctx = disp
    });

    // 4. Query for hosts with CelsCompositionHost component
    disp->hostQuery = ecs_query(world, {
        .terms = {
            { .id = ecs_id(CelsCompositionHost) }
        }
    });

    return CELS_OK;
}

/* ========================================================================= */
/* LPT Load Balancing & System Callbacks                                     */
/* ========================================================================= */

CelsResult
CelsDispatcherBalanceWorkload(CelsDispatcher *disp,
                              CelsCompositionHost **dirtyHosts,
                              uint32_t hostCount)
{
    if (disp == NULL || (hostCount > 0 && dirtyHosts == NULL)) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t threadCount = disp->threadCount;
    for (uint32_t w = 0; w < threadCount; w++) {
        disp->workers[w].slice.hostCount = 0;
        disp->workers[w].slice.totalWeight = 0;
    }

    if (hostCount == 0) {
        return CELS_OK;
    }

    // Step 1: Sort dirty hosts descending by complexity weight (LPT)
    qsort(dirtyHosts,
          (size_t)hostCount,
          sizeof(CelsCompositionHost *),
          CompareHostWeightDescending);

    // Step 2: Greedily pack hosts into the bucket with minimum total weight
    for (uint32_t i = 0; i < hostCount; i++) {
        CelsCompositionHost *const host = dirtyHosts[i];
        const uint32_t weight = CelsCompositionHostGetWeight(host);

        uint32_t minBucket = 0;
        uint32_t minWeight = disp->workers[0].slice.totalWeight;

        for (uint32_t w = 1; w < threadCount; w++) {
            if (disp->workers[w].slice.totalWeight < minWeight) {
                minWeight = disp->workers[w].slice.totalWeight;
                minBucket = w;
            }
        }

        CelsWorkerSlice *const slice = &disp->workers[minBucket].slice;
        if (slice->hostCount >= CELS_WORKER_MAX_HOSTS) {
            return CELS_ERROR_CAPACITY_EXCEEDED;
        }

        slice->hosts[slice->hostCount++] = host;
        slice->totalWeight += weight;
    }

    for (uint32_t w = 0; w < threadCount; w++) {
        disp->lastDispatchedWeight[w] = disp->workers[w].slice.totalWeight;
    }
    disp->totalDispatchedHosts += hostCount;

    return CELS_OK;
}

void
CelsDispatcherRecompositionRun(ecs_iter_t *it)
{
    CelsDispatcher *const disp = (CelsDispatcher *)it->ctx;
    if (disp == NULL || disp->threadCount == 0 || disp->hostQuery == NULL) {
        return;
    }

    // 1. Gather all dirty hosts via Flecs query
    CelsCompositionHost *dirtyHosts[CELS_MAX_DIRTY_QUERY_BUFFER];
    uint32_t dirtyCount = 0;

    ecs_iter_t qit = ecs_query_iter(disp->world, disp->hostQuery);
    while (ecs_query_next(&qit)) {
        CelsCompositionHost *const hosts =
            ecs_field(&qit, CelsCompositionHost, 0);
        const int count = qit.count;

        for (int i = 0; i < count; i++) {
            if (hosts[i].isDirty) {
                if (dirtyCount < CELS_MAX_DIRTY_QUERY_BUFFER) {
                    dirtyHosts[dirtyCount++] = &hosts[i];
                }
            }
        }
    }

    if (dirtyCount == 0) {
        for (uint32_t w = 0; w < disp->threadCount; w++) {
            disp->workers[w].slice.hostCount = 0;
            disp->workers[w].slice.totalWeight = 0;
            disp->lastDispatchedWeight[w] = 0;
        }
        return;
    }

    // 2. Distribute workload across slices using LPT greedy bin-packing
    const CelsResult balanceRes =
        CelsDispatcherBalanceWorkload(disp, dirtyHosts, dirtyCount);
    CELS_ASSERT(balanceRes == CELS_OK);

    // 3. Wake up worker threads that have assigned work
    const uint32_t threadCount = disp->threadCount;
    for (uint32_t w = 0; w < threadCount; w++) {
        CelsWorkerThread *const worker = &disp->workers[w];
        if (worker->slice.hostCount > 0) {
            pthread_mutex_lock(&worker->mutex);
            worker->hasWork = true;
            pthread_cond_signal(&worker->cvStart);
            pthread_mutex_unlock(&worker->mutex);
        }
    }

    // 4. Barrier: Main thread waits for all workers to signal completion
    for (uint32_t w = 0; w < threadCount; w++) {
        CelsWorkerThread *const worker = &disp->workers[w];
        pthread_mutex_lock(&worker->mutex);
        while (worker->hasWork) {
            pthread_cond_wait(&worker->cvDone, &worker->mutex);
        }
        pthread_mutex_unlock(&worker->mutex);
    }
}

void
CelsDispatcherPostRecomposeSync(ecs_iter_t *it)
{
    CelsDispatcher *const disp = (CelsDispatcher *)it->ctx;
    if (disp == NULL || disp->threadCount == 0) {
        return;
    }

    // Suspend ambient pipeline readonly & deferral so worker stages can be
    // sequentially merged directly into the canonical archetype tables.
    ecs_suspend_readonly_state_t roState;
    ecs_world_t *const realWorld = flecs_suspend_readonly(it->world, &roState);

    // Sequentially merge all worker stages into the canonical archetype world
    const uint32_t threadCount = disp->threadCount;
    for (uint32_t w = 0; w < threadCount; w++) {
        ecs_world_t *const stage = disp->workers[w].slice.stage;
        if (stage != NULL) {
            ecs_merge(stage);
        }
    }

    flecs_resume_readonly(realWorld, &roState);
}

/* ========================================================================= */
/* Session & Root View Implementation                                        */
/* ========================================================================= */

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

/**
 * Copies the session's own root host over the copy Flecs holds.
 *
 * ecs_set_ptr copied the host into component storage at init, so the session
 * and the world hold two structs over one slab. The sequential recompose pass
 * works on the session's own copy — it is the one whose address cells subscribe
 * against — and every existing accessor (CelsSessionIsDirty,
 * CelsSessionGetCompositionScope) reads the world's. Mirroring once per pass
 * keeps those accessors truthful without putting a Flecs lookup on the walk.
 *
 * @param session Session whose root host should be published. Non-NULL.
 */
static void
SessionEcsHostMirror(CelsSession *session)
{
    CELS_ASSERT(session != NULL);

    if (session->world == NULL || session->rootEntity == 0) {
        return;
    }
    if (!ecs_is_alive(session->world, session->rootEntity)) {
        return;
    }

    ecs_set_ptr(session->world,
                session->rootEntity,
                CelsCompositionHost,
                &session->rootHost);
}

CelsResult
CelsSessionInit(CelsSession *session,
                ecs_world_t *world,
                const CelsSessionConfig *config)
{
    if (session == NULL || world == NULL || config == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    memset(session, 0, sizeof(*session));
    session->world = world;
    session->compositionScope = config->compositionScope;
    session->transactionContext = config->transactionContext;
    session->pendingDestroyCount = 0;
    session->maxDrainIterations = config->maxDrainIterations > 0
        ? config->maxDrainIterations
        : CELS_DEFAULT_MAX_DRAIN_ITERATIONS;

    uint32_t threads = config->workerCount > 0 ? config->workerCount : 4;
    if (threads > CELS_DISPATCHER_MAX_THREADS) {
        threads = CELS_DISPATCHER_MAX_THREADS;
    }

    const CelsResult res = CelsDispatcherInit(&session->dispatcher, world, threads);
    if (res != CELS_OK) {
        return res;
    }

    if (config->compositionScope != NULL) {
        // A composable budget derives both numbers, so neither has to be sized
        // by hand; slabSize/maxGroups are ignored whenever it is set.
        size_t derivedSlabSize = 0;
        uint32_t derivedMaxGroups = 0;
        if (config->maxComposables > 0u) {
            SlabGeometryDerive(config->maxComposables,
                               &derivedSlabSize,
                               &derivedMaxGroups);
        }

        const size_t slabSize = (config->maxComposables > 0u)
            ? derivedSlabSize
            : (config->slabSize >= 4096 ? config->slabSize : 4096);
        const uint32_t maxGroups = (config->maxComposables > 0u)
            ? derivedMaxGroups
            : (config->maxGroups > 0 ? config->maxGroups : 32);

        session->rootSlab = CelsAlignedAlloc(slabSize, 64);
        if (session->rootSlab == NULL) {
            CelsDispatcherDestroy(&session->dispatcher);
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
            CelsDispatcherDestroy(&session->dispatcher);
            return hostRes;
        }

        session->slabSize = slabSize;
        memcpy(session->rootHost.props, &config->compositionScope, sizeof(CelsCompositionScopeFn));
        session->rootHost.isDirty = true;

        session->rootEntity = ecs_entity(world, { .name = "CelsRootHost" });
        ecs_set_ptr(world, session->rootEntity, CelsCompositionHost, &session->rootHost);
    }

    return CELS_OK;
}

void
CelsSessionDestroy(CelsSession *session)
{
    if (session == NULL) {
        return;
    }
    CelsDispatcherDestroy(&session->dispatcher);

    // Cells outlive sessions, so a watch record surviving this call would hold
    // a pointer into a freed slab and dirty whatever next occupies the address.
    // This is why "frees everything in one shot, no per-composable cleanup" is
    // a claim about callbacks only, not about bookkeeping.
    CelsMutableStateUnsubscribeHost(&session->rootHost);

    if (session->world != NULL && session->rootEntity != 0) {
        if (ecs_is_alive(session->world, session->rootEntity)) {
            ecs_delete(session->world, session->rootEntity);
        }
        session->rootEntity = 0;
    }
    if (session->rootSlab != NULL) {
        CelsSlotTableReset(&session->rootHost.slotTable);
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

    // The Flecs-pipeline path reads its own copy of the host, so the flag has
    // to reach that one too for ecs_progress-driven dispatch to notice.
    if (session->world == NULL || session->rootEntity == 0) {
        return;
    }
    CelsCompositionHost *const ecsHost =
        ecs_get_mut(session->world, session->rootEntity, CelsCompositionHost);
    if (ecsHost != NULL) {
        ecsHost->isDirty = true;
        ecs_modified(session->world, session->rootEntity, CelsCompositionHost);
    }
}

bool
CelsSessionIsDirty(const CelsSession *session)
{
    if (session == NULL || session->world == NULL || session->rootEntity == 0) {
        return false;
    }
    const CelsCompositionHost *host =
        ecs_get(session->world, session->rootEntity, CelsCompositionHost);
    return host != NULL && host->isDirty;
}

ecs_entity_t
CelsSessionGetRootEntity(const CelsSession *session)
{
    return session != NULL ? session->rootEntity : 0;
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
    if (session == NULL || session->world == NULL || session->rootEntity == 0) {
        return scope;
    }

    const CelsCompositionHost *host =
        ecs_get(session->world, session->rootEntity, CelsCompositionHost);
    if (host == NULL) {
        scope.entity = session->rootEntity;
        return scope;
    }

    const uint32_t groupCount = CelsSlotTableGroupCount(&host->slotTable);
    scope.groupCount = groupCount;

    if (groupCount > 0) {
        uint32_t phys = 0;
        if (CelsSlotTableGroupToPhysicalIdx(&host->slotTable, 0, &phys) == CELS_OK) {
            scope.id = CelsIdFromKey(host->slotTable.groups[phys].key);
            scope.entity = host->slotTable.groups[phys].entityId;
        } else {
            scope.entity = session->rootEntity;
        }
    } else {
        scope.entity = session->rootEntity;
    }

    return scope;
}

/* ========================================================================= */
/* Recompose pass                                                            */
/* ========================================================================= */

/*
 * The synchronous recompose pass. It sits ALONGSIDE the Flecs-pipeline dispatch
 * above rather than replacing it: CelsSessionRecompose is the sequential,
 * single-threaded entry point CELS.md is written around, while ecs_progress
 * keeps driving the worker pool for callers already on that path.
 *
 * The two are not meant to drive one session between them. Each keeps its own
 * CelsSlotTable bookkeeping over the shared slab — this path on the session's
 * own host, the pipeline on the copy Flecs stores — so alternating them within
 * one session would have each walk from the other's gap positions. Pick one per
 * session; CelsSessionRecompose republishes its result to the Flecs copy at the
 * end of every pass so the read-only accessors stay truthful either way.
 */

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
 * Deletes the Flecs entities a subtree owns, before its groups are reclaimed.
 *
 * Bookkeeping, not a callback path: deleting a parent already removes its
 * children in Flecs, so each handle is checked for liveness first.
 *
 * @param session    Session owning the world. Non-NULL.
 * @param startIndex Logical index of the subtree root.
 * @param count      Number of groups in the subtree.
 */
static void
SubtreeEntitiesDelete(CelsSession *session,
                      CelsComposableId startIndex,
                      uint32_t count)
{
    CELS_ASSERT(session != NULL);

    if (session->world == NULL) {
        return;
    }

    const CelsSlotTable *const table = &session->rootHost.slotTable;
    for (uint32_t offset = 0; offset < count; offset++) {
        uint32_t phys = 0;
        if (CelsSlotTableGroupToPhysicalIdx(table, startIndex + offset, &phys)
            != CELS_OK) {
            continue;
        }

        const ecs_entity_t entity =
            (ecs_entity_t)table->groups[phys].entityId;
        if (entity != 0 && ecs_is_alive(session->world, entity)) {
            ecs_delete(session->world, entity);
        }
    }
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

        SubtreeEntitiesDelete(session, rootIndex, count);

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

    // The walk is sequential and single-threaded, so it composes straight into
    // the canonical world rather than a worker stage. Nothing to merge after.
    composer.stage = session->world;

    CelsComposerSetCurrent(&composer);
    CelsInvalidationContextSet(host);

    host->rootFn(&composer, host->props);

    CelsInvalidationContextSet(NULL);
    CelsComposerSetCurrent(NULL);

    // Stand-in for the per-group clear the §2.1 gate performs on entering a
    // group. Until the composer reads the flags, clearing them wholesale after
    // the walk is what stops this pass's invalidations leaking into the next.
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
    SessionEcsHostMirror(session);
    return result;
}
