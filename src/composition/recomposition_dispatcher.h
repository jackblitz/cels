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

/** Composables that can be queued for invalidation on one host per drain. */
#define CELS_HOST_INVALIDATION_CAPACITY 128u

/** Default bound on drain iterations within one CelsSessionRecompose call. */
#define CELS_DEFAULT_MAX_DRAIN_ITERATIONS 8u

/** Root Compositions that can be marked for destruction before one pass. */
#define CELS_LIFECYCLE_PENDING_CAPACITY 32u

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
struct CelsCompositionHost {
    uint8_t *slabMemory;                      // 8 bytes: 64-byte aligned 4KB slab
    CelsComposableFn rootFn;                  // 8 bytes: Root composable function
    CelsSlotTable slotTable;                  // 48 bytes: Dual-gap buffer table
    uint8_t props[CELS_HOST_PROPS_CAPACITY];  // 64 bytes: Inline user props buffer
    // Composables queued for invalidation, drained at the top of each
    // recompose iteration. The queue is the source of truth; the per-group
    // flags it sets are an accelerator for the walk.
    CelsComposableId invalidationQueue[CELS_HOST_INVALIDATION_CAPACITY];
    uint32_t invalidationCount;               // Entries used in invalidationQueue
    bool isDirty;                             // 1 byte: Whole-host recomposition flag
    uint8_t _padding[3];                      // 3 bytes: Explicit alignment padding
};

/* CelsTransactionContext is defined in slot_table.h. */

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
    CelsCompositionScopeFn compositionScope; // Root composable view function returning CEL_CompositionScope
    CelsTransactionContext transactionContext; // Optional mount/prune callbacks
    size_t slabSize;                         // Slab byte size for root host (min: 4096, default: 4096)
    uint32_t workerCount;                    // Number of worker threads (default: 4, max: 8)
    uint32_t maxGroups;                      // Max group capacity in slab (default: 32)
    // How many composables this session expects to hold at once. When non-zero
    // it derives slabSize and maxGroups, so neither has to be sized by hand:
    // each composable costs 128 bytes (32 structural + 96 slots), rounded up
    // to a 4KB page. slabSize/maxGroups above are then ignored.
    uint32_t maxComposables;
    // Bound on drain iterations inside one CelsSessionRecompose call
    // (default: CELS_DEFAULT_MAX_DRAIN_ITERATIONS). Exceeding it means
    // invalidation is not settling — almost always a cycle between two
    // composables that update cells the other watches.
    uint32_t maxDrainIterations;
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
    CelsTransactionContext transactionContext; // Mount/prune reporting callbacks
    // Root Composition keys marked for destruction, consumed as the very first
    // thing the next CelsSessionRecompose does.
    uint32_t pendingDestroy[CELS_LIFECYCLE_PENDING_CAPACITY];
    uint32_t pendingDestroyCount;            // Entries used in pendingDestroy
    uint32_t maxDrainIterations;             // Convergence bound for Recompose
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
 * Runs the recompose pass: drains invalidations, walks dirty hosts, and fires
 * onCreate/onDestroy inline as composables mount and prune.
 *
 * This is the only phase that runs anything. Everything outside it — a
 * cel_update, a CelsSessionMarkDirty — only records that work is owed. The call
 * is synchronous, sequential and single-threaded: callbacks fire on this thread,
 * at the moment each composable mounts or is pruned.
 *
 * Order within one iteration: drain the queue (setting per-group flags and
 * propagating them to the root), destroy any Composition marked via
 * CelsLifecycleMarkForDestroy, then walk the remaining dirty hosts. The
 * iteration repeats while invalidation is still outstanding, so a cel_update
 * issued from inside a body or an onCreate is picked up by the same call
 * rather than costing a frame.
 *
 * Returns immediately when nothing is dirty — the quiet path is one queue
 * check, not a tree walk.
 *
 * @param session Target session. Non-NULL.
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or
 *         CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE when the drain bound is hit.
 *         On non-convergence the queue is left intact and the next call
 *         resumes from it; nothing is lost and nothing is half-applied.
 */
CelsResult CelsSessionRecompose(CelsSession *session);

/**
 * Attaches or replaces the session's transaction context after init.
 *
 * @param session Target session. Non-NULL.
 * @param context Callbacks to attach. NULL clears any existing context.
 */
void CelsSessionSetTransactionContext(CelsSession *session,
                                      const CelsTransactionContext *context);

/**
 * Queues an invalidation for one composable on one host.
 *
 * Appends to the host's queue and marks the host dirty. Sets no flags and runs
 * nothing: propagation happens when the queue is drained, inside Recompose.
 * A full queue degrades to marking the whole host dirty rather than dropping
 * the invalidation — coarser, still correct.
 *
 * @param host       Host owning the composable. Non-NULL.
 * @param composable Logical group index to invalidate.
 * @return CELS_OK or CELS_ERROR_INVALID_ARGUMENT.
 */
CelsResult CelsCompositionHostInvalidate(CelsCompositionHost *host,
                                         CelsComposableId composable);

/**
 * Renumbers queued composable ids at or above a threshold by a delta.
 *
 * Queued ids are logical group indices captured before the walk, so a
 * structural change made during the walk renumbers what they refer to. An id
 * left stale invalidates whichever composable now holds that number. Same
 * failure as a stale parentIndex or a stale subscription, in the third and
 * last place an id is held outside the slot table.
 *
 * @param host      Host whose queue is renumbered. NULL is accepted and ignored.
 * @param threshold Lowest queued id affected by the renumbering.
 * @param delta     Amount to add to each affected id. Zero is a no-op.
 */
void CelsCompositionHostShiftInvalidations(CelsCompositionHost *host,
                                           CelsComposableId threshold,
                                           int32_t delta);

/**
 * Marks a root Composition for destruction by key.
 *
 * This is the only way to tear a Composition down. The mark is consumed at the
 * very start of the next CelsSessionRecompose, before anything is composed:
 * onDestroy fires once for the Composition's own root composable, and its body
 * never runs that pass.
 *
 * Callbacks do not cascade — a child gets its own onDestroy only when it
 * vanishes from an ordinary recomposition. Cleanup does cascade: the subtree's
 * groups are walked internally to unsubscribe their watch records, firing
 * nothing.
 *
 * @param session Target session. Non-NULL.
 * @param key     Key of the root Composition to destroy.
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or CELS_ERROR_CAPACITY_EXCEEDED
 *         if more than CELS_LIFECYCLE_PENDING_CAPACITY marks are pending.
 */
CelsResult CelsLifecycleMarkForDestroy(CelsSession *session, uint32_t key);

/**
 * Invalidates the session's root composable — the coarse escape hatch, for
 * when threading a cell through would be more trouble than recomposing the
 * whole tree. Same queue, same machinery, whole-tree granularity.
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
