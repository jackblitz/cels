/*
 * CELS (Composition Entity Lifecycle State)
 * Single-header declarative composition engine for C99.
 *
 * Implements the Jetpack Compose slot table and RememberObserver lifecycle model.
 * Define CELS_IMPLEMENTATION in exactly one .c file before including:
 *   #define CELS_IMPLEMENTATION
 *   #include "cels.h"
 */

#ifndef CELS_H
#define CELS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Configuration Limits                                                      */
/* ========================================================================= */

#ifndef CELS_MAX_DEPTH
#define CELS_MAX_DEPTH 32
#endif

#ifndef CELS_MAX_STATES
#define CELS_MAX_STATES 64
#endif

#ifndef CELS_MAX_WATCHERS
#define CELS_MAX_WATCHERS 8
#endif

#ifndef CELS_MAX_QUEUE
#define CELS_MAX_QUEUE 64
#endif

#ifndef CELS_MAX_GROUPS
#define CELS_MAX_GROUPS 256
#endif

#ifndef CELS_DATA_ARENA_SIZE
#define CELS_DATA_ARENA_SIZE 32768
#endif

#ifndef CELS_MAX_CLEANUPS
#define CELS_MAX_CLEANUPS 128
#endif

#ifndef CELS_MAX_DRAIN_ITERATIONS
#define CELS_MAX_DRAIN_ITERATIONS 8
#endif

#define CELS_SLOT_ALIGNMENT 8
#define CELS_ALIGN_UP(size) (((size) + (CELS_SLOT_ALIGNMENT - 1)) & ~(CELS_SLOT_ALIGNMENT - 1))

/* ========================================================================= */
/* Enums & Status Flags                                                      */
/* ========================================================================= */

typedef enum CelsResult {
    CELS_OK                               =  0,
    CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE = -1,
    CELS_ERROR_SLOT_ARENA_OVERFLOW        = -2,
    CELS_ERROR_GROUP_OVERFLOW             = -3,
    CELS_ERROR_QUEUE_OVERFLOW             = -4,
    CELS_ERROR_CLEANUP_OVERFLOW           = -5,
    CELS_ERROR_UNBALANCED_SCOPE           = -6,
    CELS_ERROR_NO_ROOT_COMPOSABLE         = -7,
    CELS_ERROR_STATE_OVERFLOW             = -8
} CelsResult;

#define CELS_FLAG_NONE                 0
#define CELS_FLAG_INVALIDATED          (1 << 0)
#define CELS_FLAG_CONTAINS_INVALIDATED (1 << 1)
#define CELS_FLAG_FRESH_MOUNT          (1 << 2)

/* ========================================================================= */
/* Core Data Types                                                           */
/* ========================================================================= */

typedef struct CelsSession CelsSession;
typedef void (*CelsRootFn)(CelsSession *s);

typedef struct CelsStateHeader {
    uint16_t watcherCount;
    uint32_t watcherKeys[CELS_MAX_WATCHERS];
} CelsStateHeader;

typedef struct CelsStateCell {
    const void      *ptr;
    CelsStateHeader  header;
} CelsStateCell;

#define CEL_State(TypeName) \
    typedef struct TypeName TypeName; \
    struct TypeName

typedef struct CelsObserverDesc {
    size_t size;
    void (*onRemembered)(void *observer, CelsSession *s);
    void (*onForgotten)(void *observer, CelsSession *s);
} CelsObserverDesc;

typedef struct CelsCleanupHook {
    uint32_t groupKey;
    void    *instance;
    void   (*onForgotten)(void *instance, CelsSession *s);
} CelsCleanupHook;

typedef struct CelsSlotGroup {
    uint32_t key;
    uint16_t parentIndex;
    uint16_t groupSize;
    uint32_t dataOffset;
    uint16_t dataSize;
    uint16_t flags;
} CelsSlotGroup;

typedef struct CelsSessionConfig {
    CelsRootFn root;
    uint32_t   maxDrainIterations;
} CelsSessionConfig;

struct CelsSession {
    CelsRootFn root;
    bool       hasComposedOnce;

    uint32_t currentDepth;
    uint32_t currentGroupIndex;
    uint32_t currentSlotOffset;
    uint32_t logicalCursor;

    uint8_t  activeStack[CELS_MAX_DEPTH];
    uint32_t groupIndexStack[CELS_MAX_DEPTH];
    uint16_t oldGroupSizeStack[CELS_MAX_DEPTH];

