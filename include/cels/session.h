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

#define CELS_MAX_DEPTH 32u
#define CELS_MAX_GROUPS 256u
#define CELS_DATA_ARENA_SIZE 32768u
#define CELS_MAX_CLEANUPS 128u
#define CELS_MAX_DRAIN_ITERATIONS 8u

#define CELS_SLOT_ALIGNMENT 8u
#define CELS_ALIGN_UP(size) \
    (((size) + (CELS_SLOT_ALIGNMENT - 1u)) & ~(CELS_SLOT_ALIGNMENT - 1u))

#ifndef CELS_MAX_SLOTS
#define CELS_MAX_SLOTS (CELS_DATA_ARENA_SIZE / CELS_SLOT_ALIGNMENT)
#endif

#define CELS_FLAG_NONE 0u
#define CELS_FLAG_INVALIDATED (1u << 0)
#define CELS_FLAG_CONTAINS_INVALIDATED (1u << 1)
#define CELS_FLAG_FRESH_MOUNT (1u << 2)

typedef void (*CelsRootFn)(CelsSession *session);

/**
 * Configuration options for initializing a CelsSession.
 */
typedef struct CelsSessionConfig {
    CelsRootFn root;
    uint32_t maxDrainIterations;
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
    uint32_t size;
} CelsSlotAllocation;

#define CELS_MAX_ATTACHED_COMPOSITIONS 8u

typedef struct CelsAttachedComposition {
    uint64_t key;
    void (*body)(CelsSession *s, uint64_t key);
    bool (*lifecycleEval)(void *userData);
    void *statePtr;
    bool isAttached;
} CelsAttachedComposition;

/**
 * Primary session orchestrating composition, traversal, and reactive state.
 */
struct CelsSession {
    CelsRootFn root;
    bool hasComposedOnce;
    bool isRecomposing;

    uint32_t currentDepth;
    uint32_t currentGroupIndex;
    uint32_t currentSlotOffset;
    uint32_t logicalCursor;

    uint8_t activeStack[CELS_MAX_DEPTH];
    uint32_t groupIndexStack[CELS_MAX_DEPTH];
    uint32_t oldGroupSizeStack[CELS_MAX_DEPTH];
    uint32_t slotOffsetStack[CELS_MAX_DEPTH];

    /* Structural groups gap buffer */
    CelsSlotGroup groups[CELS_MAX_GROUPS];
    uint32_t groupsGapStart;
    uint32_t groupsGapEnd;

    /* Nonmoving slot arena: remembered pointers stay pinned */
    uint8_t dataArena[CELS_DATA_ARENA_SIZE];
    uint32_t dataGapStart;
    uint32_t dataGapEnd;
    CelsSlotAllocation slots[CELS_MAX_SLOTS];
    uint32_t slotCount;
    uint32_t nextGroupId;

    /* Reactive state registry */
    CelsStateRegistry stateRegistry;

#define states stateRegistry.cells
#define stateCount stateRegistry.cellCount

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
};

/* ========================================================================= */
/* Attached Composition Functions                                            */
/* ========================================================================= */

void CelsSessionAttachComposition(CelsSession *s,
                                  uint64_t key,
                                  void (*body)(CelsSession *s, uint64_t key),
                                  bool (*eval)(void *userData),
                                  void *statePtr);

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

static inline uint32_t
CelsGetLogicalGroupCount(const CelsSession *s)
{
    return CELS_MAX_GROUPS - (s->groupsGapEnd - s->groupsGapStart);
}

static inline uint32_t
CelsGroupLogicalToPhysical(const CelsSession *s, uint32_t logical)
{
    return (logical < s->groupsGapStart)
        ? logical
        : logical + (s->groupsGapEnd - s->groupsGapStart);
}

static inline CelsSlotGroup *
CelsGetGroup(CelsSession *s, uint32_t logical)
{
    return &s->groups[CelsGroupLogicalToPhysical(s, logical)];
}

static inline bool
CelsIsFreshMount(CelsSession *s)
{
    if (s == NULL || s->currentDepth == 0) {
        return false;
    }
    return (CelsGetGroup(s, s->currentGroupIndex)->flags
            & CELS_FLAG_FRESH_MOUNT) != 0;
}
