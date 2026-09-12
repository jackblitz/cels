#include "cels/session.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_MSC_VER)
#include <malloc.h>
#endif

#include "cels/slot_table.h"
#include "cels/state.h"

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

static void *
CelsAllocAlignedSlab(size_t size)
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

static void
CelsFreeAlignedSlab(void *ptr)
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

CelsSession *
CelsGetCurrentSession(void)
{
    return s_currentSession;
}

void
CelsSetCurrentSession(CelsSession *session)
{
    s_currentSession = session;
}

static void
MoveGroupGap(CelsSession *s, uint32_t targetLogical)
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

static void
FireCleanupsForGroup(CelsSession *s, uint32_t groupId)
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

static void
ReleaseSlotsForGroup(CelsSession *s, uint32_t groupId)
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

static void
DrainInvalidationQueue(CelsSession *s)
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

void
CelsSessionInit(CelsSession *s, const CelsSessionConfig *config)
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

    s->root = config ? config->root : NULL;
    s->hasComposedOnce = false;

    s->maxDrainIterations = (config && config->maxDrainIterations > 0)
        ? config->maxDrainIterations
        : CELS_MAX_DRAIN_ITERATIONS;

    CelsStateRegistryInit(&s->stateRegistry);
}

void
CelsSessionSetRoot(CelsSession *s, CelsRootFn rootFn)
{
    assert(s != NULL);
    s->root = rootFn;
}

void
CelsSessionDestroy(CelsSession *s)
{
    if (s == NULL) {
        return;
    }

    CelsSession *const prev = s_currentSession;
    s_currentSession = s;

    if (s->maxGroups > 0 && CelsGetLogicalGroupCount(s) > 0) {
        CelsPruneSubtree(s, 0);
    }

    s_currentSession = (prev == s) ? NULL : prev;

    if (s->ownsSlab && s->slab != NULL) {
        CelsFreeAlignedSlab(s->slab);
    }

    memset(s, 0, sizeof(*s));
}

void
CelsSessionAttachComposition(CelsSession *s,
                             uint64_t key,
                             void (*body)(CelsSession *s, uint64_t key),
                             bool (*eval)(void *userData),
                             void *statePtr)
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
        assert(s->attachedCount < CELS_MAX_ATTACHED_COMPOSITIONS && "Exceeded CELS_MAX_ATTACHED_COMPOSITIONS");
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

CelsResult
CelsSessionRecompose(CelsSession *s)
{
    assert(s != NULL);
    if (s->root == NULL && s->attachedCount == 0) {
        return CELS_ERROR_INVALID_STATE;
    }

    if (s->hasComposedOnce && s->queueCount == 0) {
        return CELS_OK;
    }

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

void
CelsPruneSubtree(CelsSession *s, uint32_t rootLogicalIndex)
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

void
CelsPruneSubtreeByKey(CelsSession *s, uint64_t key)
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

bool
CelsEnterComposition(CelsSession *s, uint64_t rootKey)
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

bool
CelsEnterComposable(CelsSession *s, uint64_t key)
{
    assert(s->currentDepth > 0 && "CEL_Composable must be nested within CEL_Composition");
    if (s->currentDepth >= CELS_MAX_DEPTH) {
        fprintf(stderr,
                "[CELS ERROR] Out of session memory: Exceeded maximum composition nesting depth (%u / %u).\n",
                s->currentDepth, CELS_MAX_DEPTH);
        assert(s->currentDepth < CELS_MAX_DEPTH && "Exceeded CELS_MAX_DEPTH");
        return false;
    }

    const uint32_t depth = s->currentDepth++;
    s->slotOffsetStack[depth - 1] = s->currentSlotOffset;

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
        fprintf(stderr,
                "[CELS ERROR] Out of session memory: Cannot allocate composable group (key: 0x%016llX).\n"
                "             Active groups: %u / %u (slab budget: %zu B).\n"
                "             Consider increasing slabSize (e.g. CELS_SLAB_48K or CELS_SLAB_64K).\n",
                (unsigned long long)key, totalGroups, s->maxGroups, s->slabSize);
        assert(totalGroups < s->maxGroups && "CELS_ERROR_GROUP_OVERFLOW");
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

void
CelsExitGroup(CelsSession *s)
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

void *
CelsResolveSlot(CelsSession *s,
                size_t size,
                const void *initVal,
                const CelsLifecycleDesc *desc)
{
    assert(s->currentDepth > 0);
    CelsSlotGroup *const group = CelsGetGroup(s, s->currentGroupIndex);
    const size_t alignedSize = CELS_ALIGN_UP(size);
    assert(alignedSize > 0 && alignedSize <= UINT16_MAX);

    if (group->flags & CELS_FLAG_FRESH_MOUNT) {
        if (s->slotCount >= s->maxSlots) {
            fprintf(stderr,
                    "[CELS ERROR] Out of session memory: Slot allocation limit reached (%u / %u slots in %zu-byte slab).\n"
                    "             Consider increasing slabSize (e.g. CELS_SLAB_48K or CELS_SLAB_64K).\n",
                    s->slotCount, s->maxSlots, s->slabSize);
            assert(s->slotCount < s->maxSlots && "CELS_ERROR_SLOT_ARENA_OVERFLOW");
            return NULL;
        }
        uint32_t offset = 0;
        uint32_t insertion = 0;

        while (insertion < s->slotCount) {
            CelsSlotAllocation *const next = &s->slots[insertion];
            if (offset + alignedSize <= next->arenaOffset) {
                break;
            }
            offset = next->arenaOffset + next->size;
            ++insertion;
        }

        if (offset + alignedSize > s->dataArenaSize) {
            fprintf(stderr,
                    "[CELS ERROR] Out of session memory: Slot data arena overflow (requested: %zu B, used: %u B, arena capacity: %zu B in %zu-byte slab).\n"
                    "             Consider increasing slabSize (e.g. CELS_SLAB_48K or CELS_SLAB_64K).\n",
                    alignedSize, s->dataGapStart, s->dataArenaSize, s->slabSize);
            assert(offset + alignedSize <= s->dataArenaSize
                   && "CELS_ERROR_SLOT_ARENA_OVERFLOW");
            return NULL;
        }

        memmove(&s->slots[insertion + 1],
                &s->slots[insertion],
                (s->slotCount - insertion) * sizeof(s->slots[0]));

        s->slots[insertion] = (CelsSlotAllocation){
            .groupId = (uint32_t)group->userData,
            .slotOffset = s->currentSlotOffset,
            .arenaOffset = offset,
            .size = (uint32_t)alignedSize
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
                assert(s->cleanupCount < CELS_MAX_CLEANUPS
                       && "CELS_ERROR_CLEANUP_OVERFLOW");
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

    for (uint32_t i = 0; i < s->slotCount; ++i) {
        CelsSlotAllocation *const slot = &s->slots[i];
        if (slot->groupId == (uint32_t)group->userData
            && slot->slotOffset == s->currentSlotOffset) {
            assert(slot->size == alignedSize && "Remembered slot type/order changed");
            s->currentSlotOffset += (uint32_t)alignedSize;
            return &s->dataArena[slot->arenaOffset];
        }
    }

    assert(false && "Remembered slot count/order changed");
    return NULL;
}

void *
CelsGetState(CelsSession *s, uint64_t key)
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

void *
CelsFindLifecycleState(CelsSession *s, uint64_t key)
{
    return CelsGetState(s, key);
}

void *
CelsFindObserver(CelsSession *s, uint64_t key)
{
    return CelsGetState(s, key);
}
