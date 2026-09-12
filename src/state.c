#include "cels/state.h"

#include <assert.h>
#include <string.h>

#include "cels/session.h"

#define CELS_ASSERT(cond) assert(cond)

void
CelsStateRegistryInit(CelsStateRegistry *registry)
{
    CELS_ASSERT(registry != NULL);
    memset(registry, 0, sizeof(*registry));
}

static CelsStateHeader *
GetOrCreateStateHeader(CelsSession *session, const void *statePtr)
{
    CELS_ASSERT(session != NULL);
    CELS_ASSERT(statePtr != NULL);

    for (uint32_t i = 0; i < session->stateCount; ++i) {
        if (session->states[i].ptr == statePtr) {
            return &session->states[i].header;
        }
    }

    CELS_ASSERT(session->stateCount < CELS_MAX_STATES);
    const uint32_t idx = session->stateCount++;
    session->states[idx].ptr = statePtr;
    session->states[idx].header.watcherCount = 0;
    return &session->states[idx].header;
}

void
CelsStateRead(CelsSession *session, const void *statePtr)
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

void
CelsStateCommitMutation(CelsSession *session,
                        const void *statePtr,
                        const void *oldVal,
                        size_t size)
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
    for (uint32_t i = 0; i < session->stateCount; ++i) {
        if (session->states[i].ptr == statePtr) {
            header = &session->states[i].header;
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
        }
    }
}

void
CelsStateRegistryUnsubscribeKey(CelsStateRegistry *registry, uint64_t groupKey)
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

void
CelsStateRegistryReleaseRange(CelsStateRegistry *registry,
                              uintptr_t first,
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
