#include "cels/runtime/session.h"
#include "cels/engine.h"
#include "cels/runtime/module.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_MSC_VER)
#include <malloc.h>
#endif

#include "cels/runtime/thread.h"
#include "cels/runtime/slot_table.h"
#include "cels/runtime/state.h"
#include "cels/runtime/log.h"

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
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    void *ptr = NULL;
    if (posix_memalign(&ptr, CELS_CACHE_LINE_SIZE, size) != 0) {
        return NULL;
    }
    return ptr;
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

        (void)first;
        (void)end;

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

#ifndef CELS_MAX_SESSIONS
#define CELS_MAX_SESSIONS 32u
#endif

typedef struct CelsSessionRegistryEntry {
    CEL_Id id;
    CEL_Session *session;
} CelsSessionRegistryEntry;

static CelsSessionRegistryEntry s_sessionRegistry[CELS_MAX_SESSIONS];
static uint32_t s_sessionRegistryCount = 0;

/**
 * Dynamically allocates and initializes a new heap-backed CEL_Session.
 *
 * Configures the session slab according to options, assigns sessionId, marks
 * the session as heap-allocated, and records it in the global session registry.
 *
 * @param sessionId Unique 64-bit session identifier.
 * @param options   Optional capacity profile and memory sizing overrides. Safe if NULL.
 * @return Pointer to initialized CEL_Session instance.
 */
CEL_Session *CelSessionCreate(CEL_Id sessionId, const CelSessionOptions *options)
{
    CEL_Session *session = (CEL_Session *)calloc(1, sizeof(CEL_Session));
    assert(session != NULL);
    CelsSessionConfig config = {0};
    if (options) {
        config.profile = options->profile;
        config.maxComposables = options->maxComposables;
        config.slabSize = options->slabCapacityBytes;
    }
    CelsSessionInit(session, &config);
    session->id = sessionId;
    session->isHeapAllocated = true;

    /* Register in session table */
    if (s_sessionRegistryCount < CELS_MAX_SESSIONS) {
        s_sessionRegistry[s_sessionRegistryCount++] = (CelsSessionRegistryEntry){
            .id = sessionId,
            .session = session
        };
    }
    return session;
}

/**
 * Creates and initializes a heap-backed CEL_Session dimensioned for a workload capacity profile.
 *
 * @param sessionId Unique 64-bit session identifier.
 * @param profile   Target capacity profile (e.g. CELS_PROFILE_1K, CELS_PROFILE_512).
 * @return Pointer to initialized CEL_Session instance.
 */
CEL_Session *CelSessionCreateWithProfile(CEL_Id sessionId, CelsSessionProfile profile)
{
    CelSessionOptions opts = {
        .profile = profile,
        .maxComposables = CelsMaxComposablesFromProfile(profile),
        .slabCapacityBytes = CelsSlabSizeFromProfile(profile),
    };
    return CelSessionCreate(sessionId, &opts);
}

/**
 * Looks up an active session from the global session registry by its 64-bit identifier.
 *
 * @param sessionId Unique 64-bit session identifier.
 * @return Pointer to matching CEL_Session, or NULL if not found.
 */
CEL_Session *cel_session(CEL_Id sessionId)
{
    for (uint32_t i = 0; i < s_sessionRegistryCount; ++i) {
        if (s_sessionRegistry[i].id == sessionId) {
            return s_sessionRegistry[i].session;
        }
    }
    return NULL;
}

/**
 * Resolves the currently active ambient session bound to the calling thread.
 *
 * @return Pointer to active CEL_Session, or NULL if none is active.
 */
CEL_Session *cel_active_session(void)
{
    return CelsGetCurrentSession();
}

/**
 * Destroys a session, releasing all slot memory, subscriptions, and registered modules.
 *
 * Removes the session from the global registry, destroys the session slab and slot table,
 * and frees the session structure if it was heap-allocated.
 *
 * @param session Target session to destroy. Safe if NULL.
 */
void CelSessionDestroy(CEL_Session *session)
{
    if (session == NULL) return;
    for (uint32_t i = 0; i < s_sessionRegistryCount; ++i) {
        if (s_sessionRegistry[i].session == session) {
            --s_sessionRegistryCount;
            memmove(&s_sessionRegistry[i],
                    &s_sessionRegistry[i + 1],
                    (s_sessionRegistryCount - i) * sizeof(s_sessionRegistry[0]));
            break;
        }
    }
    bool wasHeap = session->isHeapAllocated;
    CelsSessionDestroy(session);
    if (wasHeap) {
        free(session);
    }
}

