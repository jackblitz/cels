#pragma once

/**
 * @file recomposition_dispatcher.h
 * @brief Multi-threaded Recomposition Dispatcher and Flecs Bridge.
 *
 * Provides thread-isolated parallel recomposition for Cels composition hosts
 * with Longest Processing Time (LPT) greedy load balancing and deterministic
 * serial staging merge into Flecs ECS.
 *
 * Typical usage:
 * @code
 *     ecs_world_t *world = ecs_init();
 *
 *     CelsDispatcher disp;
 *     CelsResult result = CelsDispatcherInit(&disp, world, 4);
 *     if (result != CELS_OK) {
 *         fprintf(stderr, "Dispatcher init failed\n");
 *         return result;
 *     }
 *
 *     // Register composition hosts with world...
 *     // When dirty, progress world pipeline:
 *     ecs_progress(world, 0.016f);
 *
 *     CelsDispatcherDestroy(&disp);
 *     ecs_fini(world);
 * @endcode
 *
 * Thread safety: Worker threads execute against dedicated, thread-isolated
 * ecs_stage_t instances created via ecs_stage_new(). No worker thread ever
 * acquires locks or mutates the shared ecs_world_t during OnRecompose.
 * All staged structural operations are flushed serially on the main thread
 * during PostRecomposeSync via ecs_merge().
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flecs.h"
#include "composition/composer.h"
#include "composition/slottable/slot_table.h"

#define CELS_DISPATCHER_MAX_THREADS 8u
#define CELS_HOST_PROPS_CAPACITY 64u
#define CELS_WORKER_MAX_HOSTS 256u

/**
 * Root composable function pointer.
 *
 * @param cmp   Active transient composer for the host. Non-NULL.
 * @param props Pointer to user props buffer stored in host. Non-NULL.
 */
typedef void (*CelsComposableFn)(CelsComposer *cmp, void *props);

/**
 * Composition host holding an isolated SlotTable, composable root function,
 * user props buffer, dirty flag, and 4KB slab pointer.
 * Fields ordered largest to smallest to eliminate internal padding.
 */
typedef struct CelsCompositionHost {
    uint8_t *slabMemory;                      // 8 bytes: 64-byte aligned 4KB slab
    CelsComposableFn rootFn;                  // 8 bytes: Root composable function
    CelsSlotTable slotTable;                  // 48 bytes: Dual-gap buffer table
    uint8_t props[CELS_HOST_PROPS_CAPACITY];  // 64 bytes: Inline user props buffer
    bool isDirty;                             // 1 byte: Dirty flag for recomposition
    uint8_t _padding[7];                      // 7 bytes: Explicit cache alignment padding
} CelsCompositionHost;

/**
 * Work slice assigned to a single worker thread for parallel recomposition.
 * Fields ordered largest to smallest.
 */
typedef struct CelsWorkerSlice {
    CelsCompositionHost *hosts[CELS_WORKER_MAX_HOSTS]; // Pointers to assigned hosts
    ecs_world_t *stage;                                // Dedicated private worker stage
    ecs_world_t *world;                                // Flecs canonical world handle
    uint32_t hostCount;                                // Number of assigned hosts
    uint32_t totalWeight;                              // Summed complexity weight (LPT)
} CelsWorkerSlice;

/**
 * Worker thread encapsulating POSIX thread synchronization, condition variables,
 * state flags, and assigned work slice.
 */
typedef struct CelsWorkerThread {
    pthread_t thread;       // POSIX worker thread handle
    pthread_mutex_t mutex;  // Per-worker work mutex
    pthread_cond_t cvStart; // Signaled to wake worker for new slice
    pthread_cond_t cvDone;  // Signaled by worker when slice finishes
    CelsWorkerSlice slice;  // Assigned workload slice and private stage
    uint32_t workerIndex;   // Zero-based index of this worker [0, threadCount)
    bool hasWork;           // Flag indicating work is queued/running
    bool shutdown;          // Flag requesting worker loop termination
    uint8_t _padding[6];    // Alignment padding
} CelsWorkerThread;

/**
 * Recomposition dispatcher managing the worker thread pool, private Flecs
 * stages, LPT load balancing, and pipeline phase synchronization.
 */
typedef struct CelsDispatcher {
    ecs_world_t *world;                                         // Canonical world
    ecs_query_t *hostQuery;                                     // Flecs query for hosts
    CelsWorkerThread workers[CELS_DISPATCHER_MAX_THREADS];     // Worker pool
    uint32_t threadCount;                                       // Active worker count (<= 8)
    uint32_t totalDispatchedHosts;                              // Cumulative hosts dispatched
    uint32_t lastDispatchedWeight[CELS_DISPATCHER_MAX_THREADS]; // Last weights per thread
    ecs_entity_t phaseOnRecompose;                              // OnRecompose phase handle
    ecs_entity_t phasePostRecomposeSync;                        // PostRecomposeSync phase
    ecs_entity_t systemRecompose;                               // System handle for recompose
    ecs_entity_t systemSync;                                    // System handle for sync
} CelsDispatcher;

