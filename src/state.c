#include "cels/state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cels/session.h"

#define CELS_ASSERT(cond) assert(cond)

/**
 * Initializes a reactive state registry to an empty state.
 *
 * Clears all registered state cells and resets the active count to zero.
 * Safe to call on uninitialized registry memory.
 *
 * @param registry Target registry. Non-NULL.
 */
void CelsStateRegistryInit(CelsStateRegistry *registry)
{
    CELS_ASSERT(registry != NULL);
    memset(registry, 0, sizeof(*registry));
}

/**
 * Resolves or allocates a tracking header for a reactive state cell.
 *
 * Searches the registry for an existing cell matching statePtr. If not found,
 * appends a new cell to the registry and initializes its watcher count to zero.
 * Fails with an assert and error diagnostic if the registry capacity is exceeded.
 *
 * @param session  Active session owning the registry. Non-NULL.
 * @param statePtr Memory address of the state object. Non-NULL.
 * @return Pointer to the state cell's watcher header, or NULL if capacity exceeded.
 */
static CelsStateHeader *GetOrCreateStateHeader(CelsSession *session,
                                               const void *statePtr)
{
    CELS_ASSERT(session != NULL);
    CELS_ASSERT(statePtr != NULL);

    for (uint32_t i = 0; i < session->stateRegistry.cellCount; ++i) {
        if (session->stateRegistry.cells[i].ptr == statePtr) {
            return &session->stateRegistry.cells[i].header;
        }
    }

    if (session->stateRegistry.cellCount >= CELS_MAX_STATES) {
        fprintf(stderr,
                "[CELS ERROR] Out of session memory: Reactive state registry capacity exceeded (%u / %u states).\n",
                session->stateRegistry.cellCount, CELS_MAX_STATES);
        CELS_ASSERT(session->stateRegistry.cellCount < CELS_MAX_STATES);
        return NULL;
    }
    const uint32_t idx = session->stateRegistry.cellCount++;
    session->stateRegistry.cells[idx].ptr = statePtr;
    session->stateRegistry.cells[idx].header.watcherCount = 0;
    return &session->stateRegistry.cells[idx].header;
}

/**
 * Records a read dependency on a state address for the currently active composable.
 *
 * When called from within an active composable body, extracts the current group's
 * key and adds it to the list of watchers for statePtr. Duplicate registrations
 * within the same component pass are deduplicated.
 *
 * @param session  Target session, or NULL to use the ambient current session.
 * @param statePtr Memory address of the reactive state being read. Non-NULL.
 */
void CelsStateRead(CelsSession *session, const void *statePtr)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL || statePtr == NULL) {
        return;
    }

    if (session->currentDepth > 0 && session->activeStack[session->currentDepth - 1]) {
        const uint32_t activeGroupIdx =
            session->groupIndexStack[session->currentDepth - 1];
        const uint64_t currentKey = CelsGetGroup(session, activeGroupIdx)->key;

        CelsStateHeader *const header = GetOrCreateStateHeader(session, statePtr);

        for (uint16_t i = 0; i < header->watcherCount; ++i) {
            if (header->watcherKeys[i] == currentKey) {
                return;
            }
        }

        if (header->watcherCount < CELS_MAX_WATCHERS) {
            header->watcherKeys[header->watcherCount++] = currentKey;
        }
    } else if (session->currentDepth == 0
               && CelsGetLogicalGroupCount(session) > 0) {
        const uint64_t currentKey = CelsGetGroup(session, 0)->key;
        CelsStateHeader *const header = GetOrCreateStateHeader(session, statePtr);

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

/**
 * Compares mutated state against a pre-mutation snapshot and queues invalidations.
 *
 * Performs a byte-by-byte comparison using memcmp. If the state data changed,
 * locates the corresponding state cell in the registry and enqueues all registered
 * watcher group keys into the session's invalidation queue for the next recomposition.
 *
 * @param session  Target session, or NULL to use the ambient current session.
 * @param statePtr Address of the mutated state. Non-NULL.
 * @param oldVal   Pointer to snapshot buffer captured before mutation. Non-NULL.
 * @param size     Byte size of the state struct.
 */
void CelsStateCommitMutation(CelsSession *session, const void *statePtr,
                             const void *oldVal, size_t size)
{
    if (session == NULL) {
        session = CelsGetCurrentSession();
    }
    if (session == NULL || statePtr == NULL || oldVal == NULL) {
        return;
    }

    if (memcmp(statePtr, oldVal, size) == 0) {
        return;
    }

    CelsStateHeader *header = NULL;
    for (uint32_t i = 0; i < session->stateRegistry.cellCount; ++i) {
        if (session->stateRegistry.cells[i].ptr == statePtr) {
            header = &session->stateRegistry.cells[i].header;
            break;
        }
    }

    if (header == NULL) {
        return;
    }

    for (uint16_t i = 0; i < header->watcherCount; ++i) {
        if (session->queueCount < CELS_MAX_QUEUE) {
            session->invalidationQueue[session->queueCount++] =
                header->watcherKeys[i];
        } else {
            fprintf(stderr,
                    "[CELS ERROR] Out of session memory: Invalidation queue overflow (%u / %u keys).\n",
                    session->queueCount, CELS_MAX_QUEUE);
            break;
        }
    }
}

/**
 * Unsubscribes a composable group key from all reactive state cells.
 *
 * Called when a composable group is pruned from the tree or during cleanup.
 * Removes the key from every watcher array and compacts cells that have no
 * remaining watchers.
 *
 * @param registry Target state registry. NULL is safely ignored.
 * @param groupKey Group callsite key to unsubscribe.
 */
void CelsStateRegistryUnsubscribeKey(CelsStateRegistry *registry,
                                     uint64_t groupKey)
{
    if (registry == NULL) {
        return;
    }

    for (uint32_t i = 0; i < registry->cellCount;) {
        CelsStateHeader *const header = &registry->cells[i].header;
        for (uint16_t w = 0; w < header->watcherCount; ++w) {
            if (header->watcherKeys[w] == groupKey) {
                header->watcherKeys[w] =
                    header->watcherKeys[--header->watcherCount];
                break;
            }
        }
        if (header->watcherCount == 0) {
            registry->cells[i] = registry->cells[--registry->cellCount];
        } else {
            ++i;
        }
    }
}

/**
 * Releases all reactive state cells whose tracked pointers fall within [first, end).
 *
 * Called during arena slot compaction and group pruning to purge state cells
 * that point to decommissioned slot memory.
 *
 * @param registry Target state registry. NULL is safely ignored.
 * @param first    Inclusive starting memory address.
 * @param end      Exclusive ending memory address.
 */
void CelsStateRegistryReleaseRange(CelsStateRegistry *registry, uintptr_t first,
                                   uintptr_t end)
{
    if (registry == NULL) {
        return;
    }

    for (uint32_t i = 0; i < registry->cellCount;) {
        const uintptr_t ptr = (uintptr_t)registry->cells[i].ptr;
        if (ptr >= first && ptr < end) {
            registry->cells[i] = registry->cells[--registry->cellCount];
        } else {
            ++i;
        }
    }
}