/**
 * Executes a recomposition pass over the specified session.
 *
 * @param session Target session. Non-NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelSessionRecompose(CEL_Session *session)
{
    return CelsSessionRecompose(session);
}

/**
 * Triggers a recomposition pass across all active sessions in the global registry.
 *
 * @return CELS_OK if all sessions recomposed successfully; otherwise returns the last error code.
 */
CelsResult CelsRecomposeAllSessions(void)
{
    CelsResult lastRes = CELS_OK;
    for (uint32_t i = 0; i < s_sessionRegistryCount; ++i) {
        if (s_sessionRegistry[i].session != NULL && s_sessionRegistry[i].session->attachedCount > 0) {
            CelsResult res = CelsSessionRecompose(s_sessionRegistry[i].session);
            if (res != CELS_OK) {
                lastRes = res;
            }
        }
    }
    return lastRes;
}

/**
 * Allocates a 64-byte cache-aligned raw memory buffer from the session slab's high-water data arena.
 *
 * Moves dataGapEnd downward in O(1) time without heap allocation.
 *
 * @param s    Target session. Safe if NULL.
 * @param size Byte size of memory requested.
 * @return Cache-aligned pointer to allocated memory in slab, or NULL on arena overflow.
 */
void *CelsSessionAllocData(CelsSession *s, size_t size)
{
    if (s == NULL || size == 0) return NULL;
    const size_t aligned = (size + CELS_CACHE_LINE_SIZE - 1u) & ~(CELS_CACHE_LINE_SIZE - 1u);
    if (s->dataGapStart + aligned > s->dataGapEnd) {
        cel_print_log(ERROR, "[CELS ERROR] Out of session memory: Data arena overflow in slab.\n");
        return NULL;
    }
    s->dataGapEnd -= (uint32_t)aligned;
    return &s->dataArena[s->dataGapEnd];
}

/**
 * Double-buffered reactive state lookup/allocation helper for legacy macros.
 *
 * @param s          Target session. Safe if NULL (resolves ambient session).
 * @param id         Unique 64-bit state identifier.
 * @param size       State struct size in bytes.
 * @param defaultVal Optional initial state payload. Safe if NULL.
 * @return Pointer to published front buffer payload.
 */
void *CelsSessionRememberState(CEL_Session *s, CEL_Id id, size_t size, const void *defaultVal)
{
    if (s == NULL) {
        s = CelsGetCurrentSession();
    }
    if (s == NULL || id == 0) return NULL;
    CelsStateCell *cell = CelsStateGetOrCreateCell(s, id, size, defaultVal);
    return cell ? cell->frontBuffer : NULL;
}

/**
 * Returns the recommended slab byte capacity for a session capacity profile.
 *
 * @param profile Target workload capacity profile.
 * @return Total slab size in bytes (e.g. 16 KiB, 64 KiB, 512 KiB).
 */
size_t CelsSlabSizeFromProfile(CelsSessionProfile profile)
{
    switch (profile) {
    case CELS_PROFILE_128: return CELS_SLAB_16K;
    case CELS_PROFILE_256: return CELS_SLAB_32K;
    case CELS_PROFILE_512: return CELS_SLAB_64K;
    case CELS_PROFILE_1K:  return CELS_SLAB_128K;
    case CELS_PROFILE_2K:  return CELS_SLAB_256K;
    case CELS_PROFILE_4K:  return CELS_SLAB_512K;
    case CELS_PROFILE_8K:  return CELS_SLAB_1M;
    case CELS_PROFILE_DEFAULT:
    default:
        return (size_t)CELS_DEFAULT_SLAB_SIZE;
    }
}

/**
 * Returns the maximum composable group limit for a session capacity profile.
 *
 * @param profile Target workload capacity profile.
 * @return Maximum supported composable node count.
 */