/* ========================================================================= */
/* Public API (PascalCase noun-first, verb-last)                             */
/* ========================================================================= */

/**
 * Initializes a composition host with a caller-provided 4KB memory slab.
 *
 * @param host       Target composition host. Non-NULL.
 * @param slabMemory 64-byte aligned memory slab (at least 4096 bytes). Non-NULL.
 * @param slabSize   Total byte size of slabMemory (>= 4096).
 * @param maxGroups  Group capacity to allocate in the slab.
 * @param rootFn     Root composable function pointer. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_ARGUMENT.
 */
CelsResult CelsCompositionHostInit(CelsCompositionHost *host,
                                   void *slabMemory,
                                   size_t slabSize,
                                   uint32_t maxGroups,
                                   CelsComposableFn rootFn);

/**
 * Flags a composition host as dirty or clean.
 *
 * @param host  Target composition host. Non-NULL.
 * @param dirty Dirty state boolean.
 */
void CelsCompositionHostSetDirty(CelsCompositionHost *host, bool dirty);

/**
 * Checks whether a composition host is currently flagged as dirty.
 *
 * @param host Target composition host. Non-NULL.
 * @return true if dirty, false otherwise.
 */
bool CelsCompositionHostIsDirty(const CelsCompositionHost *host);

/**
 * Computes the execution complexity weight of a host for LPT load balancing.
 *
 * @param host Target composition host. Non-NULL.
 * @return Complexity weight (node count or group count; >= 1).
 */
uint32_t CelsCompositionHostGetWeight(const CelsCompositionHost *host);

/**
 * Initializes the recomposition dispatcher, creates per-worker ecs_stage_t
 * instances, and spawns worker threads.
 *
 * @param disp        Target dispatcher. Non-NULL.
 * @param world       Canonical Flecs world. Non-NULL.
 * @param threadCount Number of worker threads to spawn (1 to 8).
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or CELS_ERROR_INVALID_STATE.
 */
CelsResult CelsDispatcherInit(CelsDispatcher *disp,
                              ecs_world_t *world,
                              uint32_t threadCount);

/**
 * Signals worker threads to shutdown, waits for threads to join, frees
 * per-worker stages, and destroys condition variables and mutexes.
 *
 * @param disp Target dispatcher. NULL is safely accepted and does nothing.
 */
void CelsDispatcherDestroy(CelsDispatcher *disp);

/**
 * Bootstraps custom phases OnRecompose and PostRecomposeSync spliced before
 * EcsPreUpdate, and registers the dispatcher systems.
 *
 * @param disp  Target dispatcher. Non-NULL.
 * @param world Canonical Flecs world. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_ARGUMENT.
 */
CelsResult CelsDispatcherBootstrapPhases(CelsDispatcher *disp,
                                         ecs_world_t *world);

/**
 * Distributes dirty composition hosts across worker buckets using Longest
 * Processing Time (LPT) first greedy bin-packing.
 *
 * @param disp       Target dispatcher. Non-NULL.
 * @param dirtyHosts Array of pointers to dirty composition hosts. Non-NULL.
 * @param hostCount  Number of hosts in dirtyHosts array.
 * @return CELS_OK or CELS_ERROR_CAPACITY_EXCEEDED.
 */
CelsResult CelsDispatcherBalanceWorkload(CelsDispatcher *disp,
                                         CelsCompositionHost **dirtyHosts,
                                         uint32_t hostCount);

/**
 * Flecs system callback executed during the OnRecompose phase on the main thread.
 * Queries dirty hosts, executes LPT load balancing, awakens worker threads,
 * and waits at condition variable barrier for all workers to signal completion.
 *
 * @param it Flecs iterator. Non-NULL.
 */
void CelsDispatcherRecompositionRun(ecs_iter_t *it);

/**
 * Flecs system callback executed during the PostRecomposeSync phase on the main
 * thread. Sequentially merges all per-worker stages into the canonical world.
 *
 * @param it Flecs iterator. Non-NULL.
 */
void CelsDispatcherPostRecomposeSync(ecs_iter_t *it);

/* ========================================================================= */
/* Session & Root View API                                                   */
/* ========================================================================= */

/**
 * Root composition view function pointer returning a completed CEL_CompositionScope.
 */
typedef CEL_CompositionScope (*CelsCompositionScopeFn)(void);
typedef CelsCompositionScopeFn CelsRootViewFn;

/**
 * Configuration options for initializing a CelsSession.
 */
