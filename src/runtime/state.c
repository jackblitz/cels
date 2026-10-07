#include "cels/runtime/state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cels/runtime/session.h"
#include "cels/runtime/log.h"

#define CELS_ASSERT(cond) assert(cond)

/**
 * Initializes a session reactive state registry to an empty state.
 *
 * Clears cell count and zero-initializes all cell array storage.
 *
 * @param registry Target state registry pointer. Non-NULL.
 */
void CelsStateRegistryInit(CelsStateRegistry *registry)
{
    CELS_ASSERT(registry != NULL);
    memset(registry, 0, sizeof(*registry));
}

/**
 * Searches the registry for an active reactive state cell matching CEL_Id.
 *
 * Performs a linear scan over registered cells, matching on inUse and unique 64-bit ID.
 *
 * @param registry Target state registry pointer. Safe if NULL.
 * @param id       Unique 64-bit state identifier.
 * @return Pointer to matching CelsStateCell, or NULL if not found.
 */
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

/**
 * Resolves an existing reactive state cell or creates and allocates a new one.
 *
 * If cell exists, returns it immediately. Otherwise, allocates front buffer
 * (preceded by CelsStateHeader) and staging back buffer from the session data slab.
 * Initializes both buffers with defaultVal (or zeroes), records metadata, and registers
 * the new cell in session->stateRegistry.
 *
 * @param session    Owning session providing slab storage. Non-NULL.
 * @param id         Unique 64-bit state identifier.
 * @param size       Size in bytes of user state structure.
 * @param defaultVal Optional initial state payload buffer. Safe if NULL.
 * @return Pointer to resolved CelsStateCell, or NULL on capacity/allocation failure.
 */
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

    /* 3. Allocate front (with preceding CelsStateHeader) and back buffers from session slab */
    const size_t totalFrontSize = sizeof(CelsStateHeader) + size;
    void *frontRaw = CelsSessionAllocData(session, totalFrontSize);
    void *back = CelsSessionAllocData(session, size);
    if (frontRaw == NULL || back == NULL) {
        cel_print_log(ERROR, "[CELS ERROR] Failed to allocate state memory for CEL_Id 0x%llX\n", (unsigned long long)id);
        return NULL;
    }

    CelsStateHeader *hdr = (CelsStateHeader*)frontRaw;
    hdr->magic = CELS_STATE_MAGIC;
    hdr->size = (uint32_t)size;
    hdr->session = session;
    hdr->id = id;
    hdr->reserved = 0;

    void *front = (void*)((uint8_t*)frontRaw + sizeof(CelsStateHeader));

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

/**
 * Subscribes the active composable group to state ID and returns the published front buffer.
 *
 * Locates the state cell by ID, resolves the calling composable group key from session
 * execution stacks, registers the key in the cell's watcher list, and returns a
 * read-only pointer to the published front buffer.
 *
 * @param session Owning or active session. Safe if NULL (resolves current session).
 * @param id      Unique 64-bit state identifier.
 * @param size    Expected size in bytes for schema verification.
 * @return Read-only pointer into published front buffer, or NULL if cell not found.
 */
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

/**
 * Reads the published snapshot of a state cell without establishing a dependency subscription.
 *
 * Unlike CelsStateWatch, does not register the calling composable as a watcher,
 * avoiding recomposition triggers when the state is mutated.
 *
 * @param session Owning or active session. Safe if NULL.
 * @param id      Unique 64-bit state identifier.
 * @param size    Expected size in bytes.
 * @return Read-only pointer to published front buffer, or NULL if not found.
 */
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

/**
 * Obtains a mutable pointer into the staging back buffer and schedules invalidations.
 *
 * Flags the state cell dirty so changes will be published at the frame boundary,
 * and enqueues all subscribed watcher keys into the session's invalidationQueue.
 * Enforces the architectural rule that mutations cannot occur inside active composables.
 *
 * @param session Owning or active session. Safe if NULL.
 * @param id      Unique 64-bit state identifier.
 * @param size    Expected size in bytes.
 * @return Mutable pointer to staging back buffer, or NULL if cell not found.
 */
void *CelsStateMutate(CelsSession *session, CEL_Id id, size_t size)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL || id == 0) {
        return NULL;
    }

    // Enforce DSL rule: cel_mutate cannot be called during composable/composition evaluation,
    // but IS permitted within CEL_Task coroutines, lifecycle hooks, event handlers, and simulation loops.
    assert((!session->isRecomposing || session->isExecutingTask || session->isHandlingEvent) &&
           "cel_mutate cannot be called inside a Composable or Composition body. Perform mutations in event callbacks, input handlers, simulation loops, or CEL_Task coroutines.");

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

/**
 * Synchronizes staging back buffers to published front buffers across all dirty state cells.
 *
 * Iterates through all registered cells in the session. For every inUse and isDirty cell,
 * copies backBuffer -> frontBuffer and clears the dirty flag. Invoked at frame boundaries
 * by CelsSessionRecompose.
 *
 * @param session Target session. Safe if NULL.
 */
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