uint32_t CelsMaxComposablesFromProfile(CelsSessionProfile profile)
{
    switch (profile) {
    case CELS_PROFILE_128: return 128u;
    case CELS_PROFILE_256: return 256u;
    case CELS_PROFILE_512: return 512u;
    case CELS_PROFILE_1K:  return 1024u;
    case CELS_PROFILE_2K:  return 2048u;
    case CELS_PROFILE_4K:  return 4096u;
    case CELS_PROFILE_8K:  return 8192u;
    case CELS_PROFILE_DEFAULT:
    default:
        return 4096u;
    }
}

/**
 * Generates a pre-dimensioned CelsSessionConfig matching a workload capacity profile.
 *
 * @param profile Target workload capacity profile.
 * @return Populated CelsSessionConfig struct with optimal slab size and group limits.
 */
CelsSessionConfig CelsSessionProfileConfig(CelsSessionProfile profile)
{
    CelsSessionConfig cfg = {0};
    cfg.profile = profile;
    cfg.maxComposables = CelsMaxComposablesFromProfile(profile);
    cfg.slabSize = CelsSlabSizeFromProfile(profile);
    cfg.maxGroups = cfg.maxComposables;
    return cfg;
}

/**
 * Generates an auto-scaled CelsSessionConfig dimensioned for a target composable count.
 *
 * Selects the smallest capacity profile that fits maxComposables to minimize memory footprint.
 *
 * @param maxComposables Estimated or required composable count.
 * @return Populated CelsSessionConfig struct.
 */
CelsSessionConfig CelsSessionCapacityConfig(uint32_t maxComposables)
{
    CelsSessionConfig cfg = {0};
    cfg.maxComposables = maxComposables;
    CelsSessionProfile prof;
    if (maxComposables <= 128u) {
        prof = CELS_PROFILE_128;
    } else if (maxComposables <= 256u) {
        prof = CELS_PROFILE_256;
    } else if (maxComposables <= 512u) {
        prof = CELS_PROFILE_512;
    } else if (maxComposables <= 1024u) {
        prof = CELS_PROFILE_1K;
    } else if (maxComposables <= 2048u) {
        prof = CELS_PROFILE_2K;
    } else if (maxComposables <= 4096u) {
        prof = CELS_PROFILE_4K;
    } else if (maxComposables <= 8192u) {
        prof = CELS_PROFILE_8K;
    } else {
        prof = CELS_PROFILE_DEFAULT;
    }
    cfg.profile = prof;
    if (maxComposables <= 8192u) {
        cfg.slabSize = CelsSlabSizeFromProfile(prof);
        cfg.maxGroups = CelsMaxComposablesFromProfile(prof);
    } else {
        cfg.slabSize = (size_t)maxComposables * 128u;
        cfg.maxGroups = maxComposables;
    }
    return cfg;
}

/**
 * Initializes a CelsSession instance configured for a specific capacity profile.
 *
 * @param session Target session struct. Non-NULL.
 * @param profile Target workload capacity profile.
 */
