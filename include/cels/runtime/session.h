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

#include "cels/runtime/slot_table.h"
#include "cels/runtime/state.h"
#include "cels/runtime/transaction.h"
#include "cels/runtime/event.h"

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
struct CelsAppDef;
#ifndef CELS_APP_DEF_TYPEDEF_DEFINED
#define CELS_APP_DEF_TYPEDEF_DEFINED
typedef struct CelsAppDef CelsAppDef;
typedef struct CelsAppDef CelsAppManifest;
#endif
struct CelsApp;
#ifndef CELS_APP_TYPEDEF_DEFINED
#define CELS_APP_TYPEDEF_DEFINED
typedef struct CelsApp CelsApp;
#endif
#ifndef CELS_SESSION_TYPEDEF_DEFINED
#define CELS_SESSION_TYPEDEF_DEFINED
typedef struct CelsSession CelsSession;
typedef struct CelsSession CEL_Session;
#endif

/**
 * @enum CelsSessionProfile
 * @brief High-level workload capacity profiles for session memory sizing.
 *
 * Defines the target scale of the session composition tree. CELS automatically
 * dimensions the cache-aligned slab, slot table gap buffer, descriptor index,
 * and data arena based on the selected profile:
 *
 * | Profile              | Capacity          | Slab Size | Best Used For |
 * |----------------------|-------------------|-----------|---------------|
 * | CELS_PROFILE_DEFAULT | 4,096 composables | 512 KiB   | Default standard capacity if unspecified |
 * | CELS_PROFILE_128     | 128 composables   | 16 KiB    | Micro-dialogs, tiny popups, embedded widgets |
 * | CELS_PROFILE_256     | 256 composables   | 32 KiB    | HUD overlays, tooltips, sub-panels |
 * | CELS_PROFILE_512     | 512 composables   | 64 KiB    | L1/L2 cache-resident UI panels & focused subtrees |
 * | CELS_PROFILE_1K      | 1,024 composables | 128 KiB   | Standard application windows & forms |
 * | CELS_PROFILE_2K      | 2,048 composables | 256 KiB   | Complex screens with many active lists & controls |
 * | CELS_PROFILE_4K      | 4,096 composables | 512 KiB   | Broad applications with multiple concurrent modules |
 * | CELS_PROFILE_8K      | 8,192 composables | 1 MiB     | Heavy simulation trees & large hierarchies |
 */
typedef enum CelsSessionProfile {
    CELS_PROFILE_DEFAULT = 0,    /**< Default capacity (4,096 composables, 512 KiB) */
    CELS_PROFILE_128     = 1,    /**< Up to 128 composables (16 KiB slab) - Micro-dialogs, tiny popups, embedded widgets */
    CELS_PROFILE_256     = 2,    /**< Up to 256 composables (32 KiB slab) - HUD overlays, tooltips, sub-panels */
    CELS_PROFILE_512     = 3,    /**< Up to 512 composables (64 KiB slab) - L1/L2 cache-resident UI panels */
    CELS_PROFILE_1K      = 4,    /**< Up to 1,024 composables (128 KiB slab) - Standard application windows */
    CELS_PROFILE_2K      = 5,    /**< Up to 2,048 composables (256 KiB slab) - Complex screens with many active lists */
    CELS_PROFILE_4K      = 6,    /**< Up to 4,096 composables (512 KiB slab) - Broad applications with multiple modules */
    CELS_PROFILE_8K      = 7,    /**< Up to 8,192 composables (1 MiB slab) - Heavy simulation trees & large hierarchies */
} CelsSessionProfile;

/**
 * Options for initializing a CEL_Session.
 */
typedef struct CelSessionOptions {
    CelsSessionProfile profile;              /**< Target workload capacity profile (e.g. CELS_PROFILE_1K). */
    uint32_t           maxComposables;       /**< Estimated or exact composable count (auto-sizes optimal slab). */
    size_t             slabCapacityBytes;    /**< Total slab capacity in bytes (optional raw override). */
    void              *allocator;            /**< Reserved for custom allocator hook (or NULL for default aligned slab). */
} CelSessionOptions;

/**
 * Session configuration options.
 */
