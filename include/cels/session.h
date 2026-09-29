#pragma once

/**
 * @file session.h
 * @brief Session coordinator, recomposition loop, and lifecycle state management.
 *
 * Typical usage:
 * @code
 *     CelsSession session;
 *     CelsSessionInit(&session, &(CelsSessionConfig){
 *         .root = MyRootApp
 *     });
 *
 *     CelsSessionRecompose(&session);
 *     CelsSessionDestroy(&session);
 * @endcode
 *
 * Thread safety: CelsSession is single-threaded by design. Recomposition walks
 * execute on the calling thread. Mutating state across threads requires
 * marshaling mutations to the session thread.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cels/slot_table.h"
#include "cels/state.h"
#include "cels/transaction.h"

#ifndef CELS_MAX_DEPTH
#define CELS_MAX_DEPTH 64u
#endif

#ifndef CELS_MAX_GROUPS
#define CELS_MAX_GROUPS 4096u
#endif

#ifndef CELS_DATA_ARENA_SIZE
#define CELS_DATA_ARENA_SIZE 32768u
#endif

#ifndef CELS_DEFAULT_SLAB_SIZE
#define CELS_SLAB_16K   (16u * 1024u)
#define CELS_SLAB_32K   (32u * 1024u)
#define CELS_SLAB_48K   (48u * 1024u)
#define CELS_SLAB_64K   (64u * 1024u)
#define CELS_SLAB_128K  (128u * 1024u)
#define CELS_SLAB_256K  (256u * 1024u)
#define CELS_SLAB_512K  (512u * 1024u)
#define CELS_SLAB_1M    (1024u * 1024u)
#define CELS_DEFAULT_SLAB_SIZE CELS_SLAB_512K
#endif

#ifndef CEL_SLAB
#if defined(_MSC_VER)
#define CEL_SLAB(name, size) __declspec(align(64)) uint8_t name[size]
#else
#define CEL_SLAB(name, size) __attribute__((aligned(64))) uint8_t name[size]
#endif
#endif

#ifndef CELS_MAX_CLEANUPS
#define CELS_MAX_CLEANUPS 4096u
#endif

#ifndef CELS_MAX_DRAIN_ITERATIONS
#define CELS_MAX_DRAIN_ITERATIONS 8u
#endif

#define CELS_SLOT_ALIGNMENT 8u
#define CELS_ALIGN_UP(size) \
    (((size) + (CELS_SLOT_ALIGNMENT - 1u)) & ~(CELS_SLOT_ALIGNMENT - 1u))

#ifndef CELS_MAX_SLOTS
#define CELS_MAX_SLOTS 4096u
#endif

#define CELS_FLAG_NONE 0u
#define CELS_FLAG_INVALIDATED (1u << 0)
#define CELS_FLAG_CONTAINS_INVALIDATED (1u << 1)
#define CELS_FLAG_FRESH_MOUNT (1u << 2)

#define CELS_SESSION_MAGIC 0x53455353u /* 'SESS' */

/* Forward declarations */
struct CelsEngine;
typedef struct CelsEngine CelsEngine;
typedef struct CelsEngine CelsApp;
#ifndef CELS_SESSION_TYPEDEF_DEFINED
#define CELS_SESSION_TYPEDEF_DEFINED
typedef struct CelsSession CelsSession;
typedef struct CelsSession CEL_Session;
#endif

/**
 * Options for initializing a CEL_Session.
 */
typedef struct CelSessionOptions {
    size_t slabCapacityBytes; /**< Total slab capacity in bytes (e.g. CELS_SLAB_64K). Defaults to 512 KiB if 0. */
    void  *allocator;         /**< Reserved for custom allocator hook (or NULL for default aligned slab). */
} CelSessionOptions;

/**
 * Backward-compatible session configuration.
 */
typedef struct CelsSessionConfig {
    size_t     slabSize;           /**< Slab size in bytes. */
    void      *slab;               /**< Optional user-provided 64-byte aligned buffer. */
    uint32_t   maxGroups;          /**< Optional max groups. */
    uint32_t   maxDrainIterations; /**< Maximum recomposition drain passes. */
    struct CelsEngine *engine;     /**< Optional host engine. */
} CelsSessionConfig;

/**
 * Cleanup hook tracking an active lifecycle state instance.
 */
typedef struct CelsCleanupHook {
    uint64_t groupKey;
    uint32_t groupId;
    void *instance;
    void (*onDestroy)(void *instance, CelsSession *session);
} CelsCleanupHook;

/**
 * Stable arena slot allocation record. Pointers remain valid across edits.
 */