typedef struct CelsSessionConfig {
    uint32_t workerCount;                    // Number of worker threads (default: 4, max: 8)
    CelsCompositionScopeFn compositionScope; // Root composable view function returning CEL_CompositionScope
    size_t slabSize;                         // Slab byte size for root host (min: 4096, default: 4096)
    uint32_t maxGroups;                      // Max group capacity in slab (default: 32)
} CelsSessionConfig;

/**
 * Ergonomic session manager coordinating a developer-owned Flecs world,
 * worker thread pool, and root composition host.
 *
 * What is a Slab Size?
 * In Cels, each composition host owns an isolated CelsSlotTable carved from a
 * single, 64-byte cacheline-aligned memory "slab" of size `slabSize`.
 * The slab is partitioned into two adjoining buffers:
 *   1. Groups Buffer: maxGroups * sizeof(CelsSlotGroup) (32 bytes each)
 *   2. Slots Buffer:  (slabSize - maxGroups * 32) bytes (8-byte CelsSlotValue words)
 * This layout guarantees linear cache locality, zero runtime heap allocations during
 * recomposition, and optimal TLB/L1 cache page utilization (default: 4096 bytes / 4KB).
 */
typedef struct CelsSession {
    ecs_world_t *world;                      // Developer-owned canonical Flecs world
    CelsDispatcher dispatcher;               // Multi-threaded recomposition dispatcher
    CelsCompositionHost rootHost;            // Root composition host
    void *rootSlab;                          // 64-byte aligned memory slab for root host
    size_t slabSize;                         // Byte size of the allocated slab (default: 4096)
    ecs_entity_t rootEntity;                 // Flecs entity holding CelsCompositionHost
    CelsCompositionScopeFn compositionScope; // Root composition view function
} CelsSession;

/**
 * Initializes a CelsSession with a developer-owned Flecs world and configuration.
 * Automatically allocates a 64-byte aligned slab of size config->slabSize (default 4KB),
 * initializes the root host, registers the host entity in the world, and spawns the worker pool.
 *
 * @param session Target session struct. Non-NULL.
 * @param world   Developer-owned Flecs world. Non-NULL.
 * @param config  Session configuration parameters. Non-NULL.
 * @return CELS_OK or error code.
 */
CelsResult CelsSessionInit(CelsSession *session,
                           ecs_world_t *world,
                           const CelsSessionConfig *config);

/**
 * Destroys a CelsSession, shuts down worker threads, frees root host slab,
 * and removes the root host entity from the Flecs world.
 *
 * @param session Target session struct. NULL is safely accepted and does nothing.
 */
void CelsSessionDestroy(CelsSession *session);

/**
 * Flags the session's root composition view as dirty, causing it to recompose
 * during the next ecs_progress() call.
 *
 * @param session Target session struct.
 */
void CelsSessionMarkDirty(CelsSession *session);

/**
 * Checks whether the session's root composition view is currently flagged dirty.
 *
 * @param session Target session struct.
 * @return true if dirty, false otherwise.
 */
bool CelsSessionIsDirty(const CelsSession *session);

/**
 * Returns the Flecs entity handle associated with the session's root composition host.
 *
 * @param session Target session struct.
 * @return Flecs entity handle, or 0 if not initialized.
 */
ecs_entity_t CelsSessionGetRootEntity(const CelsSession *session);

/**
 * Returns the allocated slab size in bytes for the session's root composition host.
 *
 * @param session Target session struct.
 * @return Slab byte size (e.g. 4096), or 0 if session is NULL or uninitialized.
 */
size_t CelsSessionGetSlabSize(const CelsSession *session);

/**
 * Returns the active root CEL_CompositionScope for the session.
 *
 * @param session Target session struct.
 * @return Active CEL_CompositionScope containing root entity, key id, and group count.
 */
CEL_CompositionScope CelsSessionGetCompositionScope(const CelsSession *session);

/* ========================================================================= */
/* Compatibility Aliases (as requested in Prompt Specification)              */
/* ========================================================================= */

typedef CelsCompositionHost CompositionHost;
typedef CelsComposableFn ComposableFn;
typedef CelsWorkerSlice WorkerSlice;
typedef CelsWorkerThread WorkerThread;
typedef CelsDispatcher RecompositionDispatcher;

#define dispatcher_init(disp, world, thread_count) \
    CelsDispatcherInit((disp), (world), (thread_count))

#define dispatcher_destroy(disp) \
    CelsDispatcherDestroy(disp)

#define balance_workload_into_slices(disp, hosts, count) \
    CelsDispatcherBalanceWorkload((disp), (hosts), (count))

#define system_run_recomposition CelsDispatcherRecompositionRun
#define system_post_recompose_sync CelsDispatcherPostRecomposeSync

extern ecs_entity_t ecs_id(CelsCompositionHost);
#define FLECS_IDCompositionHostID_ FLECS_IDCelsCompositionHostID_