void CelsSessionInitWithProfile(CelsSession *session, CelsSessionProfile profile)
{
    CelsSessionConfig cfg = CelsSessionProfileConfig(profile);
    CelsSessionInit(session, &cfg);
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

    size_t slabSize = 0;
    uint32_t maxGroups = 0;

    if (config && config->slabSize > 0) {
        slabSize = config->slabSize;
        if (config->maxComposables > 0) {
            maxGroups = config->maxComposables;
        } else if (config->maxGroups > 0) {
            maxGroups = config->maxGroups;
        } else if (config->profile != CELS_PROFILE_DEFAULT) {
            maxGroups = CelsMaxComposablesFromProfile(config->profile);
        } else {
            maxGroups = (uint32_t)(slabSize / 128u);
        }
    } else if (config && config->maxComposables > 0) {
        CelsSessionConfig capCfg = CelsSessionCapacityConfig(config->maxComposables);
        slabSize = capCfg.slabSize;
        maxGroups = capCfg.maxGroups;
    } else if (config && config->profile != CELS_PROFILE_DEFAULT) {
        slabSize = CelsSlabSizeFromProfile(config->profile);
        maxGroups = CelsMaxComposablesFromProfile(config->profile);
    } else if (config && config->maxGroups > 0) {
        CelsSessionConfig capCfg = CelsSessionCapacityConfig(config->maxGroups);
        slabSize = capCfg.slabSize;
        maxGroups = capCfg.maxGroups;
    } else {
        slabSize = (size_t)CELS_DEFAULT_SLAB_SIZE;
        maxGroups = (uint32_t)(slabSize / 128u);
    }

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
    s->maxGroups = maxGroups;
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
    s->engine = config ? config->engine : NULL;
    s->hasComposedOnce = false;
    s->isHotReloadPending = false;
    s->isHeapAllocated = false;
    s->fallbackModuleCount = 0;

    s->maxDrainIterations = (config && config->maxDrainIterations > 0)
        ? config->maxDrainIterations
        : CELS_MAX_DRAIN_ITERATIONS;

    CelsStateRegistryInit(&s->stateRegistry);
    CelsEventQueueInit(&s->eventQueue);
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

    CelsEventQueueClear(&s->eventQueue);
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
void CelsSessionAttachComposition(CEL_Session *s,
                                  CEL_Id key,
                                  void (*body)(void *userData),
                                  void *userData,
                                  bool (*eval)(void *evalCtx),
                                  void *evalCtx)
{
    assert(s != NULL);
    assert(body != NULL);

    for (uint32_t i = 0; i < s->attachedCount; ++i) {
        if (s->attachedCompositions[i].key == key) {
            s->attachedCompositions[i].body = body;
            s->attachedCompositions[i].userData = userData;
            s->attachedCompositions[i].lifecycleEval = eval;
            s->attachedCompositions[i].evalCtx = evalCtx;
            s->attachedCompositions[i].isAttached = true;
            return;
        }
    }

    if (s->attachedCount >= CELS_MAX_ATTACHED_COMPOSITIONS) {
        cel_print_log(ERROR, "[CELS ERROR] Exceeded CELS_MAX_ATTACHED_COMPOSITIONS\n");
        return;
    }
    s->attachedCompositions[s->attachedCount++] = (CelsAttachedComposition){
        .key = key,
        .body = body,
        .userData = userData,
        .lifecycleEval = eval,
        .evalCtx = evalCtx,
        .isAttached = true
    };
}

/**
 * Associates lifecycle callback hooks with the currently active composable group.
 *
 * Records the group key, group ID, instance pointer, and destructor callback into
 * the session's cleanup array. If onCreate is non-NULL, invokes it immediately.
 * When the group is later pruned or destroyed, onDestroy will be executed to reclaim resources.
 *
 * @param s         Active session instance. Safe if NULL or outside composition.
 * @param instance  User state or resource instance to manage with lifecycle callbacks. Safe if NULL.
 * @param onCreate  Optional constructor callback invoked immediately upon registration. May be NULL.
 * @param onDestroy Optional destructor callback invoked when the associated group is unmounted. May be NULL.
 */
void CelsSessionRegisterLifecycle(CelsSession *s, void *instance, void (*onCreate)(void *, CelsSession *), void (*onDestroy)(void *, CelsSession *))
{
    if (s == NULL || s->currentDepth == 0) return;

    CelsSlotGroup *const group = CelsGetGroup(s, s->currentGroupIndex);
    
    if (s->cleanupCount >= CELS_MAX_CLEANUPS) {
        fprintf(stderr,
                "[CELS ERROR] Out of session memory: Cleanup hook capacity exceeded (%u / %u).\n",
                s->cleanupCount, CELS_MAX_CLEANUPS);
        return;
    }
    
    s->cleanups[s->cleanupCount++] = (CelsCleanupHook){
        .groupKey = group->key,
        .groupId = (uint32_t)group->userData,
        .instance = instance,
        .onDestroy = onDestroy
    };
    
    if (onCreate != NULL) {
        onCreate(instance, s);
    }
}

/**
 * Updates the destructor callback and instance pointer for an active lifecycle hook.
 *
 * Searches existing cleanup entries matching the current group's ID or key and refreshes
 * their instance and onDestroy function pointers. This prevents stale code addresses after
 * dynamic module hot-reload without re-triggering constructors. If no matching entry exists,
 * registers a new lifecycle hook without invoking onCreate.
 *
 * @param s         Active session instance. Safe if NULL or outside composition.
 * @param instance  Updated user state or resource instance pointer.
 * @param onDestroy Updated destructor callback pointer to execute when the group unmounts. May be NULL.
 */
void CelsSessionUpdateLifecycle(CelsSession *s, void *instance, void (*onDestroy)(void *, CelsSession *))
{
    if (s == NULL || s->currentDepth == 0) return;

    CelsSlotGroup *const group = CelsGetGroup(s, s->currentGroupIndex);
    const uint32_t gid = (uint32_t)group->userData;
    const uint64_t key = group->key;

    for (uint32_t i = 0; i < s->cleanupCount; ++i) {
        if (s->cleanups[i].groupId == gid || s->cleanups[i].groupKey == key) {
            s->cleanups[i].groupId = gid;
            s->cleanups[i].groupKey = key;
            s->cleanups[i].instance = instance;
            s->cleanups[i].onDestroy = onDestroy;
            return;
        }
    }

    CelsSessionRegisterLifecycle(s, instance, NULL, onDestroy);
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
            s->attachedCompositions[i].userData = NULL;
            s->attachedCompositions[i].lifecycleEval = NULL;
            s->attachedCompositions[i].evalCtx = NULL;
            return;
        }
    }
}

