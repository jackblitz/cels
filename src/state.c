#include "cels/state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cels/session.h"
#include "cels/log.h"

#define CELS_ASSERT(cond) assert(cond)

void CelsStateRegistryInit(CelsStateRegistry *registry)
{
    CELS_ASSERT(registry != NULL);
    memset(registry, 0, sizeof(*registry));
}

CelsStateCell *CelsStateRegistryFindCell(CelsStateRegistry *registry, CEL_Id id)
{
    if (registry == NULL || id == 0) {
        return NULL;
    }
    for (uint32_t i = 0; i < registry->cellCount; ++i) {
        if (registry->cells[i].inUse && registry->cells[i].id == id) {
            return &registry->cells[i];
        }
    }
    return NULL;
}

CelsStateCell *CelsStateGetOrCreateCell(CelsSession *session,
                                        CEL_Id id,
                                        size_t size,
                                        const void *defaultVal)
{
    CELS_ASSERT(session != NULL);
    CELS_ASSERT(id != 0);
    CELS_ASSERT(size > 0);

    /* 1. Return existing if present */
    CelsStateCell *existing = CelsStateRegistryFindCell(&session->stateRegistry, id);
    if (existing != NULL) {
        return existing;
    }

    /* 2. Check capacity */
    if (session->stateRegistry.cellCount >= CELS_MAX_STATES) {
        cel_print_log(ERROR,
                "[CELS ERROR] Out of session memory: State registry capacity exceeded (%u / %u states).\n",
                session->stateRegistry.cellCount, CELS_MAX_STATES);
        CELS_ASSERT(session->stateRegistry.cellCount < CELS_MAX_STATES);
        return NULL;
    }

    /* 3. Allocate front and back buffers from session slab */
    void *front = CelsSessionAllocData(session, size);
    void *back = CelsSessionAllocData(session, size);
    if (front == NULL || back == NULL) {
        cel_print_log(ERROR, "[CELS ERROR] Failed to allocate state memory for CEL_Id 0x%llX\n", (unsigned long long)id);
        return NULL;
    }

    if (defaultVal != NULL) {
        memcpy(front, defaultVal, size);
        memcpy(back, defaultVal, size);
    } else {
        memset(front, 0, size);
        memset(back, 0, size);
    }

    const uint32_t idx = session->stateRegistry.cellCount++;
    CelsStateCell *cell = &session->stateRegistry.cells[idx];
    cell->id = id;
    cell->frontBuffer = front;
    cell->backBuffer = back;
    cell->size = size;
    cell->isDirty = false;
    cell->inUse = true;
    cell->watcherCount = 0;

    return cell;
}

const void *CelsStateWatch(CelsSession *session, CEL_Id id, size_t size)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL || id == 0) {
        return NULL;
    }

    CelsStateCell *cell = CelsStateRegistryFindCell(&session->stateRegistry, id);
    if (cell == NULL) {
        return NULL;
    }

    /* Register current executing group as a watcher */
    uint64_t currentKey = 0;
    if (session->currentDepth > 0 && session->activeStack[session->currentDepth - 1]) {
        const uint32_t activeGroupIdx = session->groupIndexStack[session->currentDepth - 1];
        currentKey = CelsGetGroup(session, activeGroupIdx)->key;
    } else if (session->currentDepth == 0 && CelsGetLogicalGroupCount(session) > 0) {
        currentKey = CelsGetGroup(session, 0)->key;
    }

    if (currentKey != 0) {
        bool alreadyWatching = false;
        for (uint16_t i = 0; i < cell->watcherCount; ++i) {
            if (cell->watcherKeys[i] == currentKey) {
                alreadyWatching = true;
                break;
            }
        }
        if (!alreadyWatching && cell->watcherCount < CELS_MAX_WATCHERS) {
            cell->watcherKeys[cell->watcherCount++] = currentKey;
        }
    }

    (void)size;
    return cell->frontBuffer;
}

const void *CelsStateGet(CelsSession *session, CEL_Id id, size_t size)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL || id == 0) {
        return NULL;
    }

    CelsStateCell *cell = CelsStateRegistryFindCell(&session->stateRegistry, id);
    (void)size;
    return cell ? cell->frontBuffer : NULL;
}

void *CelsStateMutate(CelsSession *session, CEL_Id id, size_t size)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL || id == 0) {
        return NULL;
    }

    /* Enforce DSL rule: cel_mutate cannot be called during composable evaluation */
    assert(!session->isRecomposing && session->currentDepth == 0 &&
           "cel_mutate cannot be called inside a Composable or Composition body. Perform mutations in event callbacks, input handlers, simulation loops, or lifecycle hooks.");

    CelsStateCell *cell = CelsStateRegistryFindCell(&session->stateRegistry, id);
    if (cell == NULL) {
        return NULL;
    }

    /* Flag state dirty for next publish pass */
    cell->isDirty = true;

    /* Enqueue all watchers for recomposition */
    for (uint16_t i = 0; i < cell->watcherCount; ++i) {
        const uint64_t targetKey = cell->watcherKeys[i];
        bool alreadyQueued = false;
        for (uint32_t q = 0; q < session->queueCount; ++q) {
            if (session->invalidationQueue[q] == targetKey) {
                alreadyQueued = true;
                break;
            }
        }
        if (!alreadyQueued && session->queueCount < CELS_MAX_QUEUE) {
            session->invalidationQueue[session->queueCount++] = targetKey;
        }
    }

    (void)size;
    return cell->backBuffer;
}

void CelsStatePublishDirty(CelsSession *session)
{
    if (session == NULL) {
        return;
    }

    for (uint32_t i = 0; i < session->stateRegistry.cellCount; ++i) {
        CelsStateCell *cell = &session->stateRegistry.cells[i];
        if (cell->inUse && cell->isDirty) {
            memcpy(cell->frontBuffer, cell->backBuffer, cell->size);
            cell->isDirty = false;
        }
    }
}

void CelsStateRegistryUnsubscribeKey(CelsStateRegistry *registry, uint64_t groupKey)
{
    if (registry == NULL || groupKey == 0) {
        return;
    }

    for (uint32_t i = 0; i < registry->cellCount; ++i) {
        CelsStateCell *cell = &registry->cells[i];
        if (!cell->inUse) {
            continue;
        }
        for (uint16_t j = 0; j < cell->watcherCount; ) {
            if (cell->watcherKeys[j] == groupKey) {
                --cell->watcherCount;
                memmove(&cell->watcherKeys[j],
                        &cell->watcherKeys[j + 1],
                        (cell->watcherCount - j) * sizeof(cell->watcherKeys[0]));
            } else {
                ++j;
            }
        }
    }
}