typedef struct CelsSlotAllocation {
    uint32_t groupId;
    uint32_t slotOffset;
    uint32_t arenaOffset;
    uint16_t size;         /**< Aligned allocation size in data arena */
    uint16_t userSize;     /**< Exact requested user type size for schema evolution check */
} CelsSlotAllocation;

#define CELS_MAX_ATTACHED_COMPOSITIONS 16u

typedef struct CelsAttachedComposition {
    CEL_Id key;
    void (*body)(void *userData);
    void *userData;
    bool (*lifecycleEval)(void *evalCtx);
    void *evalCtx;
    bool isAttached;
} CelsAttachedComposition;

#ifndef CELS_MAX_MODULES
#define CELS_MAX_MODULES 16u
#endif

#ifndef CELS_MODULE_BINDING_DEFINED
#define CELS_MODULE_BINDING_DEFINED
/**
 * Record tracking an active engine subsystem module registered with the application or session.
 */
typedef struct CelsModuleBinding {
    uint64_t key;          /**< 64-bit FNV-1a hash of module type name */
    const char *name;      /**< Human-readable name for diagnostics */
    void *instance;        /**< Pointer to developer's module struct */
    void (*onDestroy)(void *instance); /**< Optional cleanup callback */
} CelsModuleBinding;
#endif

/**
 * Primary session orchestrating composition, traversal, and reactive state.
 */
struct CelsSession {
    uint32_t magic;        /**< CELS_SESSION_MAGIC validation tag */
    CEL_Id   id;           /**< Unique 64-bit session identifier */
    bool hasComposedOnce;
    bool isRecomposing;
    bool isHotReloadPending;
    bool isHeapAllocated;

    uint32_t currentDepth;
    uint32_t currentGroupIndex;
    uint32_t currentSlotOffset;
    uint32_t logicalCursor;

    uint8_t activeStack[CELS_MAX_DEPTH];
    uint32_t groupIndexStack[CELS_MAX_DEPTH];
    uint32_t oldGroupSizeStack[CELS_MAX_DEPTH];
    uint32_t slotOffsetStack[CELS_MAX_DEPTH];

    /* Slab storage */
    void *slab;
    size_t slabSize;
    bool ownsSlab;

    /* Structural groups gap buffer (carved from slab) */
    CelsSlotGroup *groups;
    uint32_t maxGroups;
    uint32_t groupsGapStart;
    uint32_t groupsGapEnd;

    /* Slot allocation table (carved from slab) */
    CelsSlotAllocation *slots;
    uint32_t maxSlots;
    uint32_t slotCount;
    uint32_t nextGroupId;

    /* Nonmoving slot arena (carved from slab): remembered pointers stay pinned */
    uint8_t *dataArena;
    size_t dataArenaSize;
    uint32_t dataGapStart;
    uint32_t dataGapEnd;

    /* Reactive state registry (double-buffered) */
    CelsStateRegistry stateRegistry;

    /* Lifecycle state cleanups */
    CelsCleanupHook cleanups[CELS_MAX_CLEANUPS];
    uint32_t cleanupCount;

    /* Invalidation queue */
    uint64_t invalidationQueue[CELS_MAX_QUEUE];
    uint32_t queueCount;

    /* Next frame invalidation queue (for tasks and delayed yields) */
    uint64_t nextFrameQueue[CELS_MAX_QUEUE];
    uint32_t nextFrameQueueCount;

    uint32_t maxDrainIterations;

    /* Attached Compositions */
    CelsAttachedComposition attachedCompositions[CELS_MAX_ATTACHED_COMPOSITIONS];
    uint32_t attachedCount;

    /* Owning Host Engine Pointer & Fallback Module Storage */
    struct CelsEngine *engine;
    CelsModuleBinding fallbackModules[CELS_MAX_MODULES];
    uint32_t fallbackModuleCount;

    /* Double-buffered transaction batches (for lockless cross-thread bridge) */
    CelsTransactionBatch transactionBatches[2];
    uint32_t activeBatchIndex;

    /* Generic user context pointer */
    void *userData;

    /* Post-recomposition frame completion hook */
    void (*postRecomposeHook)(struct CelsSession *session, void *userData);
    void *postRecomposeUserData;
};

typedef void (*CelsPostRecomposeFn)(CelsSession *session, void *userData);

/* ========================================================================= */
/* Keyed Session Creation & Registry                                         */
/* ========================================================================= */

/**
 * Creates and registers a session by its unique 64-bit ID.
 *
 * @param sessionId Unique 64-bit ID (e.g. CEL_ID("SESSION_MAIN")).
 * @param options   Memory slab and capacity configuration.
 * @return Pointer to created CEL_Session.
 */