bool CelsSessionNeedsRecompose(const CelsSession *s)
{
    if (s == NULL) {
        return false;
    }
    if (!s->hasComposedOnce) {
        return true;
    }
    if (s->queueCount > 0 || s->nextFrameQueueCount > 0) {
        return true;
    }
    if (s->isHotReloadPending) {
        return true;
    }
    if (s->eventQueue.count > 0) {
        return true;
    }
    for (uint32_t i = 0; i < s->stateRegistry.cellCount; ++i) {
        if (s->stateRegistry.cells[i].inUse && s->stateRegistry.cells[i].isDirty) {
            return true;
        }
    }
    return false;
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
    if (s->attachedCount == 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    /* Transfer next-frame task/external invalidations to active invalidation queue */
    while (s->nextFrameQueueCount > 0 && s->queueCount < CELS_MAX_QUEUE) {
        s->invalidationQueue[s->queueCount++] = s->nextFrameQueue[--s->nextFrameQueueCount];
    }

    bool hasDirtyState = false;
    for (uint32_t i = 0; i < s->stateRegistry.cellCount; ++i) {
        if (s->stateRegistry.cells[i].inUse && s->stateRegistry.cells[i].isDirty) {
            hasDirtyState = true;
            break;
        }
    }

    if (s->hasComposedOnce && s->queueCount == 0 && !s->isHotReloadPending && !hasDirtyState) {
        return CELS_OK;
    }
    s->isHotReloadPending = false;

    CelsSession *const prevSession = s_currentSession;
    s_currentSession = s;

    uint32_t iterations = 0;
    s->isRecomposing = true;
    s->isHandlingEvent = false;

    do {
        if (++iterations > s->maxDrainIterations) {
            s->isRecomposing = false;
            s->isHandlingEvent = false;
            s_currentSession = prevSession;
            return CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE;
        }

        /* Publish double-buffered state snapshots before each evaluation pass */
        CelsStatePublishDirty(s);

        DrainInvalidationQueue(s);

        s->currentDepth = 0;
        s->currentSlotOffset = 0;
        s->logicalCursor = 0;

        for (uint32_t i = 0; i < s->attachedCount; ++i) {
            CelsAttachedComposition *const comp = &s->attachedCompositions[i];
            if (!comp->isAttached) {
                continue;
            }

            bool alive = true;
            if (comp->lifecycleEval != NULL) {
                alive = comp->lifecycleEval(comp->evalCtx);
            }

            if (alive) {
                if (CelsEnterComposition(s, comp->key)) {
                    comp->body(comp->userData);
                }
                CelsExitGroup(s);
            } else {
                CelsPruneSubtreeByKey(s, comp->key);
                if (s->engine != NULL && s == &s->engine->session) {
                    uint64_t rootKey = (s->engine->appModule != NULL && s->engine->appModule->attachedKey != 0)
                        ? s->engine->appModule->attachedKey
                        : (s->attachedCount > 0 ? s->attachedCompositions[0].key : 0);
                    if (comp->key == rootKey) {
                        s->engine->shouldQuit = true;
                    }
                }
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

    /* Transfer next-frame task invalidations to queue for the next recompose pass */
    while (s->nextFrameQueueCount > 0 && s->queueCount < CELS_MAX_QUEUE) {
        s->invalidationQueue[s->queueCount++] = s->nextFrameQueue[--s->nextFrameQueueCount];
    }

    /* Publish double-buffered state snapshots at frame boundary */
    CelsStatePublishDirty(s);

    /* Post-recomposition frame completion hook (e.g. Flecs or worker commit) */
    if (s->postRecomposeHook != NULL) {
        s->postRecomposeHook(s, s->postRecomposeUserData);
    }

    /* Retire consumed events at frame completion */
    CelsEventRetireConsumed(s);

    s->hasComposedOnce = true;
    s->isRecomposing = false;
    s->isHandlingEvent = false;
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
 *
 * If the session belongs to a host CelsEngine, delegates registration to CelsEngineRegisterModule.
 * Otherwise, records or updates the binding in the session's internal fallback module table.
 *
 * @param s         Target session. Non-NULL.
 * @param key       Unique 64-bit type or subsystem identifier (e.g. CELS_TYPE_KEY(SDL_Renderer)).
 * @param name      Human-readable name of the module for diagnostics. Safe if NULL.
 * @param instance  Subsystem module instance pointer to bind. Non-NULL.
 * @param onReload  Optional callback invoked during module hot-reload. May be NULL.
 * @param onDestroy Optional destructor callback invoked when the session or module is torn down. May be NULL.
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
 *
 * If the session is associated with an engine host, queries CelsEngineGetModule.
 * Otherwise, searches the session's fallback module table.
 *
 * @param s   Target session. Safe if NULL.
 * @param key Unique 64-bit subsystem or component identifier.
 * @return Pointer to registered subsystem module instance, or NULL if not found or session is NULL.
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
            .slotIndex = 0,
            .slotCount = 0,
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
        .slotIndex = 0,
        .slotCount = 0,
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
            g->slotCount = (uint16_t)s->currentSlotOffset;
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
void *CelsResolveSlot(CelsSession *s, size_t size, const void *initVal)
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

        if (offset + alignedSize > s->dataGapEnd) {
            static bool printed = false;
            if (!printed) {
                cel_print_log(ERROR,
                        "[CELS ERROR] Out of session memory: Slot data arena overflow (requested: %zu B, used: %u B, arena capacity: %u B in %zu-byte slab).\n"
                        "             Consider increasing slabSize (e.g. CELS_SLAB_48K or CELS_SLAB_64K).\n"
                        "             (Further identical allocation errors will be suppressed)",
                        alignedSize, s->dataGapStart, s->dataGapEnd, s->slabSize);
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
            group->slotIndex = offset;
        }

        uint8_t *const slotPtr = &s->dataArena[offset];
        s->dataGapStart += (uint32_t)alignedSize;

        if (initVal != NULL) {
            memcpy(slotPtr, initVal, size);
        } else {
            memset(slotPtr, 0, size);
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
                return CelsResolveSlot(s, size, initVal);
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
        s = CelsGetCurrentSession();
    }
    if (s == NULL || key == 0) {
        return NULL;
    }

    /* 1. Check double-buffered reactive state registry */
    CelsStateCell *cell = CelsStateRegistryFindCell(&s->stateRegistry, key);
    if (cell != NULL && cell->inUse) {
        return cell->frontBuffer;
    }

    /* 2. Check active cleanups (lifecycle states) */
    for (uint32_t i = 0; i < s->cleanupCount; ++i) {
        if (s->cleanups[i].groupKey == key) {
            return s->cleanups[i].instance;
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
 * Queues a 64-bit group key for invalidation and recomposition on the next pass.
 *
 * Appends the key to the next-frame queue if not already present.
 *
 * @param session Target session. Safe if NULL.
 * @param key     Unique 64-bit group key to invalidate.
 */
void CelsSessionInvalidateKey(CelsSession *session, uint64_t key)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL) return;
    for (uint32_t i = 0; i < session->nextFrameQueueCount; ++i) {
        if (session->nextFrameQueue[i] == key) return;
    }
    if (session->nextFrameQueueCount < CELS_MAX_QUEUE) {
        session->nextFrameQueue[session->nextFrameQueueCount++] = key;
    }
}

/**
 * Queues a 64-bit group key for immediate invalidation in the active recomposition pass.
 *
 * If recomposition is actively in progress, pushes the key directly to the active invalidation
 * queue to schedule another drain iteration in the current frame. Otherwise, delegates to
 * CelsSessionInvalidateKey to schedule it for the next frame.
 *
 * @param session Target session. Can be NULL (falls back to ambient session).
 * @param key     Unique 64-bit group key to invalidate immediately.
 */
void CelsSessionInvalidateKeyImmediate(CelsSession *session, uint64_t key)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL) return;
    if (session->isRecomposing) {
        for (uint32_t i = 0; i < session->queueCount; ++i) {
            if (session->invalidationQueue[i] == key) return;
        }
        if (session->queueCount < CELS_MAX_QUEUE) {
            session->invalidationQueue[session->queueCount++] = key;
        }
        return;
    }
    CelsSessionInvalidateKey(session, key);
}

/**
 * Returns the stable 64-bit key of the currently executing composable group.
 *
 * Inspects the current composition stack depth and returns the callsites or synthesized
 * key of the active composable group in the session hierarchy.
 *
 * @param session Target session. Can be NULL (falls back to ambient session).
 * @return Unique 64-bit key of the current group, or 0 if outside any composition or session is NULL.
 */
uint64_t CelsSessionGetCurrentGroupKey(const CelsSession *session)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL || session->currentDepth == 0) {
        return 0;
    }
    const CelsSlotGroup *group = CelsGetGroup((CelsSession*)session, session->currentGroupIndex);
    return group ? group->key : 0;
}

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
                         const void *data)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL) return false;

    CelsTransactionBatch *batch = &session->transactionBatches[session->activeBatchIndex];
    if (batch->opCount >= CELS_MAX_TRANSACTIONS) return false;
    if (batch->dataSize + size > CELS_TRANSACTION_DATA_SIZE) return false;

    uint32_t offset = batch->dataSize;
    if (size > 0 && data != NULL) {
        memcpy(&batch->data[offset], data, size);
        batch->dataSize += (uint32_t)size;
    }

    batch->ops[batch->opCount++] = (CelsTransactionOp){
        .opCode = CELS_OP_SET,
        .size = (uint32_t)size,
        .targetId = targetId,
        .typeKey = typeKey,
        .dataOffset = offset
    };
    return true;
}

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
                            uint64_t typeKey)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL) return false;

    CelsTransactionBatch *batch = &session->transactionBatches[session->activeBatchIndex];
    if (batch->opCount >= CELS_MAX_TRANSACTIONS) return false;

    batch->ops[batch->opCount++] = (CelsTransactionOp){
        .opCode = CELS_OP_REMOVE,
        .size = 0,
        .targetId = targetId,
        .typeKey = typeKey,
        .dataOffset = 0
    };
    return true;
}

