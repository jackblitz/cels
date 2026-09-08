#pragma once

/**
 * @file session.h
 * @brief The Session — one lifecycle domain, and the recompose pass over it.
 *
 * A Session owns one 64-byte aligned memory slab and the diffing state carved
 * out of it. It never touches a world, a renderer, or a network layer; what a
 * composable *is* — its data, its rendering, its replication — is entirely the
 * caller's code, wired up through the transaction context's two callbacks.
 *
 * Typical usage:
 * @code
 *     CelsSession session;
 *     const CelsSessionConfig config = {
 *         .compositionScope = ArenaView,
 *         .maxComposables = 128,
 *         .transactionContext = {
 *             .onCreate = OnComposableCreated,
 *             .onDestroy = OnComposableDestroyed,
 *         },
 *     };
 *     if (CelsSessionInit(&session, &config) != CELS_OK) {
 *         return 1;
 *     }
 *
 *     while (running) {
 *         if (CelsSessionRecompose(&session) != CELS_OK) {
 *             LogRecompositionCycle();
 *         }
 *     }
 *     CelsSessionDestroy(&session);
 * @endcode
 *
 * Nothing here runs on its own. CelsSessionRecompose is the only phase that
 * executes anything; a cel_update or a CelsSessionMarkDirty merely records that
 * work is owed. Diffing, mounting, pruning and the onCreate/onDestroy callbacks
 * all happen synchronously and inline within that one call.
 *
 * Thread safety: none, by design. Recomposition is a sequential walk that gains
 * nothing from concurrency, and firing callbacks inline on the calling thread
 * is what removes every cross-thread hazard from the model. cel_update appends
 * to a host's invalidation queue without locking, so it must be called on the
 * same thread that calls CelsSessionRecompose; marshal across yourself if your
 * event source lives elsewhere.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "composition/composer.h"
#include "composition/slottable/slot_table.h"

/** Inline user props carried by a composition host. */
#define CELS_HOST_PROPS_CAPACITY 64u

/** Composables that can be queued for invalidation on one host per drain. */
#define CELS_HOST_INVALIDATION_CAPACITY 128u

/** Default bound on drain iterations within one CelsSessionRecompose call. */
#define CELS_DEFAULT_MAX_DRAIN_ITERATIONS 8u

/** Root Compositions that can be marked for destruction before one pass. */
#define CELS_LIFECYCLE_PENDING_CAPACITY 32u

/** Slab bytes per composable: 32 structural + a 96-byte slot budget. */
#define CELS_BYTES_PER_COMPOSABLE 128u

/** Slab sizes round up to this. */
#define CELS_SLAB_PAGE_SIZE 4096u

/**
 * Root composable function pointer.
 *
 * @param cmp   Active transient composer for the host. Non-NULL.
 * @param props Pointer to the user props buffer stored in the host. Non-NULL.
 */
typedef void (*CelsComposableFn)(CelsComposer *cmp, void *props);

/**
 * Composition host: an isolated SlotTable over one slab, its root composable,
 * and the invalidation state the recompose pass consumes.
 * Fields ordered largest to smallest to eliminate internal padding.
 */
struct CelsCompositionHost {
    uint8_t *slabMemory;                      // 64-byte aligned slab
    CelsComposableFn rootFn;                  // Root composable function
    CelsSlotTable slotTable;                  // Dual-gap buffer table
    uint8_t props[CELS_HOST_PROPS_CAPACITY];  // Inline user props buffer
    // Composables queued for invalidation, drained at the top of each
    // recompose iteration. The queue is the source of truth; the per-group
    // flags it sets are an accelerator for the walk.
    CelsComposableId invalidationQueue[CELS_HOST_INVALIDATION_CAPACITY];
    uint32_t invalidationCount;               // Entries used in invalidationQueue
    bool isDirty;                             // Whole-host recomposition flag
    uint8_t _padding[3];                      // Explicit alignment padding
};

/**
 * Root composition view function pointer, returning a completed scope.
 */
typedef CEL_CompositionScope (*CelsCompositionScopeFn)(void);
typedef CelsCompositionScopeFn CelsRootViewFn;

/**
 * Configuration for initializing a CelsSession.
 *
 * All-zero is not a usable configuration — compositionScope must be set for the
 * session to own a slab — but every other field has a meaningful zero.
 */
typedef struct CelsSessionConfig {
    CelsCompositionScopeFn compositionScope;   // Root composable view function
    CelsTransactionContext transactionContext; // Optional mount/prune callbacks
    size_t slabSize;                           // Slab bytes (min/default 4096)
    uint32_t maxGroups;                        // Group capacity (default 32)
    // How many composables this session expects to hold at once. When non-zero
    // it derives slabSize and maxGroups, so neither has to be sized by hand:
    // each composable costs 128 bytes, rounded up to a 4KB page. slabSize and
    // maxGroups above are then ignored.
    uint32_t maxComposables;
    // Bound on drain iterations inside one CelsSessionRecompose call
    // (default: CELS_DEFAULT_MAX_DRAIN_ITERATIONS). Exceeding it means
    // invalidation is not settling — almost always a cycle between two
    // composables that update cells the other watches.
    uint32_t maxDrainIterations;
} CelsSessionConfig;