CEL_Session *CelSessionCreate(CEL_Id sessionId, const CelSessionOptions *options);

/**
 * Retrieves a registered session by its unique ID.
 *
 * @param sessionId Unique 64-bit ID.
 * @return Pointer to resolved CEL_Session, or NULL if not found.
 */
CEL_Session *cel_session(CEL_Id sessionId);

/**
 * Returns the currently active ambient CEL_Session bound to the executing thread.
 *
 * @return Pointer to active CEL_Session, or NULL outside a session tick.
 */
CEL_Session *cel_active_session(void);

/**
 * Allocates aligned memory from the session's nonmoving data arena.
 *
 * @param s    Target session. Non-NULL.
 * @param size Byte size to allocate.
 * @return Pointer to aligned arena memory, or NULL on overflow.
 */
void *CelsSessionAllocData(CelsSession *s, size_t size);

/**
 * Remembers addressable reactive state in the session's cache-aligned slab.
 *
 * @param s          Target session. Non-NULL.
 * @param id         Unique 64-bit state identifier.
 * @param size       Size in bytes of state struct.
 * @param defaultVal Default values to seed on initial mount.
 * @return Pointer to persistent state in session slab.
 */
void *CelsSessionRememberState(CEL_Session *s, CEL_Id id, size_t size, const void *defaultVal);

/* ========================================================================= */
/* Attached Composition Functions                                            */
/* ========================================================================= */

void CelsSessionAttachComposition(CEL_Session *s,
                                  CEL_Id key,
                                  void (*body)(void *userData),
                                  void *userData,
                                  bool (*eval)(void *evalCtx),
                                  void *evalCtx);

void CelsSessionDetachComposition(CEL_Session *s, CEL_Id key);

void CelsSessionRegisterLifecycle(CEL_Session *s,
                                  void *instance,
                                  void (*onCreate)(void *instance, CEL_Session *s),
                                  void (*onDestroy)(void *instance, CEL_Session *s));

void CelsSessionUpdateLifecycle(CEL_Session *s,
                                void *instance,
                                void (*onDestroy)(void *instance, CEL_Session *s));

/* ========================================================================= */
/* Session Lifecycle Functions                                               */
/* ========================================================================= */

/**
 * Initializes a session with configuration options.
 *
 * @param session Target session. Non-NULL.
 * @param config  Configuration options, or NULL for defaults.
 */
void CelsSessionInit(CelsSession *session, const CelsSessionConfig *config);

/**
 * Tears down a session, releasing all active lifecycle cleanups and slab memory.
 *
 * @param session Target session. NULL is safely ignored.
 */
void CelsSessionDestroy(CelsSession *session);
void CelSessionDestroy(CEL_Session *session);

/**
 * Executes a recomposition pass over the session tree.
 *
 * @param session Target session. Non-NULL.
 * @return CELS_OK or error code.
 */
CelsResult CelsSessionRecompose(CelsSession *session);
CelsResult CelSessionRecompose(CEL_Session *session);

/**
 * Recomposes all registered active sessions.
 *
 * @return CELS_OK on success, or the last encountered error code.
 */
CelsResult CelsRecomposeAllSessions(void);
#define cel_recompose_all CelsRecomposeAllSessions

/**
 * Flags all mounted composition groups for re-evaluation on the next recompose pass.
 *
 * @param session Target session. Non-NULL.
 */
void CelsSessionHotReload(CelsSession *session);

/**
 * Registers an engine subsystem module with the session.
 */
void CelsSessionRegisterModule(CelsSession *session,
                              uint64_t key,
                              const char *name,
                              void *instance,
                              void (*onReload)(void *instance, struct CelsSession *session),
                              void (*onDestroy)(void *instance));

/**
 * Retrieves a registered subsystem module pointer by its 64-bit key.
 */
void *CelsSessionGetModule(const CelsSession *session, uint64_t key);

/**
 * Returns the currently active ambient session for the calling thread.
 */
CelsSession *CelsGetCurrentSession(void);

/**
 * Sets the active ambient session for the calling thread.
 */
void CelsSetCurrentSession(CelsSession *session);

/* ========================================================================= */
/* Traversal & Tree Manipulation API                                         */
/* ========================================================================= */

bool CelsEnterComposition(CelsSession *session, uint64_t rootKey);
bool CelsEnterComposable(CelsSession *session, uint64_t key);
void CelsExitGroup(CelsSession *session);
void CelsPruneSubtree(CelsSession *session, uint32_t rootLogicalIndex);
void CelsPruneSubtreeByKey(CelsSession *session, uint64_t key);

