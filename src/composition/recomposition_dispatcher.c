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

#define CELS_ASSERT(cond) assert(cond)
#define CELS_MAX_DIRTY_QUERY_BUFFER 1024u

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

    uint32_t threads = config->workerCount > 0 ? config->workerCount : 4;
    if (threads > CELS_DISPATCHER_MAX_THREADS) {
        threads = CELS_DISPATCHER_MAX_THREADS;
    }

    const CelsResult res = CelsDispatcherInit(&session->dispatcher, world, threads);
    if (res != CELS_OK) {
        return res;
    }

    if (config->compositionScope != NULL) {
        const size_t slabSize = config->slabSize >= 4096 ? config->slabSize : 4096;
        const uint32_t maxGroups = config->maxGroups > 0 ? config->maxGroups : 32;

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
    if (session == NULL || session->world == NULL || session->rootEntity == 0) {
        return;
    }
    CelsCompositionHost *host =
        ecs_get_mut(session->world, session->rootEntity, CelsCompositionHost);
    if (host != NULL) {
        host->isDirty = true;
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