/**
 * Stages a target deletion/destruction in the session's active transaction batch.
 *
 * @param session  Target session. Can be NULL (falls back to ambient session).
 * @param targetId Target entity or resource identifier.
 * @return True if staged successfully; false if batch capacity is full.
 */
bool CelsSessionStageDelete(CelsSession *session, uint64_t targetId)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL) return false;

    CelsTransactionBatch *batch = &session->transactionBatches[session->activeBatchIndex];
    if (batch->opCount >= CELS_MAX_TRANSACTIONS) return false;

    batch->ops[batch->opCount++] = (CelsTransactionOp){
        .opCode = CELS_OP_DELETE,
        .size = 0,
        .targetId = targetId,
        .typeKey = 0,
        .dataOffset = 0
    };
    return true;
}

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
                            const void *data)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL) return false;

    CelsTransactionBatch *batch = &session->transactionBatches[session->activeBatchIndex];
    if (batch->opCount >= CELS_MAX_TRANSACTIONS) return false;
    if (batch->dataSize + size > CELS_TRANSACTION_DATA_SIZE) return false;

    uint32_t offset = batch->dataSize;
    if (size > 0 && data != NULL) {
        memcpy(&batch->data[offset], data, size);
        batch->dataSize += (uint32_t)size;
    }

    batch->ops[batch->opCount++] = (CelsTransactionOp){
        .opCode = opCode,
        .size = (uint32_t)size,
        .targetId = targetId,
        .typeKey = typeKey,
        .dataOffset = offset
    };
    return true;
}

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
                                       void *userData)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL || handler == NULL) return 0;

    CelsTransactionBatch *batch = &session->transactionBatches[session->activeBatchIndex];
    uint32_t count = batch->opCount;
    for (uint32_t i = 0; i < count; ++i) {
        const CelsTransactionOp *op = &batch->ops[i];
        const void *payload = (op->size > 0) ? &batch->data[op->dataOffset] : NULL;
        handler((CelsOpCode)op->opCode, op->targetId, op->typeKey, payload, (size_t)op->size, userData);
    }

    batch->opCount = 0;
    batch->dataSize = 0;
    return count;
}

