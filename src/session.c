#include "cels/session.h"
#include "cels/engine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_MSC_VER)
#include <malloc.h>
#endif

#include "cels/slot_table.h"
#include "cels/state.h"
#include "cels/log.h"

#ifndef CELS_THREAD_LOCAL
    #if defined(_MSC_VER)
        #define CELS_THREAD_LOCAL __declspec(thread)
    #elif defined(__GNUC__) && !defined(_WIN32)
        #define CELS_THREAD_LOCAL __thread
    #elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__) && !defined(_WIN32)
        #define CELS_THREAD_LOCAL _Thread_local
    #else
        #define CELS_THREAD_LOCAL
    #endif
#endif

/**
 * Allocates a 64-byte cache-line aligned memory slab.
 *
 * Slabs are aligned to CELS_CACHE_LINE_SIZE (64 bytes) to guarantee that
 * groups, slot allocations, and arena boundaries match CPU cache lines.
 *
 * @param size Total byte size of the slab to allocate.
 * @return Pointer to 64-byte aligned memory, or NULL on allocation failure.
 */
static void *CelsAllocAlignedSlab(size_t size)
{
#if defined(_WIN32) || defined(_MSC_VER)
    return _aligned_malloc(size, CELS_CACHE_LINE_SIZE);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
    return aligned_alloc(CELS_CACHE_LINE_SIZE, (size + CELS_CACHE_LINE_SIZE - 1u) & ~(CELS_CACHE_LINE_SIZE - 1u));
#else
    void *ptr = NULL;
    if (posix_memalign(&ptr, CELS_CACHE_LINE_SIZE, size) != 0) {
        return NULL;
    }
    return ptr;
#endif
}

/**
 * Releases memory previously allocated with CelsAllocAlignedSlab.
 *
 * @param ptr Pointer to aligned slab memory. NULL is safely ignored.
 */
