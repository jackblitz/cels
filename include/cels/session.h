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

#ifndef CELS_MAX_DEPTH
#define CELS_MAX_DEPTH 64u
#endif

#ifndef CELS_MAX_GROUPS
#define CELS_MAX_GROUPS 4096u
#endif

#ifndef CELS_DATA_ARENA_SIZE
#define CELS_DATA_ARENA_SIZE 32768u
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

/**
 * Memory slab profiles for CelsSession.
 * Fitting small session slabs entirely in L1d cache (16-64KB) eliminates CPU cache stalls.
 * For larger game worlds with hundreds or thousands of entities, scale up to 128K - 4M.
 */
typedef enum CelsSlabProfile {
    CELS_SLAB_16K  = 16u * 1024u,   /**< 16 KiB: Embedded & low-power cores (~128 groups) */
    CELS_SLAB_32K  = 32u * 1024u,   /**< 32 KiB: Standard L1d (Zen 1-3, Intel E-cores, ARM) [DEFAULT] (~256 groups) */
    CELS_SLAB_48K  = 48u * 1024u,   /**< 48 KiB: Modern high-perf L1d (Intel P-cores, Zen 4/5) (~384 groups) */
    CELS_SLAB_64K  = 64u * 1024u,   /**< 64 KiB: Extended L1d (Apple Silicon, complex trees) (~512 groups) */
    CELS_SLAB_128K = 128u * 1024u,  /**< 128 KiB: Mid-scale scenes (~1,024 groups) */
    CELS_SLAB_256K = 256u * 1024u,  /**< 256 KiB: High-scale scenes (~2,048 groups) */
    CELS_SLAB_512K = 512u * 1024u,  /**< 512 KiB: Large worlds (~4,096 groups) */
    CELS_SLAB_1M   = 1024u * 1024u, /**< 1 MiB: Massive scenes (~8,192 groups) */
    CELS_SLAB_2M   = 2048u * 1024u, /**< 2 MiB: Mega scale (~16,384 groups) */
    CELS_SLAB_4M   = 4096u * 1024u, /**< 4 MiB: Ultra scale (~32,768 groups) */
} CelsSlabProfile;

#ifndef CELS_DEFAULT_SLAB_SIZE
#define CELS_DEFAULT_SLAB_SIZE CELS_SLAB_512K
#endif

/**
 * Macro helper to declare a 64-byte aligned slab buffer for zero-alloc mode.
 */
#if defined(_MSC_VER)
#define CEL_SLAB(name, size) __declspec(align(64)) uint8_t name[size]
#else
#define CEL_SLAB(name, size) __attribute__((aligned(64))) uint8_t name[size]
#endif

typedef void (*CelsRootFn)(CelsSession *session);

/**
 * Configuration options for initializing a CelsSession.
 */
typedef struct CelsSessionConfig {
    CelsRootFn root;
    uint32_t   maxDrainIterations;
    size_t     slabSize;    /**< Total slab size in bytes (e.g. CELS_SLAB_32K). Defaults to 32 KiB if 0. */
    void      *slab;        /**< Optional user-provided 64-byte aligned buffer (zero-alloc mode). */
    uint32_t   maxGroups;   /**< Optional max groups. If 0, auto-calculated from slabSize. */
    struct CelsEngine *engine; /**< Optional host engine owning system modules */
} CelsSessionConfig;

/**
 * Lifecycle state callbacks for managed resources stored in slot memory.
 */
typedef struct CelsLifecycleDesc {
    size_t size;
    void (*onCreate)(void *instance, CelsSession *session);
    void (*onDestroy)(void *instance, CelsSession *session);
} CelsLifecycleDesc;
typedef CelsLifecycleDesc CelsObserverDesc;

#define onRemembered onCreate
#define OnCreated onCreate
#define OnCreate onCreate
#define onForgotten onDestroy
#define OnDestroyed onDestroy
#define OnDestroy onDestroy

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

#define CELS_MAX_ATTACHED_COMPOSITIONS 8u