/**
 * Swaps active and ready transaction batches for lockless handoff to worker threads.
 *
 * The previously active batch becomes ready for consumption via CelsSessionGetReadyBatch,
 * and the new active batch is reset for new staging operations.
 *
 * @param session Target session. Safe if NULL.
 */
void CelsSessionSwapTransactionBatches(CelsSession *session)
{
    if (session == NULL) return;
    session->activeBatchIndex = 1u - session->activeBatchIndex;
    session->transactionBatches[session->activeBatchIndex].opCount = 0;
    session->transactionBatches[session->activeBatchIndex].dataSize = 0;
}

/**
 * Returns a pointer to the ready transaction batch for consumption by worker threads.
 *
 * @param session Target session. Can be NULL.
 * @return Read-only pointer to the ready batch, or NULL if session is NULL.
 */
const CelsTransactionBatch *CelsSessionGetReadyBatch(const CelsSession *session)
{
    if (session == NULL) return NULL;
    uint32_t readyIndex = 1u - session->activeBatchIndex;
    return &session->transactionBatches[readyIndex];
}

/**
 * Resets the ready transaction batch after worker threads finish processing.
 *
 * @param session Target session. Safe if NULL.
 */
void CelsSessionClearReadyBatch(CelsSession *session)
{
    if (session == NULL) return;
    uint32_t readyIndex = 1u - session->activeBatchIndex;
    session->transactionBatches[readyIndex].opCount = 0;
    session->transactionBatches[readyIndex].dataSize = 0;
}