static void CelsFreeAlignedSlab(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
#if defined(_WIN32) || defined(_MSC_VER)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

static CELS_THREAD_LOCAL CelsSession *s_currentSession = NULL;

/**
 * Returns the currently active ambient session for the calling thread.
 *
 * Used by macros when an explicit session parameter is omitted.
 *
 * @return Active CelsSession pointer, or NULL if no session is active.
 */
CelsSession *CelsGetCurrentSession(void)
{
    return s_currentSession;
}

/**
 * Sets the active ambient session for the calling thread.
 *
 * @param session Target session to bind to current thread context. May be NULL.
 */
void CelsSetCurrentSession(CelsSession *session)
{
    s_currentSession = session;
}

/**
 * Shifts the groups gap buffer to targetLogical index.
 *
 * Moves elements around the gap using memmove to open insertion space
 * or compact memory at targetLogical while maintaining logical ordering.
 *
 * @param s             Target session. Non-NULL.
 * @param targetLogical Desired logical gap start index.
 */
static void MoveGroupGap(CelsSession *s, uint32_t targetLogical)
{
    if (targetLogical == s->groupsGapStart) {
        return;
    }

    if (targetLogical < s->groupsGapStart) {
        const uint32_t delta = s->groupsGapStart - targetLogical;
        memmove(&s->groups[s->groupsGapEnd - delta],
                &s->groups[targetLogical],
                delta * sizeof(CelsSlotGroup));
        s->groupsGapStart -= delta;
        s->groupsGapEnd -= delta;
    } else {
        const uint32_t delta = targetLogical - s->groupsGapStart;
        memmove(&s->groups[s->groupsGapStart],
                &s->groups[s->groupsGapEnd],
                delta * sizeof(CelsSlotGroup));
        s->groupsGapStart += delta;
        s->groupsGapEnd += delta;
    }
}

/**
 * Invokes onDestroy callbacks for all lifecycle states belonging to groupId.
 *
 * Walks the cleanups array in reverse order of creation and fires any registered
 * destructor before removing the hook and compacting the array.
 *
 * @param s       Target session. Non-NULL.
 * @param groupId Unique group identifier whose resources are being released.
 */
static void FireCleanupsForGroup(CelsSession *s, uint32_t groupId)
{
    for (uint32_t i = s->cleanupCount; i > 0; --i) {
        const uint32_t idx = i - 1;
        if (s->cleanups[idx].groupId == groupId) {
            if (s->cleanups[idx].onDestroy) {
                s->cleanups[idx].onDestroy(s->cleanups[idx].instance, s);
            }
            --s->cleanupCount;
            memmove(&s->cleanups[idx],
                    &s->cleanups[idx + 1],
                    (s->cleanupCount - idx) * sizeof(s->cleanups[0]));
        }
    }
}

/**
 * Reclaims all data arena slots allocated to groupId.
 *
 * Also purges any reactive state cells registered in the reclaimed memory range
 * to prevent dangling pointer subscriptions.
 *
 * @param s       Target session. Non-NULL.
 * @param groupId Unique group identifier whose slots should be reclaimed.
 */
static void ReleaseSlotsForGroup(CelsSession *s, uint32_t groupId)
{
    for (uint32_t i = 0; i < s->slotCount;) {
        CelsSlotAllocation *const slot = &s->slots[i];
        if (slot->groupId != groupId) {
            ++i;
            continue;
        }

        const uintptr_t first = (uintptr_t)&s->dataArena[slot->arenaOffset];
        const uintptr_t end = first + slot->size;

        CelsStateRegistryReleaseRange(&s->stateRegistry, first, end);

        s->dataGapStart -= slot->size;
        --s->slotCount;
        memmove(slot, slot + 1, (s->slotCount - i) * sizeof(*slot));
    }
}

/**
 * Drains the invalidation queue and marks affected groups and ancestor paths.
 *
 * Sets CELS_FLAG_INVALIDATED on the targeted group and all its transitive descendants,
 * and sets CELS_FLAG_CONTAINS_INVALIDATED on each ancestor up to the root.
 *
 * @param s Target session. Non-NULL.
 */
static void DrainInvalidationQueue(CelsSession *s)
{
    const uint32_t totalGroups = CelsGetLogicalGroupCount(s);

    while (s->queueCount > 0) {
        const uint64_t targetKey = s->invalidationQueue[--s->queueCount];

        for (uint32_t i = 0; i < totalGroups; ++i) {
            CelsSlotGroup *const g = CelsGetGroup(s, i);
            if (g->key == targetKey) {
                g->flags |= CELS_FLAG_INVALIDATED;

                for (uint32_t c = i + 1;
                     c <= i + g->groupSize && c < totalGroups;
                     ++c) {
                    CelsGetGroup(s, c)->flags |= CELS_FLAG_INVALIDATED;
                }

                if (i > 0) {
                    uint32_t curr = g->parentIndex;
                    while (true) {
                        CelsSlotGroup *const p = CelsGetGroup(s, curr);
                        p->flags |= CELS_FLAG_CONTAINS_INVALIDATED;
                        if (curr == 0) {
                            break;
                        }
                        curr = p->parentIndex;
                    }
                }
                break;
            }
        }
    }
}

/**
 * Initializes a session, carving partitions out of an aligned memory slab.
 *
 * Configures the groups gap buffer, the slot allocation table, the nonmoving
 * data arena, and the reactive state registry within the slab. If config->slab
 * is NULL, allocates an aligned slab of the requested size.
 *
 * @param s      Session to initialize. Non-NULL.
 * @param config Optional session configuration. If NULL, defaults are used.
 */
void CelsSessionInit(CelsSession *s, const CelsSessionConfig *config)
{
    assert(s != NULL);
    memset(s, 0, sizeof(*s));

    size_t slabSize = (config && config->slabSize > 0)
        ? config->slabSize
        : (size_t)CELS_DEFAULT_SLAB_SIZE;

    /* Align slabSize up to cache line boundary */
    slabSize = (slabSize + CELS_CACHE_LINE_SIZE - 1u) & ~(CELS_CACHE_LINE_SIZE - 1u);

    if (config && config->slab != NULL) {
        assert(((uintptr_t)config->slab % CELS_CACHE_LINE_SIZE) == 0 && "User slab must be 64-byte cache line aligned");
        s->slab = config->slab;
        s->ownsSlab = false;
    } else {
        s->slab = CelsAllocAlignedSlab(slabSize);
        assert(s->slab != NULL && "Failed to allocate cache-aligned slab memory");
        s->ownsSlab = true;
    }

    s->slabSize = slabSize;
    memset(s->slab, 0, slabSize);

    /* Determine group and slot capacities */
    if (config && config->maxGroups > 0) {
        s->maxGroups = config->maxGroups;
    } else {
        s->maxGroups = (uint32_t)(slabSize / 128u);
    }
    s->maxGroups = s->maxGroups & ~3u;
    if (s->maxGroups < 16u) {
        s->maxGroups = 16u;
    }

    s->maxSlots = s->maxGroups;

    const size_t groupBytes = s->maxGroups * sizeof(CelsSlotGroup);
    const size_t slotBytes = s->maxSlots * sizeof(CelsSlotAllocation);
    assert(slabSize > groupBytes + slotBytes && "Slab size too small for requested group and slot capacities");

    s->dataArenaSize = slabSize - groupBytes - slotBytes;

    /* Carve partitions from contiguous 64-byte aligned slab */
    s->groups = (CelsSlotGroup *)s->slab;
    s->slots = (CelsSlotAllocation *)((uint8_t *)s->slab + groupBytes);
    s->dataArena = (uint8_t *)s->slab + groupBytes + slotBytes;

    s->groupsGapStart = 0;
    s->groupsGapEnd = s->maxGroups;

    s->dataGapStart = 0;
    s->dataGapEnd = (uint32_t)s->dataArenaSize;
    s->nextGroupId = 1;

    s->magic = CELS_SESSION_MAGIC;
    s->root = config ? config->root : NULL;
    s->engine = config ? config->engine : NULL;
    s->hasComposedOnce = false;
    s->isHotReloadPending = false;
    s->fallbackModuleCount = 0;

    s->maxDrainIterations = (config && config->maxDrainIterations > 0)
        ? config->maxDrainIterations
        : CELS_MAX_DRAIN_ITERATIONS;

    CelsStateRegistryInit(&s->stateRegistry);
}

/**
 * Assigns or replaces the root composable function for the session.
 *
 * @param s      Target session. Non-NULL.
 * @param rootFn Root composable callback function.
 */
void CelsSessionSetRoot(CelsSession *s, CelsRootFn rootFn)
{
    assert(s != NULL);
    s->root = rootFn;
}

/**
 * Tears down a session, releasing all active lifecycle states and slab memory.
 *
 * Invokes onDestroy on all registered lifecycle states, releases slot memory,
 * and frees the internal slab if owned by the session.
 *
 * @param s Target session. NULL is safely ignored.
 */
void CelsSessionDestroy(CelsSession *s)
{
    if (s == NULL) {
        return;
    }

    CelsSession *const prev = s_currentSession;
    s_currentSession = s;

    if (s->maxGroups > 0 && CelsGetLogicalGroupCount(s) > 0) {
        CelsPruneSubtree(s, 0);
    }

    /* Teardown fallback modules only if session does not belong to a host engine */
    if (s->engine == NULL) {
        for (uint32_t i = s->fallbackModuleCount; i > 0; --i) {
            if (s->fallbackModules[i - 1].onDestroy != NULL) {
                s->fallbackModules[i - 1].onDestroy(s->fallbackModules[i - 1].instance);
            }
        }
        s->fallbackModuleCount = 0;
    }

    s_currentSession = (prev == s) ? NULL : prev;

    if (s->ownsSlab && s->slab != NULL) {
        CelsFreeAlignedSlab(s->slab);
    }

    s->magic = 0;
    memset(s, 0, sizeof(*s));
}

/**
 * Attaches an independent top-level composition to the session with a lifecycle evaluator.
 *
 * During recomposition, each attached composition's lifecycle evaluator is checked.
 * If true, the composition is executed; if false, it is pruned from the tree.
 *
 * @param s        Target session. Non-NULL.
 * @param key      Unique composition identifier.
 * @param body     Composition function pointer. Non-NULL.
 * @param eval     Lifecycle evaluator function pointer. May be NULL.
 * @param statePtr Optional user state pointer passed to eval. May be NULL.
 */
void CelsSessionAttachComposition(CelsSession *s, uint64_t key,
                                  void (*body)(CelsSession *s, uint64_t key),
                                  bool (*eval)(void *userData), void *statePtr)
{
    assert(s != NULL);
    assert(body != NULL);

    for (uint32_t i = 0; i < s->attachedCount; ++i) {
        if (s->attachedCompositions[i].key == key) {
            s->attachedCompositions[i].body = body;
            s->attachedCompositions[i].lifecycleEval = eval;
            s->attachedCompositions[i].statePtr = statePtr;
            s->attachedCompositions[i].isAttached = true;
            return;
        }
    }

    if (s->attachedCount >= CELS_MAX_ATTACHED_COMPOSITIONS) {
        fprintf(stderr,
                "[CELS ERROR] Out of session memory: Exceeded CELS_MAX_ATTACHED_COMPOSITIONS (%u).\n",
                CELS_MAX_ATTACHED_COMPOSITIONS);
        // assert(s->attachedCount < CELS_MAX_ATTACHED_COMPOSITIONS && "Exceeded CELS_MAX_ATTACHED_COMPOSITIONS");
        if (s->attachedCount >= CELS_MAX_ATTACHED_COMPOSITIONS) {
            fprintf(stderr, "[CELS ERROR] Exceeded CELS_MAX_ATTACHED_COMPOSITIONS\n");
            return;
        }
        return;
    }
    s->attachedCompositions[s->attachedCount++] = (CelsAttachedComposition){
        .key = key,
        .body = body,
        .lifecycleEval = eval,
        .statePtr = statePtr,
        .isAttached = true
    };
}

/**
 * Detaches an attached composition from the session.
 *
 * @param s   Target session. NULL is safely ignored.
 * @param key Unique composition identifier.
 */
void CelsSessionDetachComposition(CelsSession *s, uint64_t key)
{
    if (s == NULL) {
        return;
    }
    for (uint32_t i = 0; i < s->attachedCount; ++i) {
        if (s->attachedCompositions[i].key == key) {
            s->attachedCompositions[i].isAttached = false;
            s->attachedCompositions[i].body = NULL;
            s->attachedCompositions[i].lifecycleEval = NULL;
            return;
        }
    }
}

/**
 * Executes a recomposition pass over the session tree.
 *
 * Drains pending invalidations, marks affected groups and ancestors, and
 * traverses the hierarchy. Unchanged subtrees are skipped in O(1). Continues
 * in a loop until invalidations settle or maxDrainIterations is reached.
 *
 * @param s Target session. Non-NULL.
 * @return CELS_OK on success, or an error code on invalid state or non-convergence.
 */
CelsResult CelsSessionRecompose(CelsSession *s)
{
    assert(s != NULL);
    if (s->root == NULL && s->attachedCount == 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    if (s->hasComposedOnce && s->queueCount == 0 && !s->isHotReloadPending) {
        return CELS_OK;
    }
    s->isHotReloadPending = false;

    CelsSession *const prevSession = s_currentSession;
    s_currentSession = s;

    uint32_t iterations = 0;
    s->isRecomposing = true;

    do {
        if (++iterations > s->maxDrainIterations) {
            s->isRecomposing = false;
            s_currentSession = prevSession;
            return CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE;
        }

        DrainInvalidationQueue(s);

        s->currentDepth = 0;
        s->currentSlotOffset = 0;
        s->logicalCursor = 0;

        if (s->root != NULL) {
            s->root(s);
        }

        for (uint32_t i = 0; i < s->attachedCount; ++i) {
            CelsAttachedComposition *const comp = &s->attachedCompositions[i];
            if (!comp->isAttached) {
                continue;
            }

            if (comp->statePtr == NULL) {
                comp->statePtr = CelsGetState(s, comp->key);
            }

            bool alive = true;
            if (comp->lifecycleEval != NULL) {
                alive = comp->lifecycleEval(comp->statePtr);
            }

            if (alive) {
                if (CelsEnterComposition(s, comp->key)) {
                    comp->body(s, comp->key);
                }
                CelsExitGroup(s);
                if (comp->statePtr == NULL) {
                    comp->statePtr = CelsGetState(s, comp->key);
                }
            } else {
                CelsPruneSubtreeByKey(s, comp->key);
                comp->statePtr = NULL;
            }
        }

        if (s->currentDepth != 0) {
            s->isRecomposing = false;
            if (prevSession) {
                s_currentSession = prevSession;
            }
            return CELS_ERROR_INVALID_STATE;
        }

    } while (s->queueCount > 0);

    s->hasComposedOnce = true;
    s->isRecomposing = false;
    if (prevSession) {
        s_currentSession = prevSession;
    }
    return CELS_OK;
}

/**
 * Flags all mounted composition groups for re-evaluation on the next recompose pass.
 *
 * Marks all logical groups as CELS_FLAG_INVALIDATED (and ancestors CELS_FLAG_CONTAINS_INVALIDATED)
 * while leaving CELS_FLAG_FRESH_MOUNT cleared, so existing slots, cel_remember memory,
 * and entity IDs are preserved across the hot reload. Also invokes onReload on any
 * registered modules.
 *
 * @param s Target session. Non-NULL.
 */
void CelsSessionHotReload(CelsSession *s)
{
    assert(s != NULL);
    const uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    for (uint32_t i = 0; i < totalGroups; ++i) {
        CelsSlotGroup *const g = CelsGetGroup(s, i);
        if (g != NULL) {
            g->flags |= CELS_FLAG_INVALIDATED;
            if (i > 0) {
                g->flags |= CELS_FLAG_CONTAINS_INVALIDATED;
            }
            g->flags &= ~CELS_FLAG_FRESH_MOUNT;
        }
    }
    s->isHotReloadPending = true;
}

/**
 * Registers an engine subsystem module (SDL, Flecs, Audio, etc.) with the session.
 */
void CelsSessionRegisterModule(CelsSession *s, uint64_t key, const char *name,
                               void *instance,
                               void (*onReload)(void *instance,
                                                CelsSession *session),
                               void (*onDestroy)(void *instance))
{
    (void)onReload;
    assert(s != NULL);
    assert(instance != NULL);

    if (s->engine != NULL) {
        CelsEngineRegisterModule(s->engine, key, name, instance, onDestroy);
        return;
    }

    for (uint32_t i = 0; i < s->fallbackModuleCount; ++i) {
        if (s->fallbackModules[i].key == key) {
            s->fallbackModules[i].name = name;
            s->fallbackModules[i].instance = instance;
            s->fallbackModules[i].onDestroy = onDestroy;
            return;
        }
    }

    assert(s->fallbackModuleCount < CELS_MAX_MODULES && "Exceeded CELS_MAX_MODULES");
    s->fallbackModules[s->fallbackModuleCount++] = (CelsModuleBinding){
        .key = key,
        .name = name,
        .instance = instance,
        .onDestroy = onDestroy
    };
}

/**
 * Retrieves a registered subsystem module pointer by its 64-bit key.
 */
void *CelsSessionGetModule(const CelsSession *s, uint64_t key)
{
    if (s == NULL) {
        return NULL;
    }
    if (s->engine != NULL) {
        return CelsEngineGetModule(s->engine, key);
    }
    for (uint32_t i = 0; i < s->fallbackModuleCount; ++i) {
        if (s->fallbackModules[i].key == key) {
            return s->fallbackModules[i].instance;
        }
    }
    return NULL;
}

/**
 * Prunes a composition subtree, firing cleanups and reclaiming slot memory.
 *
 * Fires onDestroy cleanups in reverse creation order, unsubscribes reactive
 * state watchers, releases arena allocations, and shifts the groups gap buffer.
 *
 * @param s                Target session. Non-NULL.
 * @param rootLogicalIndex Logical group index of the subtree root to remove.
 */
void CelsPruneSubtree(CelsSession *s, uint32_t rootLogicalIndex)
{
    CelsSlotGroup *const root = CelsGetGroup(s, rootLogicalIndex);
    const uint32_t groupsToRemove = 1u + root->groupSize;
    const uint32_t parentIdx = root->parentIndex;

    for (uint32_t i = groupsToRemove; i > 0; --i) {
        const uint32_t targetLogical = rootLogicalIndex + (i - 1u);
        CelsSlotGroup *const g = CelsGetGroup(s, targetLogical);

        FireCleanupsForGroup(s, (uint32_t)g->userData);
        CelsStateRegistryUnsubscribeKey(&s->stateRegistry, g->key);
        ReleaseSlotsForGroup(s, (uint32_t)g->userData);
    }

    if (rootLogicalIndex > 0) {
        uint32_t curr = parentIdx;
        while (true) {
            CelsSlotGroup *const p = CelsGetGroup(s, curr);
            assert(p->groupSize >= groupsToRemove);
            p->groupSize -= (uint16_t)groupsToRemove;

            if (curr == 0) {
                break;
            }
            curr = p->parentIndex;
        }
    }

    const uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    MoveGroupGap(s, totalGroups);

    memmove(&s->groups[rootLogicalIndex],
            &s->groups[rootLogicalIndex + groupsToRemove],
            (totalGroups - rootLogicalIndex - groupsToRemove)
                * sizeof(s->groups[0]));
    s->groupsGapStart -= groupsToRemove;

    for (uint32_t i = rootLogicalIndex; i < s->groupsGapStart; ++i) {
        if (s->groups[i].parentIndex >= rootLogicalIndex + groupsToRemove) {
            s->groups[i].parentIndex -= groupsToRemove;
        }
    }
}

/**
 * Finds an active group by its 64-bit key and prunes its entire subtree.
 *
 * @param s   Target session. NULL is safely ignored.
 * @param key Callsite key of the group to prune.
 */
void CelsPruneSubtreeByKey(CelsSession *s, uint64_t key)
{
    if (s == NULL) {
        return;
    }

    const uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    for (uint32_t i = 0; i < totalGroups; ++i) {
        if (CelsGetGroup(s, i)->key == key) {
            CelsPruneSubtree(s, i);
            return;
        }
    }
}

/**
 * Establishes a root composition scope in the slot table.
 *
 * Initializes the root group (index 0) if the table is empty, or verifies that
 * the existing root group matches rootKey. Must not be nested within another
 * active composition.
 *
 * @param s       Target session. Non-NULL.
 * @param rootKey Stable 64-bit key for the root composition.
 * @return true if the composition should be entered; false on error.
 */
bool CelsEnterComposition(CelsSession *s, uint64_t rootKey)
{
    assert(s->currentDepth == 0 && "CEL_Composition cannot be nested");
    s_currentSession = s;

    if (s->slab == NULL || s->maxGroups == 0) {
        fprintf(stderr, "[CELS ERROR] Out of session memory: Session slab is not initialized.\n");
        return false;
    }

    const uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    if (totalGroups >= s->maxGroups) {
        fprintf(stderr,
                "[CELS ERROR] Out of session memory: Group capacity (%u) reached in %zu-byte slab when mounting root composition (key: 0x%016llX).\n"
                "             Consider increasing slabSize (e.g. CELS_SLAB_48K or CELS_SLAB_64K).\n",
                s->maxGroups, s->slabSize, (unsigned long long)rootKey);
        return false;
    }

    const uint32_t depth = s->currentDepth++;
    s->activeStack[depth] = 1;

    if (totalGroups == 0) {
        MoveGroupGap(s, 0);
        s->groups[0] = (CelsSlotGroup){
            .key = rootKey,
            .userData = s->nextGroupId++,
            .parentIndex = 0,
            .dataOffset = 0,
            .dataSize = 0,
            .groupSize = 0,
            .nodeCount = 0,
            .flags = CELS_FLAG_FRESH_MOUNT
        };
        s->groupsGapStart = 1;
    } else {
        CelsSlotGroup *const root = CelsGetGroup(s, 0);
        if (root->key != rootKey) {
            CelsPruneSubtree(s, 0);
            --s->currentDepth;
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

/**
 * Enters a composable group during traversal, executing reconciliation and skipping.
 *
 * Implements the core traversal algorithm:
 * 1. Synthesizes a key if key == 0.
 * 2. Checks if an existing group at the cursor matches key, or searches sibling groups
 *    and moves them forward if reordered.
 * 3. Evaluates invalidation flags: if clean, increments the cursor past the subtree
 *    in O(1) and returns false (skipping execution).
 * 4. If dirty, clears flags and returns true to run the body.
 * 5. If fresh, inserts a new group into the gap buffer and returns true.
 *
 * @param s   Target session. Non-NULL.
 * @param key Stable 64-bit key, or 0 for auto-synthesized key.
 * @return true if the composable body should execute; false if skipped in O(1).
 */
bool CelsEnterComposable(CelsSession *s, uint64_t key)
{
    assert(s->currentDepth > 0 && "CEL_Composable must be nested within CEL_Composition");
    if (s->currentDepth >= CELS_MAX_DEPTH) {
        fprintf(stderr,
                "[CELS ERROR] Out of session memory: Exceeded maximum composition nesting depth (%u / %u).\n",
                s->currentDepth, CELS_MAX_DEPTH);
        // assert(s->currentDepth < CELS_MAX_DEPTH && "Exceeded CELS_MAX_DEPTH");
        s->activeStack[s->currentDepth] = 0;
        s->currentDepth++;
        return false;
    }

    const uint32_t depth = s->currentDepth++;
    s->slotOffsetStack[depth - 1] = s->currentSlotOffset;
    s->activeStack[depth] = 0;

    const uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    const uint32_t cursor = s->logicalCursor;
    const uint32_t parentIdx = s->groupIndexStack[depth - 1];
    const uint32_t parentEnd = parentIdx + 1 + CelsGetGroup(s, parentIdx)->groupSize;
    uint32_t matchIdx = UINT32_MAX;

    if (key == 0) {
        const uint64_t parentKey = (parentIdx != UINT32_MAX)
            ? CelsGetGroup(s, parentIdx)->key
            : 0xCBF29CE484222325ULL;
        key = CelsKeyIndex(parentKey, ((uint64_t)(cursor - parentIdx) + 1u) * 0x9e3779b97f4a7c15ULL);
    }

    for (uint32_t i = cursor; i < parentEnd;) {
        CelsSlotGroup *const candidate = CelsGetGroup(s, i);
        if (candidate->key == key) {
            matchIdx = i;
            break;
        }
        i += 1u + (uint32_t)candidate->groupSize;
    }

    if (matchIdx != UINT32_MAX && matchIdx != cursor) {
        const uint32_t movedCount = 1u + (uint32_t)CelsGetGroup(s, matchIdx)->groupSize;
        CelsSlotGroup stackMoved[64];
        CelsSlotGroup *moved = (movedCount <= 64)
            ? stackMoved
            : (CelsSlotGroup *)malloc(movedCount * sizeof(CelsSlotGroup));
        assert(moved != NULL);
        MoveGroupGap(s, totalGroups);
        memcpy(moved, &s->groups[matchIdx], movedCount * sizeof(moved[0]));
        memmove(&s->groups[cursor + movedCount],
                &s->groups[cursor],
                (matchIdx - cursor) * sizeof(moved[0]));
        memcpy(&s->groups[cursor], moved, movedCount * sizeof(moved[0]));
        if (moved != stackMoved) {
            free(moved);
        }

        for (uint32_t i = 1; i < totalGroups; ++i) {
            const uint32_t parent = s->groups[i].parentIndex;
            if (parent >= matchIdx && parent < matchIdx + movedCount) {
                s->groups[i].parentIndex = cursor + (parent - matchIdx);
            } else if (parent >= cursor && parent < matchIdx) {
                s->groups[i].parentIndex = parent + movedCount;
            }
        }
    }

    if (matchIdx != UINT32_MAX) {
        CelsSlotGroup *const cached = CelsGetGroup(s, cursor);

        if (!(cached->flags & (CELS_FLAG_INVALIDATED | CELS_FLAG_CONTAINS_INVALIDATED))) {
            s->activeStack[depth] = 0;
            s->groupIndexStack[depth] = cursor;
            s->logicalCursor += (1u + (uint32_t)cached->groupSize);
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

    if (totalGroups >= s->maxGroups) {
        static bool printed = false;
        if (!printed) {
            cel_print_log(ERROR,
                    "[CELS ERROR] Out of session memory: Cannot allocate composable group (key: 0x%016llX).\n"
                    "             Active groups: %u / %u (slab budget: %zu B).\n"
                    "             Consider increasing slabSize (e.g. CELS_SLAB_48K or CELS_SLAB_64K).\n"
                    "             (Further identical allocation errors will be suppressed)",
                    (unsigned long long)key, totalGroups, s->maxGroups, s->slabSize);
            printed = true;
        }
        s->activeStack[depth] = 0;
        return false;
    }
    assert(s->nextGroupId != 0 && "CELS group identity overflow");
    MoveGroupGap(s, cursor);

    s->groups[s->groupsGapStart] = (CelsSlotGroup){
        .key = key,
        .userData = s->nextGroupId++,
        .parentIndex = parentIdx,
        .dataOffset = 0,
        .dataSize = 0,
        .groupSize = 0,
        .nodeCount = 0,
        .flags = CELS_FLAG_FRESH_MOUNT
    };
    s->groupsGapStart++;

    for (uint32_t i = cursor + 1; i <= totalGroups; ++i) {
        CelsSlotGroup *const group = CelsGetGroup(s, i);
        if (group->parentIndex >= cursor) {
            ++group->parentIndex;
        }
    }
    for (uint32_t ancestor = parentIdx;;) {
        CelsSlotGroup *const group = CelsGetGroup(s, ancestor);
        ++group->groupSize;
        if (ancestor == 0) {
            break;
        }
        ancestor = group->parentIndex;
    }

    s->activeStack[depth] = 1;
    s->groupIndexStack[depth] = cursor;
    s->oldGroupSizeStack[depth] = 0;
    s->currentGroupIndex = cursor;
    s->currentSlotOffset = 0;
    s->logicalCursor = cursor + 1;

    return true;
}

/**
 * Exits the current composition group, pruning unvisited children.
 *
 * Checks if the logical cursor reached the expected boundary of the group. Any
 * child groups that were not visited during this pass are pruned. Finalizes the
 * group data size on fresh mount and restores traversal stacks to the parent group.
 *
 * @param s Target session. Non-NULL.
 */
void CelsExitGroup(CelsSession *s)
{
    assert(s->currentDepth > 0 && "Unmatched CEL_Close call");
    const uint32_t depth = --s->currentDepth;
    const uint32_t groupIdx = s->groupIndexStack[depth];

    if (s->activeStack[depth]) {
        uint32_t expectedEnd =
            groupIdx + 1 + CelsGetGroup(s, groupIdx)->groupSize;
        while (s->logicalCursor < expectedEnd
               && s->logicalCursor < CelsGetLogicalGroupCount(s)) {
            CelsSlotGroup *const dead = CelsGetGroup(s, s->logicalCursor);
            const uint32_t removed = 1u + (uint32_t)dead->groupSize;
            CelsPruneSubtree(s, s->logicalCursor);
            expectedEnd -= removed;
        }

        CelsSlotGroup *const g = CelsGetGroup(s, groupIdx);
        if (g->flags & CELS_FLAG_FRESH_MOUNT) {
            g->dataSize = (uint16_t)s->currentSlotOffset;
            g->flags &= ~CELS_FLAG_FRESH_MOUNT;
        }
        assert(g->groupSize == (s->logicalCursor - 1) - groupIdx);
    }

    if (depth > 0) {
        s->currentGroupIndex = s->groupIndexStack[depth - 1];
        s->currentSlotOffset = s->slotOffsetStack[depth - 1];
    }
}

/**
 * Resolves persistent slot memory in the session arena for the active group.
 *
 * On fresh mount: allocates an aligned slot in s->dataArena, seeds it with initVal,
 * and registers lifecycle cleanups if desc is provided.
 * On subsequent passes: locates the previously allocated slot at currentSlotOffset
 * and returns the exact same stable pointer (guaranteeing pinned memory).
 *
 * @param s       Target session. Non-NULL.
 * @param size    Byte size of memory to allocate or resolve.
 * @param initVal Optional pointer to seed data on fresh mount. May be NULL.
 * @param desc    Optional lifecycle descriptor (onCreate/onDestroy). May be NULL.
 * @return Pointer to persistent slot memory in session data arena, or NULL on overflow.
 */
void *CelsResolveSlot(CelsSession *s, size_t size, const void *initVal,
                      const CelsLifecycleDesc *desc)
{
    assert(s->currentDepth > 0);
    CelsSlotGroup *const group = CelsGetGroup(s, s->currentGroupIndex);
    const size_t alignedSize = CELS_ALIGN_UP(size);
    assert(alignedSize > 0 && alignedSize <= UINT16_MAX);

    if (group->flags & CELS_FLAG_FRESH_MOUNT) {
        if (s->slotCount >= s->maxSlots) {
            static bool printed = false;
            if (!printed) {
                cel_print_log(ERROR,
                        "[CELS ERROR] Out of session memory: Slot allocation limit reached (%u / %u slots in %zu-byte slab).\n"
                        "             Consider increasing slabSize (e.g. CELS_SLAB_48K or CELS_SLAB_64K).\n"
                        "             (Further identical allocation errors will be suppressed)",
                        s->slotCount, s->maxSlots, s->slabSize);
                printed = true;
            }
            return NULL;
        }
        uint32_t offset = 0;
        uint32_t insertion = 0;

        if (s->slotCount > 0) {
            CelsSlotAllocation *const last = &s->slots[s->slotCount - 1];
            if (last->arenaOffset + last->size == s->dataGapStart) {
                offset = s->dataGapStart;
                insertion = s->slotCount;
            }
        }

        if (insertion == 0 && s->slotCount > 0) {
            while (insertion < s->slotCount) {
                CelsSlotAllocation *const next = &s->slots[insertion];
                if (offset + alignedSize <= next->arenaOffset) {
                    break;
                }
                offset = next->arenaOffset + next->size;
                ++insertion;
            }
        }

        if (offset + alignedSize > s->dataArenaSize) {
            static bool printed = false;
            if (!printed) {
                cel_print_log(ERROR,
                        "[CELS ERROR] Out of session memory: Slot data arena overflow (requested: %zu B, used: %u B, arena capacity: %zu B in %zu-byte slab).\n"
                        "             Consider increasing slabSize (e.g. CELS_SLAB_48K or CELS_SLAB_64K).\n"
                        "             (Further identical allocation errors will be suppressed)",
                        alignedSize, s->dataGapStart, s->dataArenaSize, s->slabSize);
                printed = true;
            }
            return NULL;
        }

        memmove(&s->slots[insertion + 1],
                &s->slots[insertion],
                (s->slotCount - insertion) * sizeof(s->slots[0]));

        s->slots[insertion] = (CelsSlotAllocation){
            .groupId = (uint32_t)group->userData,
            .slotOffset = s->currentSlotOffset,
            .arenaOffset = offset,
            .size = (uint16_t)alignedSize,
            .userSize = (uint16_t)size
        };
        ++s->slotCount;

        if (s->currentSlotOffset == 0) {
            group->dataOffset = offset;
        }

        uint8_t *const slotPtr = &s->dataArena[offset];
        s->dataGapStart += (uint32_t)alignedSize;

        if (initVal != NULL) {
            memcpy(slotPtr, initVal, size);
        } else {
            memset(slotPtr, 0, size);
        }

        if (desc != NULL) {
            if (s->cleanupCount >= CELS_MAX_CLEANUPS) {
                fprintf(stderr,
                        "[CELS ERROR] Out of session memory: Cleanup hook capacity exceeded (%u / %u).\n",
                        s->cleanupCount, CELS_MAX_CLEANUPS);
                // assert(s->cleanupCount < CELS_MAX_CLEANUPS && "CELS_ERROR_CLEANUP_OVERFLOW");
                return NULL;
            }
            s->cleanups[s->cleanupCount++] = (CelsCleanupHook){
                .groupKey = group->key,
                .groupId = (uint32_t)group->userData,
                .instance = slotPtr,
                .onDestroy = desc->onDestroy
            };
            if (desc->onCreate != NULL) {
                desc->onCreate(slotPtr, s);
            }
        }

        s->currentSlotOffset += (uint32_t)alignedSize;
        return (void *)slotPtr;
    }

    static uint32_t s_slotHint = 0;
    uint32_t start = s_slotHint;
    if (start >= s->slotCount) start = 0;

    for (uint32_t i = 0; i < s->slotCount; ++i) {
        uint32_t idx = start + i;
        if (idx >= s->slotCount) idx -= s->slotCount;
        
        CelsSlotAllocation *const slot = &s->slots[idx];
        if (slot->groupId == (uint32_t)group->userData
            && slot->slotOffset == s->currentSlotOffset) {
            
            s_slotHint = idx + 1;
            
            if (slot->userSize != (uint16_t)size) {
                fprintf(stderr,
                        "[CELS HOT-RELOAD] Struct size changed for group 0x%016llX (was %u B, now %zu B). "
                        "Resetting component to initial state.\n",
                        (unsigned long long)group->key, (unsigned)slot->userSize, size);
                FireCleanupsForGroup(s, (uint32_t)group->userData);
                ReleaseSlotsForGroup(s, (uint32_t)group->userData);
                group->flags |= CELS_FLAG_FRESH_MOUNT;
                s->currentSlotOffset = 0;
                return CelsResolveSlot(s, size, initVal, desc);
            }
            if (desc != NULL && desc->onDestroy != NULL) {
                for (uint32_t c = 0; c < s->cleanupCount; ++c) {
                    if (s->cleanups[c].groupId == (uint32_t)group->userData
                        && s->cleanups[c].instance == (void*)&s->dataArena[slot->arenaOffset]) {
                        s->cleanups[c].onDestroy = desc->onDestroy;
                        break;
                    }
                }
            }
            s->currentSlotOffset += (uint32_t)alignedSize;
            return &s->dataArena[slot->arenaOffset];
        }
    }

    assert(false && "Remembered slot count/order changed");
    return NULL;
}

/**
 * Resolves a live state pointer from the session hierarchy by group key.
 *
 * Traverses active cleanup instances, attached compositions, and group data slots
 * matching key to return the live state pointer. Eliminates global variables.
 *
 * @param s   Target session. May be NULL.
 * @param key Callsite key of the composition group.
 * @return Pointer to state struct, or NULL if not found or session is NULL.
 */
void *CelsGetState(CelsSession *s, uint64_t key)
{
    if (s == NULL) {
        return NULL;
    }

    /* 1. Check active cleanups (lifecycle states) */
    for (uint32_t i = 0; i < s->cleanupCount; ++i) {
        if (s->cleanups[i].groupKey == key) {
            return s->cleanups[i].instance;
        }
    }

    /* 2. Check attached compositions with matching key and state pointer */
    for (uint32_t i = 0; i < s->attachedCount; ++i) {
        if (s->attachedCompositions[i].key == key && s->attachedCompositions[i].statePtr != NULL) {
            return s->attachedCompositions[i].statePtr;
        }
    }

    /* 3. Check general group data slot if present */
    const uint32_t groupCount = CelsGetLogicalGroupCount(s);
    for (uint32_t i = 0; i < groupCount; ++i) {
        const CelsSlotGroup *const g = CelsGetGroup(s, i);
        if (g != NULL && g->key == key && g->slotCount > 0) {
            return (void *)&s->dataArena[g->slotIndex];
        }
    }

    return NULL;
}

/**
 * Alias for CelsGetState for backwards compatibility.
 *
 * @param s   Target session. May be NULL.
 * @param key Callsite key of the composition group.
 * @return Pointer to state struct, or NULL if not found.
 */
void *CelsFindLifecycleState(CelsSession *s, uint64_t key)
{
    return CelsGetState(s, key);
}

/**
 * Alias for CelsGetState for backwards compatibility.
 *
 * @param s   Target session. May be NULL.
 * @param key Callsite key of the composition group.
 * @return Pointer to state struct, or NULL if not found.
 */
void *CelsFindObserver(CelsSession *s, uint64_t key)
{
    return CelsGetState(s, key);
}