    /* Dual Gap Buffer: Structural Groups */
    CelsSlotGroup groups[CELS_MAX_GROUPS];
    uint32_t      groupsGapStart;
    uint32_t      groupsGapEnd;

    /* Dual Gap Buffer: Slot Data Arena */
    uint8_t       dataArena[CELS_DATA_ARENA_SIZE];
    uint32_t      dataGapStart;
    uint32_t      dataGapEnd;

    /* State Registry */
    CelsStateCell states[CELS_MAX_STATES];
    uint32_t      stateCount;

    /* RememberObserver cleanup tracking */
    CelsCleanupHook cleanups[CELS_MAX_CLEANUPS];
    uint32_t        cleanupCount;

    /* Invalidation Queue */
    uint32_t invalidationQueue[CELS_MAX_QUEUE];
    uint32_t queueCount;

    uint32_t maxDrainIterations;
    bool     isRecomposing;
};

/* ========================================================================= */
/* Key Utilities (FNV-1a Hash)                                               */
/* ========================================================================= */

static inline uint32_t CelsHashKey(const char *str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

static inline uint32_t CelsKeyIndex(uint32_t baseKey, uint32_t index) {
    return baseKey ^ (index * 0x9e3779b9u);
}

#define CEL_KEY(str) CelsHashKey(str)
#define CEL_KeyIndex(baseKey, index) CelsKeyIndex((baseKey), (index))

/* ========================================================================= */
/* Syntactically Locked Declarative DSL Macros                               */
/* ========================================================================= */

#define CEL_Composition(session, rootKey) \
    do { \
        if (CelsEnterComposition((session), (rootKey)))

#define CEL_Composable(session, key) \
    do { \
        if (CelsEnterComposable((session), (key)))

#define CEL_Close(session) \
        CelsExitGroup((session)); \
    } while (0)

#define CEL_Observer(Type) \
    typedef struct Type Type; \
    void Type##_OnRemembered(Type *self, CelsSession *s); \
    void Type##_OnForgotten(Type *self, CelsSession *s); \
    struct Type

#define CEL_BIND_OBSERVER(Type) \
    static const CelsObserverDesc Type##_Desc = { \
        .size = sizeof(Type), \
        .onRemembered = (void(*)(void*, CelsSession*))Type##_OnRemembered, \
        .onForgotten  = (void(*)(void*, CelsSession*))Type##_OnForgotten \
    }

#define cel_remember_observer(session, Type) \
    ((Type*)CelsResolveSlot((session), sizeof(Type), NULL, &Type##_Desc))

/* Compound literal provides a stack lvalue for any scalar, struct, or 0 */
#define cel_remember(session, Type, ...) \
    ((Type*)CelsResolveSlot((session), sizeof(Type), &(Type){ __VA_ARGS__ }, NULL))

#define cel_watch(session, state_ptr) \
    (CelsStateRead((session), (state_ptr)), *(state_ptr))

/* Scoped mutation: infers struct type and exposes this-> */
#define cel_mutate(session, state_ptr) \
    for (__typeof__(*(state_ptr)) _cel_old_ = *(state_ptr), *this = (state_ptr); \
         this != NULL; \
         CelsStateCommitMutation((session), this, &_cel_old_, sizeof(*this)), this = NULL)

#define CEL_FindObserver(session, key, Type) \
    ((Type*)CelsFindObserverSlot((session), (key), sizeof(Type)))

/* ========================================================================= */
/* Engine Function Declarations                                              */
/* ========================================================================= */

void        CelsSessionInit(CelsSession *s, const CelsSessionConfig *cfg);
void        CelsSessionSetRoot(CelsSession *s, CelsRootFn rootFn);
void        CelsSessionDestroy(CelsSession *s);
CelsResult  CelsSessionRecompose(CelsSession *s);

bool        CelsEnterComposition(CelsSession *s, uint32_t rootKey);
bool        CelsEnterComposable(CelsSession *s, uint32_t key);
void        CelsExitGroup(CelsSession *s);
void        CelsPruneSubtree(CelsSession *s, uint32_t rootLogicalIndex);

void*       CelsResolveSlot(CelsSession *s, size_t size, const void *initVal, const CelsObserverDesc *desc);
void        CelsStateRead(CelsSession *s, const void *statePtr);
void        CelsStateCommitMutation(CelsSession *s, const void *statePtr, const void *oldVal, size_t size);
void*       CelsFindObserverSlot(CelsSession *s, uint32_t key, size_t expectedSize);

#ifdef __cplusplus
}
#endif

#endif /* CELS_H */

/* ========================================================================= */
/* ENGINE IMPLEMENTATION                                                     */
/* ========================================================================= */

#ifdef CELS_IMPLEMENTATION

static inline uint32_t CelsGetLogicalGroupCount(const CelsSession *s) {
    return CELS_MAX_GROUPS - (s->groupsGapEnd - s->groupsGapStart);
}

static inline uint32_t CelsGroupLogicalToPhysical(const CelsSession *s, uint32_t logical) {
    return (logical < s->groupsGapStart) ? logical : logical + (s->groupsGapEnd - s->groupsGapStart);
}

static inline uint32_t CelsDataLogicalToPhysical(const CelsSession *s, uint32_t logical) {
    return (logical < s->dataGapStart) ? logical : logical + (s->dataGapEnd - s->dataGapStart);
}

static inline CelsSlotGroup* CelsGetGroup(CelsSession *s, uint32_t logical) {
    return &s->groups[CelsGroupLogicalToPhysical(s, logical)];
}

static inline uint8_t* CelsGetData(CelsSession *s, uint32_t logicalOffset) {
    return &s->dataArena[CelsDataLogicalToPhysical(s, logicalOffset)];
}

static void CelsMoveGroupGap(CelsSession *s, uint32_t targetLogical) {
    if (targetLogical == s->groupsGapStart) return;

    if (targetLogical < s->groupsGapStart) {
        uint32_t delta = s->groupsGapStart - targetLogical;
        memmove(&s->groups[s->groupsGapEnd - delta],
                &s->groups[targetLogical],
                delta * sizeof(CelsSlotGroup));
        s->groupsGapStart -= delta;
        s->groupsGapEnd   -= delta;
    } else {
        uint32_t delta = targetLogical - s->groupsGapStart;
        memmove(&s->groups[s->groupsGapStart],
                &s->groups[s->groupsGapEnd],
                delta * sizeof(CelsSlotGroup));
        s->groupsGapStart += delta;
        s->groupsGapEnd   += delta;
    }
}

static void CelsMoveDataGap(CelsSession *s, uint32_t targetLogical) {
    if (targetLogical == s->dataGapStart) return;

    if (targetLogical < s->dataGapStart) {
        uint32_t delta = s->dataGapStart - targetLogical;
        memmove(&s->dataArena[s->dataGapEnd - delta],
                &s->dataArena[targetLogical],
                delta);
        s->dataGapStart -= delta;
        s->dataGapEnd   -= delta;
    } else {
        uint32_t delta = targetLogical - s->dataGapStart;
        memmove(&s->dataArena[s->dataGapStart],
                &s->dataArena[s->dataGapEnd],
                delta);
        s->dataGapStart += delta;
        s->dataGapEnd   += delta;
    }
}

static void CelsUnsubscribeGroupWatchers(CelsSession *s, uint32_t groupKey) {
    for (uint32_t i = 0; i < s->stateCount; ++i) {
        CelsStateHeader *header = &s->states[i].header;
        for (uint16_t w = 0; w < header->watcherCount; ++w) {
            if (header->watcherKeys[w] == groupKey) {
                header->watcherKeys[w] = header->watcherKeys[--header->watcherCount];
                break;
            }
        }
    }
}

static void CelsFireCleanupsForGroup(CelsSession *s, uint32_t groupKey) {
    for (uint32_t i = s->cleanupCount; i > 0; --i) {
        uint32_t idx = i - 1;
        if (s->cleanups[idx].groupKey == groupKey) {
            if (s->cleanups[idx].onForgotten) {
                s->cleanups[idx].onForgotten(s->cleanups[idx].instance, s);
            }
            s->cleanups[idx] = s->cleanups[--s->cleanupCount];
        }
    }
}

void CelsPruneSubtree(CelsSession *s, uint32_t rootLogicalIndex) {
    CelsSlotGroup *root = CelsGetGroup(s, rootLogicalIndex);
    uint32_t groupsToRemove = 1 + root->groupSize;
    uint32_t dataBytesToRemove = 0;

    for (uint32_t i = groupsToRemove; i > 0; --i) {
        uint32_t targetLogical = rootLogicalIndex + (i - 1);
        CelsSlotGroup *g = CelsGetGroup(s, targetLogical);

        dataBytesToRemove += g->dataSize;
        CelsFireCleanupsForGroup(s, g->key);
        CelsUnsubscribeGroupWatchers(s, g->key);
    }

    uint32_t dataStartLogical = root->dataOffset;
    uint32_t parentIdx = root->parentIndex;
    bool hasParent = (rootLogicalIndex > 0);

    CelsMoveDataGap(s, dataStartLogical);
    s->dataGapEnd += dataBytesToRemove;

    uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    for (uint32_t i = rootLogicalIndex + groupsToRemove; i < totalGroups; ++i) {
        CelsSlotGroup *g = CelsGetGroup(s, i);
        g->dataOffset -= dataBytesToRemove;
    }

    CelsMoveGroupGap(s, rootLogicalIndex);
    s->groupsGapEnd += groupsToRemove;

    if (hasParent) {
        uint32_t curr = parentIdx;
        while (true) {
            CelsSlotGroup *p = CelsGetGroup(s, curr);
            assert(p->groupSize >= groupsToRemove);
            p->groupSize -= (uint16_t)groupsToRemove;

            if (curr == 0) break;
            curr = p->parentIndex;
        }
    }
}

static void CelsDrainInvalidationQueue(CelsSession *s) {
    uint32_t totalGroups = CelsGetLogicalGroupCount(s);

    while (s->queueCount > 0) {
        uint32_t targetKey = s->invalidationQueue[--s->queueCount];

        for (uint32_t i = 0; i < totalGroups; ++i) {
            CelsSlotGroup *g = CelsGetGroup(s, i);
            if (g->key == targetKey) {
                g->flags |= CELS_FLAG_INVALIDATED;

                if (i > 0) {
                    uint32_t curr = g->parentIndex;
                    while (true) {
                        CelsSlotGroup *p = CelsGetGroup(s, curr);
                        p->flags |= CELS_FLAG_CONTAINS_INVALIDATED;
                        if (curr == 0) break;
                        curr = p->parentIndex;
                    }
                }
                break;
            }
        }
    }
}

void CelsSessionInit(CelsSession *s, const CelsSessionConfig *cfg) {
    assert(s != NULL);
    memset(s, 0, sizeof(CelsSession));

    s->root = cfg ? cfg->root : NULL;
    s->hasComposedOnce = false;

    s->groupsGapStart = 0;
    s->groupsGapEnd   = CELS_MAX_GROUPS;

    s->dataGapStart   = 0;
    s->dataGapEnd     = CELS_DATA_ARENA_SIZE;

    s->maxDrainIterations = (cfg && cfg->maxDrainIterations > 0)
        ? cfg->maxDrainIterations
        : CELS_MAX_DRAIN_ITERATIONS;
}

void CelsSessionSetRoot(CelsSession *s, CelsRootFn rootFn) {
    assert(s != NULL);
    s->root = rootFn;
}

void CelsSessionDestroy(CelsSession *s) {
    if (!s) return;
    for (uint32_t i = s->cleanupCount; i > 0; --i) {
        uint32_t idx = i - 1;
        if (s->cleanups[idx].onForgotten) {
            s->cleanups[idx].onForgotten(s->cleanups[idx].instance, s);
        }
    }
    memset(s, 0, sizeof(CelsSession));
}

CelsResult CelsSessionRecompose(CelsSession *s) {
    assert(s != NULL);
    if (!s->root) {
        return CELS_ERROR_NO_ROOT_COMPOSABLE;
    }

    if (s->hasComposedOnce && s->queueCount == 0) {
        return CELS_OK;
    }

    uint32_t iterations = 0;
    s->isRecomposing = true;

    do {
        if (++iterations > s->maxDrainIterations) {
            s->isRecomposing = false;
            return CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE;
        }

        CelsDrainInvalidationQueue(s);

        s->currentDepth = 0;
        s->currentSlotOffset = 0;
        s->logicalCursor = 0;

        s->root(s);

        if (s->currentDepth != 0) {
            s->isRecomposing = false;
            return CELS_ERROR_UNBALANCED_SCOPE;
        }

    } while (s->queueCount > 0);

    s->hasComposedOnce = true;
    s->isRecomposing = false;
    return CELS_OK;
}

bool CelsEnterComposition(CelsSession *s, uint32_t rootKey) {
    assert(s->currentDepth == 0 && "CEL_Composition cannot be nested");
    uint32_t depth = s->currentDepth++;
    s->activeStack[depth] = 1;

    uint32_t totalGroups = CelsGetLogicalGroupCount(s);

    if (totalGroups == 0) {
        CelsMoveGroupGap(s, 0);
        s->groups[0] = (CelsSlotGroup){
            .key = rootKey,
            .parentIndex = 0,
            .groupSize = 0,
            .dataOffset = 0,
            .dataSize = 0,
            .flags = CELS_FLAG_FRESH_MOUNT
        };
        s->groupsGapStart = 1;
    } else {
        CelsSlotGroup *root = CelsGetGroup(s, 0);
        if (root->key != rootKey) {
            CelsPruneSubtree(s, 0);
            return CelsEnterComposition(s, rootKey);
        }
    }

    s->currentGroupIndex = 0;
    s->groupIndexStack[depth] = 0;
    s->oldGroupSizeStack[depth] = CelsGetGroup(s, 0)->groupSize;
    s->currentSlotOffset = 0;
    s->logicalCursor = 1;

    return true;
}

bool CelsEnterComposable(CelsSession *s, uint32_t key) {
    assert(s->currentDepth > 0 && "CEL_Composable must be nested within CEL_Composition");
    assert(s->currentDepth < CELS_MAX_DEPTH && "Exceeded CELS_MAX_DEPTH");

    uint32_t depth = s->currentDepth++;
    uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    uint32_t cursor = s->logicalCursor;

    if (cursor < totalGroups && CelsGetGroup(s, cursor)->key == key) {
        CelsSlotGroup *cached = CelsGetGroup(s, cursor);

        if (!(cached->flags & (CELS_FLAG_INVALIDATED | CELS_FLAG_CONTAINS_INVALIDATED))) {
            s->activeStack[depth] = 0;
            s->groupIndexStack[depth] = cursor;
            s->logicalCursor += (1 + cached->groupSize);
            return false;
        }

        cached->flags &= ~(CELS_FLAG_INVALIDATED | CELS_FLAG_CONTAINS_INVALIDATED);
        s->activeStack[depth] = 1;
        s->groupIndexStack[depth] = cursor;
        s->oldGroupSizeStack[depth] = cached->groupSize;
        s->currentGroupIndex = cursor;
        s->currentSlotOffset = 0;
        s->logicalCursor++;
        return true;
    }

    uint32_t parentIdx = s->groupIndexStack[depth - 1];
    uint32_t parentExpectedEnd = parentIdx + 1 + s->oldGroupSizeStack[depth - 1];
    uint32_t matchIdx = UINT32_MAX;

    for (uint32_t i = cursor; i < totalGroups && i < parentExpectedEnd; ++i) {
        if (CelsGetGroup(s, i)->key == key) {
            matchIdx = i;
            break;
        }
    }

    if (matchIdx != UINT32_MAX) {
        while (s->logicalCursor < matchIdx) {
            CelsSlotGroup *skipped = CelsGetGroup(s, s->logicalCursor);
            uint32_t removed = 1 + skipped->groupSize;
            CelsPruneSubtree(s, s->logicalCursor);
            matchIdx -= removed;
            parentExpectedEnd -= removed;
        }
        return CelsEnterComposable(s, key);
    }

    assert(totalGroups < CELS_MAX_GROUPS && "CELS_ERROR_GROUP_OVERFLOW");
    CelsMoveGroupGap(s, cursor);

    s->groups[s->groupsGapStart] = (CelsSlotGroup){
        .key = key,
        .parentIndex = (uint16_t)parentIdx,
        .groupSize = 0,
        .dataOffset = s->dataGapStart,
        .dataSize = 0,
        .flags = CELS_FLAG_FRESH_MOUNT
    };
    s->groupsGapStart++;

    s->activeStack[depth] = 1;
    s->groupIndexStack[depth] = cursor;
    s->oldGroupSizeStack[depth] = 0;
    s->currentGroupIndex = cursor;
    s->currentSlotOffset = 0;
    s->logicalCursor = cursor + 1;

    return true;
}

void CelsExitGroup(CelsSession *s) {
    assert(s->currentDepth > 0 && "Unmatched CEL_Close call");
    uint32_t depth = --s->currentDepth;
    uint32_t groupIdx = s->groupIndexStack[depth];

    if (s->activeStack[depth]) {
        uint32_t expectedEnd = groupIdx + 1 + s->oldGroupSizeStack[depth];
        while (s->logicalCursor < expectedEnd && s->logicalCursor < CelsGetLogicalGroupCount(s)) {
            CelsSlotGroup *dead = CelsGetGroup(s, s->logicalCursor);
            uint32_t removed = 1 + dead->groupSize;
            CelsPruneSubtree(s, s->logicalCursor);
            expectedEnd -= removed;
        }

        CelsSlotGroup *g = CelsGetGroup(s, groupIdx);
        if (g->flags & CELS_FLAG_FRESH_MOUNT) {
            g->dataSize = (uint16_t)s->currentSlotOffset;
            g->flags &= ~CELS_FLAG_FRESH_MOUNT;
        }

        g->groupSize = (uint16_t)((s->logicalCursor - 1) - groupIdx);
    }

    if (depth > 0) {
        s->currentGroupIndex = s->groupIndexStack[depth - 1];
    }
}

void* CelsResolveSlot(CelsSession *s, size_t size, const void *initVal, const CelsObserverDesc *desc) {
    assert(s->currentDepth > 0);
    CelsSlotGroup *group = CelsGetGroup(s, s->currentGroupIndex);
    size_t alignedSize = CELS_ALIGN_UP(size);

    if (group->flags & CELS_FLAG_FRESH_MOUNT) {
        assert(s->dataGapStart + alignedSize <= s->dataGapEnd && "CELS_ERROR_SLOT_ARENA_OVERFLOW");

        uint8_t *slotPtr = &s->dataArena[s->dataGapStart];
        s->dataGapStart += (uint32_t)alignedSize;

        if (initVal) {
            memcpy(slotPtr, initVal, size);
        } else {
            memset(slotPtr, 0, size);
        }

        if (desc) {
            if (desc->onForgotten) {
                assert(s->cleanupCount < CELS_MAX_CLEANUPS && "CELS_ERROR_CLEANUP_OVERFLOW");
                s->cleanups[s->cleanupCount++] = (CelsCleanupHook){
                    .groupKey = group->key,
                    .instance = slotPtr,
                    .onForgotten = desc->onForgotten
                };
            }
            if (desc->onRemembered) {
                desc->onRemembered(slotPtr, s);
            }
        }

        s->currentSlotOffset += (uint32_t)alignedSize;
        return (void*)slotPtr;
    }

    uint8_t *cachedPtr = CelsGetData(s, group->dataOffset + s->currentSlotOffset);
    s->currentSlotOffset += (uint32_t)alignedSize;
    return (void*)cachedPtr;
}

static CelsStateHeader* CelsGetOrCreateStateHeader(CelsSession *s, const void *statePtr) {
    for (uint32_t i = 0; i < s->stateCount; ++i) {
        if (s->states[i].ptr == statePtr) {
            return &s->states[i].header;
        }
    }
    assert(s->stateCount < CELS_MAX_STATES && "CELS_ERROR_STATE_OVERFLOW");
    uint32_t idx = s->stateCount++;
    s->states[idx].ptr = statePtr;
    s->states[idx].header.watcherCount = 0;
    return &s->states[idx].header;
}

void CelsStateRead(CelsSession *s, const void *statePtr) {
    assert(statePtr != NULL);

    if (s->currentDepth > 0 && s->activeStack[s->currentDepth - 1]) {
        uint32_t activeGroupIdx = s->groupIndexStack[s->currentDepth - 1];
        uint32_t currentKey = CelsGetGroup(s, activeGroupIdx)->key;

        CelsStateHeader *header = CelsGetOrCreateStateHeader(s, statePtr);

        for (uint16_t i = 0; i < header->watcherCount; ++i) {
            if (header->watcherKeys[i] == currentKey) {
                return;
            }
        }

        if (header->watcherCount < CELS_MAX_WATCHERS) {
            header->watcherKeys[header->watcherCount++] = currentKey;
        }
    }
}

void CelsStateCommitMutation(CelsSession *s, const void *statePtr, const void *oldVal, size_t size) {
    assert(statePtr != NULL);
    assert(oldVal != NULL);

    if (memcmp(statePtr, oldVal, size) == 0) {
        return;
    }

    CelsStateHeader *header = CelsGetOrCreateStateHeader(s, statePtr);
    for (uint16_t i = 0; i < header->watcherCount; ++i) {
        if (s->queueCount < CELS_MAX_QUEUE) {
            s->invalidationQueue[s->queueCount++] = header->watcherKeys[i];
        }
    }
}

void* CelsFindObserverSlot(CelsSession *s, uint32_t key, size_t expectedSize) {
    uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    for (uint32_t i = 0; i < totalGroups; ++i) {
        CelsSlotGroup *g = CelsGetGroup(s, i);
        if (g->key == key && g->dataSize >= expectedSize) {
            return (void*)CelsGetData(s, g->dataOffset);
        }
    }
    return NULL;
}

#endif /* CELS_IMPLEMENTATION */