/**
 * Gets the session's user context pointer.
 *
 * @param session Target session. Can be NULL.
 * @return User context pointer, or NULL if session is NULL or unset.
 */
void *CelsSessionGetUserData(const CelsSession *session)
{
    return session ? session->userData : NULL;
}

/**
 * Sets the session's user context pointer.
 *
 * @param session  Target session. Safe if NULL.
 * @param userData Pointer to arbitrary user application context.
 */
void CelsSessionSetUserData(CelsSession *session, void *userData)
{
    if (session != NULL) {
        session->userData = userData;
    }
}

/**
 * Registers a callback invoked immediately after CelsSessionRecompose finishes frame convergence.
 *
 * @param session  Target session. Safe if NULL.
 * @param hook     Callback invoked at frame completion. Can be NULL to clear.
 * @param userData Context pointer passed to hook. Can be NULL.
 */
void CelsSessionSetPostRecomposeHook(CelsSession *session,
                                     CelsPostRecomposeFn hook,
                                     void *userData)
{
    if (session != NULL) {
        session->postRecomposeHook = hook;
        session->postRecomposeUserData = userData;
    }
}

/**
 * Allocates or resolves persistent slot memory with an optional unmount cleanup callback.
 *
 * On first mount, registers the cleanup hook. On subsequent remounts, updates the
 * cleanup hook pointer to prevent stale code addresses after hot-reload.
 *
 * @param s         Active session instance. Non-NULL.
 * @param size      Byte size of the requested slot allocation.
 * @param initVal   Pointer to initial data bytes, or NULL for zero-initialization.
 * @param onDestroy Destructor callback invoked when slot is unmounted or session destroyed. Safe if NULL.
 * @return Stable pointer to slot memory in session data arena, or NULL on allocation failure.
 */
void *CelsResolveSlotWithCleanup(CelsSession *s,
                                 size_t size,
                                 const void *initVal,
                                 void (*onDestroy)(void *ptr, CelsSession *session))
{
    void *ptr = CelsResolveSlot(s, size, initVal);
    if (ptr != NULL && onDestroy != NULL && s != NULL && s->currentDepth > 0) {
        CelsSlotGroup *const group = CelsGetGroup(s, s->currentGroupIndex);
        if (group->flags & CELS_FLAG_FRESH_MOUNT) {
            CelsSessionRegisterLifecycle(s, ptr, NULL, onDestroy);
        } else {
            CelsSessionUpdateLifecycle(s, ptr, onDestroy);
        }
    }
    return ptr;
}