/**
 * Unsubscribes a composable group key from all reactive state cells in the registry.
 *
 * Scans all cells in the registry; if groupKey is found in a cell's watcherKeys array,
 * removes it and shifts remaining keys down. Invoked when a composable node unmounts
 * to prevent dead invalidation notifications.
 *
 * @param registry Target state registry pointer. Safe if NULL.
 * @param groupKey 64-bit composable group key to unsubscribe.
 */
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

/**
 * Subscribes the active composable group to a hoisted reactive state instance pointer.
 *
 * Resolves the preceding CelsStateHeader in O(1) to locate the cell ID, owning session,
 * and struct size, registers the calling composable group as a watcher, and returns ptr.
 *
 * @param session Owning or active session. Safe if NULL (resolves active or header session).
 * @param ptr     Pointer to reactive state struct payload. Safe if NULL.
 * @return The same state pointer for chained expressions or pass-through.
 */
const void *CelsWatchStateInstance(CelsSession *session, const void *ptr)
{
    if (ptr == NULL) {
        return NULL;
    }
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    CelsStateHeader *hdr = CelsGetStateHeader(ptr);
    if (hdr == NULL) {
        return ptr;
    }
    if (session == NULL) {
        session = hdr->session;
    }
    if (session != NULL) {
        CelsStateWatch(session, hdr->id, hdr->size);
    }
    return ptr;
}

/**
 * Obtains a mutable pointer into the staging back buffer for a reactive state instance.
 *
 * Resolves the preceding CelsStateHeader in O(1) to identify the state cell and owning
 * session, flags the state dirty, queues watchers for recomposition, and returns the
 * back-buffer pointer. Must NOT be called during active composable evaluation.
 *
 * @param ptr Pointer to reactive state struct payload. Safe if NULL.
 * @return Mutable pointer to staging back buffer, or NULL if header/session is invalid.
 */
void *CelsMutateStateInstance(void *ptr)
{
    if (ptr == NULL) {
        return NULL;
    }
    CelsStateHeader *hdr = CelsGetStateHeader(ptr);
    if (hdr == NULL) {
        return NULL;
    }
    CelsSession *session = hdr->session;
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL) {
        return NULL;
    }
    return CelsStateMutate(session, hdr->id, hdr->size);
}

/**
 * Destructor callback invoked when a slot-managed reactive state instance is unmounted.
 *
 * Reads the CEL_Id stored in the slot memory, looks up the corresponding reactive state
 * cell in the session state registry, and marks it unused, clearing all watchers and dirty flags.
 *
 * @param instance Pointer to the CEL_Id stored in slot memory. Safe if NULL.
 * @param session  Target session. Safe if NULL.
 */
static void CelsStateInstanceCleanup(void *instance, CelsSession *session)
{
    if (instance == NULL || session == NULL) {
        return;
    }
    CEL_Id id = *(CEL_Id*)instance;
    if (id == 0) {
        return;
    }
    CelsStateCell *cell = CelsStateRegistryFindCell(&session->stateRegistry, id);
    if (cell != NULL) {
        cell->inUse = false;
        cell->watcherCount = 0;
        cell->isDirty = false;
    }
}

/**
 * Allocates or resolves a persistent reactive state instance pinned to a composable slot.
 *
 * Allocates a 64-bit ID in the calling composable's slot table with an unmount cleanup hook.
 * On first mount, generates a stable session-scoped unique ID, allocates the double-buffered
 * cell in the session data slab, and copies initVal into both buffers. On subsequent passes,
 * returns the front buffer in O(1). Automatically frees the cell if the composable unmounts.
 *
 * @param session Owning or active session. Safe if NULL (resolves current session).
 * @param size    Size in bytes of the user state struct.
 * @param initVal Initial values buffer to populate on mount. Safe if NULL.
 * @return Pointer to published front-buffer user payload.
 */
void *CelsResolveStateInstance(CelsSession *session, size_t size, const void *initVal)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    assert(session != NULL && "cel_state called outside of an active CelsSession");
    assert(session->currentDepth > 0 && "cel_state must be called inside a Composable or Composition");

    /* Allocate or resolve a 64-bit persistent slot to hold this state's auto-generated unique ID */
    CEL_Id *idSlot = (CEL_Id*)CelsResolveSlotWithCleanup(session,
                                                         sizeof(CEL_Id),
                                                         NULL,
                                                         CelsStateInstanceCleanup);
    if (idSlot == NULL) {
        return NULL;
    }

    if (*idSlot == 0) {
        /* Generate stable session-scoped unique ID derived from group key and slot offset */
        const CelsSlotGroup *group = CelsGetGroup(session, session->currentGroupIndex);
        uint64_t groupKey = group ? group->key : 0x12345678ULL;
        *idSlot = CelsKeyIndex(groupKey, session->currentSlotOffset);
        if (*idSlot == 0) {
            *idSlot = 1;
        }
    }

    CelsStateCell *cell = CelsStateGetOrCreateCell(session, *idSlot, size, initVal);
    if (cell != NULL) {
        cell->inUse = true;
    }
    return cell ? cell->frontBuffer : NULL;
}

