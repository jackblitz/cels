#include "cels/session.h"

#include <assert.h>
#include <string.h>

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

    s->root = config ? config->root : NULL;
    s->hasComposedOnce = false;

    s->groupsGapStart = 0;
    s->groupsGapEnd = CELS_MAX_GROUPS;

    s->dataGapStart = 0;
    s->dataGapEnd = CELS_DATA_ARENA_SIZE;
    s->nextGroupId = 1;

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

    if (CelsGetLogicalGroupCount(s) > 0) {
        CelsPruneSubtree(s, 0);
    }

    s_currentSession = (prev == s) ? NULL : prev;
    memset(s, 0, sizeof(*s));
}

CelsResult
CelsSessionRecompose(CelsSession *s)
{
    assert(s != NULL);
    if (s->root == NULL) {
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

        s->root(s);

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
    const uint32_t depth = s->currentDepth++;
    s->activeStack[depth] = 1;

    const uint32_t totalGroups = CelsGetLogicalGroupCount(s);

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
    assert(s->currentDepth < CELS_MAX_DEPTH && "Exceeded CELS_MAX_DEPTH");

    const uint32_t depth = s->currentDepth++;
    s->slotOffsetStack[depth - 1] = s->currentSlotOffset;

    const uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    const uint32_t cursor = s->logicalCursor;
    const uint32_t parentIdx = s->groupIndexStack[depth - 1];
    const uint32_t parentEnd = parentIdx + 1 + CelsGetGroup(s, parentIdx)->groupSize;
    uint32_t matchIdx = UINT32_MAX;

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
        CelsSlotGroup moved[CELS_MAX_GROUPS];
        MoveGroupGap(s, totalGroups);
        memcpy(moved, &s->groups[matchIdx], movedCount * sizeof(moved[0]));
        memmove(&s->groups[cursor + movedCount],
                &s->groups[cursor],
                (matchIdx - cursor) * sizeof(moved[0]));
        memcpy(&s->groups[cursor], moved, movedCount * sizeof(moved[0]));

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

    assert(totalGroups < CELS_MAX_GROUPS && "CELS_ERROR_GROUP_OVERFLOW");
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
        assert(s->slotCount < CELS_MAX_SLOTS && "CELS_ERROR_SLOT_ARENA_OVERFLOW");
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

        assert(offset + alignedSize <= CELS_DATA_ARENA_SIZE
               && "CELS_ERROR_SLOT_ARENA_OVERFLOW");

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
            assert(s->cleanupCount < CELS_MAX_CLEANUPS
                   && "CELS_ERROR_CLEANUP_OVERFLOW");
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
CelsFindLifecycleState(CelsSession *s, uint64_t key)
{
    if (s == NULL) {
        return NULL;
    }

    for (uint32_t i = 0; i < s->cleanupCount; ++i) {
        if (s->cleanups[i].groupKey == key) {
            return s->cleanups[i].instance;
        }
    }
    return NULL;
}

void *
CelsFindObserver(CelsSession *s, uint64_t key)
{
    return CelsFindLifecycleState(s, key);
}