typedef struct CelsAttachedComposition {
    uint64_t key;
    void (*body)(CelsSession *s, uint64_t key);
    bool (*lifecycleEval)(void *userData);
    void *statePtr;
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
    CelsRootFn root;
    bool hasComposedOnce;
    bool isRecomposing;
    bool isHotReloadPending;

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

    /* Reactive state registry */
    CelsStateRegistry stateRegistry;

    /* Lifecycle state cleanups */
    CelsCleanupHook cleanups[CELS_MAX_CLEANUPS];
    uint32_t cleanupCount;

    /* Invalidation queue */
    uint64_t invalidationQueue[CELS_MAX_QUEUE];
    uint32_t queueCount;

    uint32_t maxDrainIterations;

    /* Attached Compositions */
    CelsAttachedComposition attachedCompositions[CELS_MAX_ATTACHED_COMPOSITIONS];
    uint32_t attachedCount;

    /* Owning Host Engine Pointer & Fallback Module Storage */
    struct CelsEngine *engine;
    CelsModuleBinding fallbackModules[CELS_MAX_MODULES];
    uint32_t fallbackModuleCount;
};

/* ========================================================================= */
/* Attached Composition Functions                                            */
/* ========================================================================= */

void CelsSessionAttachComposition(CelsSession *s,
                                  uint64_t key,
                                  void (*body)(CelsSession *s, uint64_t key),
                                  bool (*eval)(void *userData),
                                  void *statePtr);

void CelsSessionDetachComposition(CelsSession *s, uint64_t key);

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
 * Assigns or replaces the root composable function for the session.
 *
 * @param session Target session. Non-NULL.
 * @param rootFn  Root composable function pointer.
 */
void CelsSessionSetRoot(CelsSession *session, CelsRootFn rootFn);

/**
 * Tears down a session, releasing all active lifecycle states and observers.
 *
 * @param session Target session. NULL is safely ignored.
 */
void CelsSessionDestroy(CelsSession *session);

/**
 * Executes a recomposition pass over the session tree.
 *
 * @param session Target session. Non-NULL.
 * @return CELS_OK or error code.
 */
CelsResult CelsSessionRecompose(CelsSession *session);

/**
 * Flags all mounted composition groups for re-evaluation on the next recompose pass.
 *
 * Used by dynamic library hot-reload to force execution of newly loaded composable
 * bodies while preserving all slot allocations, cel_remember memory, and
 * reactive state in the session data arena.
 *
 * @param session Target session. Non-NULL.
 */
void CelsSessionHotReload(CelsSession *session);

/**
 * Registers an engine subsystem module (SDL, Flecs, Audio, etc.) with the session.
 *
 * @param session   Target session. Non-NULL.
 * @param key       Unique 64-bit identifier for the module.
 * @param name      Human-readable module name for diagnostics.
 * @param instance  Pointer to developer-owned module struct.
 * @param onReload  Optional callback invoked after hot reload. May be NULL.
 * @param onDestroy Optional callback invoked on session teardown. May be NULL.
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
 * @param session Target session, or NULL for current ambient session.
 * @param key     Unique 64-bit module identifier.
 * @return Pointer to module instance, or NULL if not found.
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
 *
 * @param session Target session. Non-NULL.
 * @param size    Byte size of memory to resolve.
 * @param initVal Optional pointer to initial seed value (used on fresh mount).
 * @param desc    Optional lifecycle state descriptor (OnCreated/OnDestroyed).
 * @return Pointer to persistent slot memory in session arena.
 */
void *CelsResolveSlot(CelsSession *session,
                      size_t size,
                      const void *initVal,
                      const CelsLifecycleDesc *desc);

/**
 * Finds an active lifecycle state or observer instance by its group key.
 *
 * @param session Target session.
 * @param key     Group key.
 * @return Pointer to instance, or NULL if not found.
 */
void *CelsGetState(CelsSession *session, uint64_t key);
void *CelsFindLifecycleState(CelsSession *session, uint64_t key);
void *CelsFindObserver(CelsSession *session, uint64_t key);

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