/**
 * Allocates or resolves persistent slot memory in the session's arena.
 */
void *CelsResolveSlot(CelsSession *session,
                      size_t size,
                      const void *initVal);

/**
 * Finds an active state instance by its key in the session.
 */
void *CelsGetState(CelsSession *session, uint64_t key);

/**
 * Queues a 64-bit group key for invalidation and recomposition on the next pass.
 */
void CelsSessionInvalidateKey(CelsSession *session, uint64_t key);

/* ========================================================================= */
/* Session Transaction API & Cross-Thread Staging Buffer                     */
/* ========================================================================= */

/**
 * Stages a component/data set operation in the session's active transaction batch.
 */
bool CelsSessionStageSet(CelsSession *session,
                         uint64_t targetId,
                         uint64_t typeKey,
                         size_t size,
                         const void *data);

/**
 * Stages a component removal operation in the session's active transaction batch.
 */
bool CelsSessionStageRemove(CelsSession *session,
                            uint64_t targetId,
                            uint64_t typeKey);

/**
 * Stages a target deletion/destruction in the session's active transaction batch.
 */
bool CelsSessionStageDelete(CelsSession *session, uint64_t targetId);

/**
 * Stages a custom opcode transaction in the session's active transaction batch.
 */
bool CelsSessionStageCustom(CelsSession *session,
                            uint32_t opCode,
                            uint64_t targetId,
                            uint64_t typeKey,
                            size_t size,
                            const void *data);

/**
 * Commits the current active transaction batch using the supplied handler callback.
 * Clears the active batch upon completion.
 *
 * @return Number of operations executed.
 */
uint32_t CelsSessionCommitTransactions(CelsSession *session,
                                       CelsTransactionHandler handler,
                                       void *userData);

/**
 * Swaps active and ready transaction batches for lockless handoff to worker threads.
 */
void CelsSessionSwapTransactionBatches(CelsSession *session);

/**
 * Returns pointer to the ready transaction batch (for consumption by worker threads).
 */
const CelsTransactionBatch *CelsSessionGetReadyBatch(const CelsSession *session);

/**
 * Resets the ready transaction batch after worker threads finish processing.
 */
void CelsSessionClearReadyBatch(CelsSession *session);

/**
 * Gets the session's user context pointer.
 */
void *CelsSessionGetUserData(const CelsSession *session);

/**
 * Sets the session's user context pointer.
 */
void CelsSessionSetUserData(CelsSession *session, void *userData);

/**
 * Registers a callback invoked immediately after CelsSessionRecompose finishes frame convergence.
 */
void CelsSessionSetPostRecomposeHook(CelsSession *session,
                                     CelsPostRecomposeFn hook,
                                     void *userData);

/**
 * Allocates or resolves persistent slot memory with an optional unmount cleanup callback.
 * If onDestroy is NULL, behaves identically to CelsResolveSlot.
 */
void *CelsResolveSlotWithCleanup(CelsSession *session,
                                 size_t size,
                                 const void *initVal,
                                 void (*onDestroy)(void *ptr, CelsSession *session));

/* ========================================================================= */
/* Infallible Inline Accessors                                                */
/* ========================================================================= */

static inline uint32_t CelsGetLogicalGroupCount(const CelsSession *s)
{
    return s->maxGroups - (s->groupsGapEnd - s->groupsGapStart);
}

static inline uint32_t CelsGroupLogicalToPhysical(const CelsSession *s,
                                                  uint32_t logical)
{
    return (logical < s->groupsGapStart)
        ? logical
        : logical + (s->groupsGapEnd - s->groupsGapStart);
}

static inline CelsSlotGroup *CelsGetGroup(CelsSession *s, uint32_t logical)
{
    return &s->groups[CelsGroupLogicalToPhysical(s, logical)];
}

static inline bool CelsIsFreshMount(CelsSession *s)
{
    if (s == NULL || s->currentDepth == 0) {
        return false;
    }
    return (CelsGetGroup(s, s->currentGroupIndex)->flags
            & CELS_FLAG_FRESH_MOUNT) != 0;
}

static inline size_t CelsGetSlabSize(const CelsSession *s)
{
    return s ? s->slabSize : 0;
}

static inline uint32_t CelsGetMaxGroups(const CelsSession *s)
{
    return s ? s->maxGroups : 0;
}

static inline uint32_t CelsGetCleanupCount(const CelsSession *s)
{
    return s ? s->cleanupCount : 0;
}

static inline uint32_t CelsGetMaxCleanups(void)
{
    return CELS_MAX_CLEANUPS;
}