/**
 * One lifecycle domain: a slab, a root composition host, and the pending work
 * against it.
 */
typedef struct CelsSession {
    CelsCompositionHost rootHost;              // Root composition host
    void *rootSlab;                            // 64-byte aligned slab
    size_t slabSize;                           // Byte size of the slab
    CelsCompositionScopeFn compositionScope;   // Root composition view function
    CelsTransactionContext transactionContext; // Mount/prune reporting callbacks
    // Root Composition keys marked for destruction, consumed as the very first
    // thing the next CelsSessionRecompose does.
    uint32_t pendingDestroy[CELS_LIFECYCLE_PENDING_CAPACITY];
    uint32_t pendingDestroyCount;              // Entries used in pendingDestroy
    uint32_t maxDrainIterations;               // Convergence bound for Recompose
} CelsSession;

/* ========================================================================= */
/* Composition Host API                                                      */
/* ========================================================================= */

/**
 * Initializes a composition host over a caller-provided memory slab.
 *
 * @param host       Target composition host. Non-NULL.
 * @param slabMemory 64-byte aligned slab of at least CELS_SLAB_PAGE_SIZE bytes.
 *                   Non-NULL, and must outlive the host.
 * @param slabSize   Total byte size of slabMemory.
 * @param maxGroups  Group capacity to carve out of the slab.
 * @param rootFn     Root composable function pointer. Non-NULL.
 * @return CELS_OK or CELS_ERROR_INVALID_ARGUMENT.
 */
CelsResult CelsCompositionHostInit(CelsCompositionHost *host,
                                   void *slabMemory,
                                   size_t slabSize,
                                   uint32_t maxGroups,
                                   CelsComposableFn rootFn);

/**
 * Flags a composition host dirty or clean.
 *
 * @param host  Target host. NULL is accepted and does nothing.
 * @param dirty Dirty state to set.
 */
void CelsCompositionHostSetDirty(CelsCompositionHost *host, bool dirty);

/**
 * Reports whether a composition host is flagged dirty.
 *
 * @param host Target host. NULL reads as not dirty.
 * @return true if dirty.
 */
bool CelsCompositionHostIsDirty(const CelsCompositionHost *host);

/**
 * Queues an invalidation for one composable on one host.
 *
 * Appends to the host's queue and marks the host dirty. Sets no flags and runs
 * nothing: propagation happens when the queue is drained, inside Recompose.
 * A full queue degrades to marking the whole host dirty rather than dropping
 * the invalidation — coarser, still correct. Passing
 * CELS_COMPOSABLE_ID_INVALID means "the whole host" and skips the queue.
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

/* ========================================================================= */
/* Session API                                                               */
/* ========================================================================= */

/**
 * Initializes a session, allocating its slab and registering the root host.
 *
 * When config->maxComposables is non-zero it derives the slab geometry and
 * config->slabSize / config->maxGroups are ignored.
 *
 * @param session Target session. Non-NULL.
 * @param config  Configuration. Non-NULL. A NULL compositionScope yields a
 *                session that owns no slab and composes nothing.
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or CELS_ERROR_OUT_OF_MEMORY.
 */
CelsResult CelsSessionInit(CelsSession *session,
                           const CelsSessionConfig *config);

/**
 * Destroys a session and frees its slab.
 *
 * Fires no per-composable callbacks. It does walk the groups internally to drop
 * their watch records, because cells outlive sessions and a surviving
 * subscription would hold a pointer into freed memory.
 *
 * @param session Target session. NULL is accepted and does nothing.
 */
void CelsSessionDestroy(CelsSession *session);

/**
 * Runs the recompose pass: drains invalidations, walks dirty hosts, and fires
 * onCreate/onDestroy inline as composables mount and prune.
 *
 * This is the only phase that runs anything. The call is synchronous and
 * sequential: callbacks fire on this thread, at the moment each composable
 * mounts or is pruned.
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
 * Invalidates the session's root composable — the coarse escape hatch, for
 * when threading a cell through would be more trouble than recomposing the
 * whole tree. Same queue, same machinery, whole-tree granularity.
 *
 * @param session Target session. NULL is accepted and does nothing.
 */
void CelsSessionMarkDirty(CelsSession *session);

/**
 * Reports whether recomposition is pending.
 *
 * @param session Target session. NULL reads as not dirty.
 * @return true if the host is flagged, the invalidation queue is non-empty, or
 *         a Composition is marked for destruction.
 */
bool CelsSessionIsDirty(const CelsSession *session);

/**
 * Returns the byte size of the session's slab, or 0 if it owns none.
 *
 * @param session Target session. NULL reads as 0.
 */
size_t CelsSessionGetSlabSize(const CelsSession *session);

/**
 * Inspects the root composable's identity and live composable count.
 *
 * @param session Target session. NULL yields a zeroed scope.
 * @return The root composition scope; zeroed if nothing has composed.
 */
CEL_CompositionScope CelsSessionGetCompositionScope(const CelsSession *session);

/* ========================================================================= */
/* Lifecycle                                                                 */
/* ========================================================================= */

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