typedef struct CelsSessionConfig {
    CelsSessionProfile profile;              /**< Workload capacity profile (e.g. CELS_PROFILE_1K, CELS_PROFILE_512). */
    uint32_t           maxComposables;       /**< Estimated or exact composable count (auto-sizes optimal slab). */
    size_t             slabSize;             /**< Slab size in bytes (optional raw override). */
    void              *slab;                 /**< Optional user-provided 64-byte aligned buffer (zero-allocation). */
    uint32_t           maxGroups;            /**< Alias / fallback for maxComposables. */
    uint32_t           maxDrainIterations;   /**< Maximum recomposition drain passes. */
    struct CelsEngine *engine;               /**< Optional host engine. */
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
    bool isExecutingTask;
    bool isHandlingEvent;
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

    /* Discrete event queue (tree events, targeted signals, and global broadcasts) */
    CelsEventQueue eventQueue;

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

/**
 * Attaches a root composition to the session with optional lifecycle evaluation.
 *
 * @param s        Target session. Non-NULL.
 * @param key      Unique 64-bit composition identifier.
 * @param body     Function pointer to root composition procedure. Non-NULL.
 * @param userData Context pointer passed to body during composition. Can be NULL.
 * @param eval     Optional evaluation predicate. When returning false, prunes subtree. Can be NULL.
 * @param evalCtx  Context pointer passed to eval predicate. Can be NULL.
 */
void CelsSessionAttachComposition(CEL_Session *s,
                                  CEL_Id key,
                                  void (*body)(void *userData),
                                  void *userData,
                                  bool (*eval)(void *evalCtx),
                                  void *evalCtx);

/**
 * Detaches an attached composition from the session by its key.
 *
 * @param s   Target session. Safe if NULL.
 * @param key Unique 64-bit composition identifier.
 */
void CelsSessionDetachComposition(CEL_Session *s, CEL_Id key);

/**
 * Registers lifecycle creation and destruction callbacks for an active group node.
 *
 * Invokes onCreate immediately if non-NULL. When the group node is pruned or unmounted,
 * onDestroy will be invoked in reverse mounting order.
 *
 * @param s         Target session. Non-NULL.
 * @param instance  User resource handle or object pointer. Can be NULL.
 * @param onCreate  Optional setup callback invoked on mount. Safe if NULL.
 * @param onDestroy Destructor callback invoked on unmount or session destruction. Safe if NULL.
 */
void CelsSessionRegisterLifecycle(CEL_Session *s,
                                  void *instance,
                                  void (*onCreate)(void *instance, CEL_Session *s),
                                  void (*onDestroy)(void *instance, CEL_Session *s));

/**
 * Updates lifecycle callback pointers for a remounted node without re-running onCreate.
 *
 * Used after hot-reload or remount to refresh code pointers to unmount destructors.
 *
 * @param s         Target session. Non-NULL.
 * @param instance  User resource handle or object pointer.
 * @param onDestroy Refreshed destructor callback pointer. Safe if NULL.
 */
void CelsSessionUpdateLifecycle(CEL_Session *s,
                                void *instance,
                                void (*onDestroy)(void *instance, CEL_Session *s));

/* ========================================================================= */
/* Session Lifecycle Functions                                               */
/* ========================================================================= */

/**
 * Returns the raw slab memory size in bytes corresponding to a CelsSessionProfile.
 *
 * @param profile Target workload capacity profile.
 * @return Slab byte size (e.g. 64 KiB for CELS_PROFILE_512).
 */
size_t CelsSlabSizeFromProfile(CelsSessionProfile profile);

/**
 * Returns the maximum composable group capacity corresponding to a CelsSessionProfile.
 *
 * @param profile Target workload capacity profile.
 * @return Maximum active composables (e.g. 512 for CELS_PROFILE_512).
 */
uint32_t CelsMaxComposablesFromProfile(CelsSessionProfile profile);

/**
 * Constructs a CelsSessionConfig initialized with a named workload profile.
 *
 * @param profile Workload capacity profile (e.g. CELS_PROFILE_1K).
 * @return Initialized configuration struct.
 */
CelsSessionConfig CelsSessionProfileConfig(CelsSessionProfile profile);

/**
 * Constructs a CelsSessionConfig initialized with an estimated composable count.
 *
 * @param maxComposables Estimated number of active composables.
 * @return Initialized configuration struct with auto-sized slab.
 */
CelsSessionConfig CelsSessionCapacityConfig(uint32_t maxComposables);

/**
 * Initializes a session directly with a named workload profile.
 *
 * @param session Target session. Non-NULL.
 * @param profile Workload capacity profile (e.g. CELS_PROFILE_1K).
 */
void CelsSessionInitWithProfile(CelsSession *session, CelsSessionProfile profile);

/**
 * Creates and registers a session by its unique ID using a named profile.
 *
 * @param sessionId Unique 64-bit session ID.
 * @param profile   Workload capacity profile.
 * @return Pointer to created CEL_Session.
 */
CEL_Session *CelSessionCreateWithProfile(CEL_Id sessionId, CelsSessionProfile profile);

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
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsSessionRecompose(CelsSession *session);
CelsResult CelSessionRecompose(CEL_Session *session);

/**
 * Checks whether a session requires recomposition (dirty state, signals, invalidations, or initial pass).
 *
 * @param session Target session. Safe if NULL.
 * @return True if session needs a recomposition pass; false if completely idle and clean.
 */
bool CelsSessionNeedsRecompose(const CelsSession *session);

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
 *
 * @param session   Target session. Non-NULL.
 * @param key       Unique 64-bit module type key.
 * @param name      Human-readable module identifier for diagnostics. Non-NULL.
 * @param instance  Pointer to module data struct. Non-NULL.
 * @param onReload  Optional callback invoked after code reload. Safe if NULL.
 * @param onDestroy Optional cleanup callback invoked on session teardown. Safe if NULL.
 */
void CelsSessionRegisterModule(CelsSession *session,
                              uint64_t key,
                              const char *name,
                              void *instance,
                              void (*onReload)(void *instance, struct CelsSession *session),
                              void (*onDestroy)(void *instance));

/**
 * Retrieves a registered subsystem module pointer by its 64-bit key.
 *
 * @param session Target session. Safe if NULL.
 * @param key     Unique 64-bit module type key.
 * @return Pointer to module struct instance, or NULL if not found.
 */
void *CelsSessionGetModule(const CelsSession *session, uint64_t key);

/**
 * Returns the currently active ambient session for the calling thread.
 *
 * @return Pointer to active session, or NULL outside an active composition pass.
 */
CelsSession *CelsGetCurrentSession(void);

/**
 * Sets the active ambient session for the calling thread.
 *
 * @param session Target session. Safe if NULL.
 */
void CelsSetCurrentSession(CelsSession *session);

/**
 * Enters a root composition group during a recomposition pass.
 *
 * Checks if the composition subtree requires re-execution based on invalidation
 * flags. Pushes group navigation onto the session stack.
 *
 * @param session Active session instance. Non-NULL.
 * @param rootKey Unique 64-bit identifier for the root composition.
 * @return True if the composition body should be executed; false if skipped.
 */
bool CelsEnterComposition(CelsSession *session, uint64_t rootKey);

/**
 * Enters a child composable group within the current active composition.
 *
 * Traverses or mounts the composable node in the session's slot table gap buffer.
 *
 * @param session Active session instance. Non-NULL.
 * @param key     Unique 64-bit identifier for the child composable.
 * @return True if the composable body should be executed; false if skipped.
 */
bool CelsEnterComposable(CelsSession *session, uint64_t key);

/**
 * Exits the current composable or composition group and pops the traversal stack.
 *
 * Finalizes group child count and advances the slot table cursor.
 *
 * @param session Active session instance. Non-NULL.
 */
void CelsExitGroup(CelsSession *session);

/**
 * Prunes and removes an entire subtree starting at the specified logical group index.
 *
 * Invokes registered unmount lifecycle cleanups for all descendant nodes in reverse order.
 *
 * @param session          Active session instance. Non-NULL.
 * @param rootLogicalIndex Logical index of the subtree root group.
 */
void CelsPruneSubtree(CelsSession *session, uint32_t rootLogicalIndex);

/**
 * Prunes and removes an entire subtree matching the specified 64-bit group key.
 *
 * Invokes registered unmount lifecycle cleanups for all descendant nodes in reverse order.
 *
 * @param session Active session instance. Non-NULL.
 * @param key     Unique 64-bit group key of the subtree root.
 */
void CelsPruneSubtreeByKey(CelsSession *session, uint64_t key);

/**
 * Allocates or resolves persistent slot memory in the session's arena.
 *
 * Returns a stable pointer to slot data pinned across recomposition passes.
 * On first mount, initializes memory with the bytes from initVal (if non-NULL).
 *
 * @param session Active session instance. Non-NULL.
 * @param size    Byte size of the requested slot allocation.
 * @param initVal Pointer to initial data bytes, or NULL for zero-initialization.
 * @return Stable pointer to slot memory in session data arena, or NULL on allocation failure.
 */
void *CelsResolveSlot(CelsSession *session,
                      size_t size,
                      const void *initVal);

/**
 * Allocates or resolves persistent slot memory with an optional unmount cleanup callback.
 *
 * On first mount, registers the cleanup hook. On subsequent remounts, updates the
 * cleanup hook pointer to prevent stale code addresses after hot-reload.
 *
 * @param session   Active session instance. Non-NULL.
 * @param size      Byte size of the requested slot allocation.
 * @param initVal   Pointer to initial data bytes, or NULL for zero-initialization.
 * @param onDestroy Destructor callback invoked when slot is unmounted or session destroyed. Safe if NULL.
 * @return Stable pointer to slot memory in session data arena, or NULL on allocation failure.
 */
void *CelsResolveSlotWithCleanup(CelsSession *session,
                                 size_t size,
                                 const void *initVal,
                                 void (*onDestroy)(void *ptr, CelsSession *session));

/**
 * Finds an active state instance by its key in the session.
 *
 * @param session Active session instance. Non-NULL.
 * @param key     Unique 64-bit state identifier.
 * @return Pointer to state payload, or NULL if not found.
 */
void *CelsGetState(CelsSession *session, uint64_t key);

/**
 * Queues a 64-bit group key for invalidation and recomposition on the next pass.
 *
 * Marks ancestor groups dirty and schedules re-evaluation on the next frame.
 *
 * @param session Active session instance. Can be NULL (falls back to ambient session).
 * @param key     Unique 64-bit group key to invalidate.
 */
void CelsSessionInvalidateKey(CelsSession *session, uint64_t key);

/**
 * Queues a 64-bit group key for immediate intra-frame recomposition if currently recomposing,
 * or on the next pass if outside recomposition.
 *
 * Used for synchronous intra-frame event bubbling (cel_event) to deliver events to ancestors
 * without a 1-frame latency.
 *
 * @param session Active session instance. Can be NULL (falls back to ambient session).
 * @param key     Unique 64-bit group key to invalidate.
 */
void CelsSessionInvalidateKeyImmediate(CelsSession *session, uint64_t key);

/**
 * Returns the 64-bit group key of the currently executing composable.
 *
 * @param session Active session instance. Can be NULL (falls back to ambient session).
 * @return Active 64-bit group key, or 0 if not currently executing inside a group.
 */
uint64_t CelsSessionGetCurrentGroupKey(const CelsSession *session);

/* ========================================================================= */
/* Session Transaction API & Cross-Thread Staging Buffer                     */
/* ========================================================================= */

/**
 * Stages a component/data set operation in the session's active transaction batch.
 *
 * Copies size payload bytes into the batch linear arena with zero locks.
 *
 * @param session  Target session. Can be NULL (falls back to ambient session).
 * @param targetId Target entity or resource identifier.
 * @param typeKey  64-bit component or payload type identifier.
 * @param size     Payload size in bytes.
 * @param data     Pointer to payload bytes to copy. Safe if NULL when size is 0.
 * @return True if staged successfully; false if batch capacity or arena is full.
 */
bool CelsSessionStageSet(CelsSession *session,
                         uint64_t targetId,
                         uint64_t typeKey,
                         size_t size,
                         const void *data);

/**
 * Stages a component removal operation in the session's active transaction batch.
 *
 * @param session  Target session. Can be NULL (falls back to ambient session).
 * @param targetId Target entity or resource identifier.
 * @param typeKey  64-bit component or payload type identifier.
 * @return True if staged successfully; false if batch capacity is full.
 */
bool CelsSessionStageRemove(CelsSession *session,
                            uint64_t targetId,
                            uint64_t typeKey);

/**
 * Stages a target deletion/destruction in the session's active transaction batch.
 *
 * @param session  Target session. Can be NULL (falls back to ambient session).
 * @param targetId Target entity or resource identifier.
 * @return True if staged successfully; false if batch capacity is full.
 */
bool CelsSessionStageDelete(CelsSession *session, uint64_t targetId);

/**
 * Stages a custom opcode transaction in the session's active transaction batch.
 *
 * @param session  Target session. Can be NULL (falls back to ambient session).
 * @param opCode   Custom opcode identifier (typically >= CELS_OP_CUSTOM).
 * @param targetId Target entity or resource identifier.
 * @param typeKey  64-bit component or payload type identifier.
 * @param size     Payload size in bytes.
 * @param data     Pointer to payload bytes to copy. Safe if NULL when size is 0.
 * @return True if staged successfully; false if batch capacity or arena is full.
 */
bool CelsSessionStageCustom(CelsSession *session,
                            uint32_t opCode,
                            uint64_t targetId,
                            uint64_t typeKey,
                            size_t size,
                            const void *data);

/**
 * Commits the current active transaction batch using the supplied handler callback.
 *
 * Iterates all staged operations in submission order and dispatches them to handler.
 * Automatically clears the active batch upon completion.
 *
 * @param session  Target session. Can be NULL (falls back to ambient session).
 * @param handler  Callback function invoked per operation. Non-NULL.
 * @param userData Context pointer passed through to handler. Can be NULL.
 * @return Number of operations dispatched.
 */
uint32_t CelsSessionCommitTransactions(CelsSession *session,
                                       CelsTransactionHandler handler,
                                       void *userData);

/**
 * Swaps active and ready transaction batches for lockless handoff to worker threads.
 *
 * The previously active batch becomes ready for consumption via CelsSessionGetReadyBatch,
 * and the new active batch is reset for new staging operations.
 *
 * @param session Target session. Safe if NULL.
 */
void CelsSessionSwapTransactionBatches(CelsSession *session);

/**
 * Returns a pointer to the ready transaction batch for consumption by worker threads.
 *
 * @param session Target session. Can be NULL.
 * @return Read-only pointer to the ready batch, or NULL if session is NULL.
 */
const CelsTransactionBatch *CelsSessionGetReadyBatch(const CelsSession *session);

/**
 * Resets the ready transaction batch after worker threads finish processing.
 *
 * @param session Target session. Safe if NULL.
 */
void CelsSessionClearReadyBatch(CelsSession *session);

/**
 * Gets the session's user context pointer.
 *
 * @param session Target session. Can be NULL.
 * @return User context pointer, or NULL if session is NULL or unset.
 */
void *CelsSessionGetUserData(const CelsSession *session);

/**
 * Sets the session's user context pointer.
 *
 * @param session  Target session. Safe if NULL.
 * @param userData Pointer to arbitrary user application context.
 */
void CelsSessionSetUserData(CelsSession *session, void *userData);

/**
 * Registers a callback invoked immediately after CelsSessionRecompose finishes frame convergence.
 *
 * @param session  Target session. Safe if NULL.
 * @param hook     Callback invoked at frame completion. Can be NULL to clear.
 * @param userData Context pointer passed to hook. Can be NULL.
 */
void CelsSessionSetPostRecomposeHook(CelsSession *session,
                                     CelsPostRecomposeFn hook,
                                     void *userData);

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

static inline uint64_t CelsGetCurrentGroupKey(CelsSession *s)
{
    if (s == NULL || s->currentDepth == 0) {
        return 0;
    }
    const CelsSlotGroup *g = CelsGetGroup(s, s->currentGroupIndex);
    return g ? g->key : 0;
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

