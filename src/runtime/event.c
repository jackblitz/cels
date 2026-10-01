#include "cels/runtime/event.h"
#include "cels/runtime/session.h"
#include "cels/runtime/task.h"
#include "cels/runtime/thread.h"
#include "cels/engine.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

void CelsEventQueueInit(CelsEventQueue *queue)
{
    if (queue == NULL) {
        return;
    }
    memset(queue, 0, sizeof(CelsEventQueue));
}

void CelsEventQueueClear(CelsEventQueue *queue)
{
    if (queue == NULL) {
        return;
    }
    queue->count = 0;
    queue->pollCursor = 0;
}

static void CelsRegisterEventListener(CelsSession *session, uint64_t typeHash, uint32_t scopeMask)
{
    if (session == NULL) {
        return;
    }
    CelsEventQueue *const q = &session->eventQueue;
    const uint64_t currentKey = CelsGetCurrentGroupKey(session);

    /* Deduplicate: check if already registered for this group and type */
    for (uint32_t i = 0; i < q->listenerCount; ++i) {
        if (q->listeners[i].key == currentKey && q->listeners[i].typeHash == typeHash) {
            q->listeners[i].scopeMask |= scopeMask;
            return;
        }
    }

    if (q->listenerCount < CELS_MAX_EVENT_LISTENERS) {
        q->listeners[q->listenerCount++] = (CelsEventListener){
            .key = currentKey,
            .typeHash = typeHash,
            .scopeMask = scopeMask
        };
    }
}

static void CheckTaskWaiters(CelsSession *session, uint64_t typeHash, uint32_t scope, const void *payload, size_t size)
{
    if (session == NULL) {
        return;
    }
    CelsEventQueue *const q = &session->eventQueue;
    for (uint32_t i = 0; i < q->waiterCount; ++i) {
        CelsTaskEventWaiter *const w = &q->waiters[i];
        if (w->isWaiting && w->typeHash == typeHash && (w->scopeMask & scope)) {
            if (w->outBuffer != NULL && size > 0) {
                const size_t copySize = (size < w->outSize) ? size : w->outSize;
                memcpy(w->outBuffer, payload, copySize);
            }
            w->received = true;
            w->isWaiting = false;
            CelsSessionInvalidateKey(session, w->groupKey);
        }
    }
}

bool CelsEventEmit(CelsSession *session, uint64_t typeHash, const void *payload, size_t size)
{
    if (session == NULL) {
        return false;
    }
    CelsEventQueue *const q = &session->eventQueue;

    /* Check any task fiber waiting on this event */
    CheckTaskWaiters(session, typeHash, CELS_EVENT_SCOPE_LOCAL, payload, size);

    if (q->count >= CELS_EVENT_QUEUE_CAPACITY) {
        return false;
    }

    CelsEventRecord *const rec = &q->records[q->count++];
    rec->typeHash = typeHash;
    rec->scope = CELS_EVENT_SCOPE_LOCAL;
    rec->size = (uint32_t)((size > CELS_EVENT_MAX_PAYLOAD) ? CELS_EVENT_MAX_PAYLOAD : size);
    rec->sourceGroupId = session->currentGroupIndex;
    rec->readCount = 0;
    rec->consumedCount = 0;
    if (payload != NULL && rec->size > 0) {
        memcpy(rec->payload, payload, rec->size);
    }

    /* Invalidate any registered ancestor listeners in this session immediately */
    for (uint32_t i = 0; i < q->listenerCount; ++i) {
        if (q->listeners[i].typeHash == typeHash && (q->listeners[i].scopeMask & CELS_EVENT_SCOPE_LOCAL)) {
            CelsSessionInvalidateKeyImmediate(session, q->listeners[i].key);
        }
    }

    return true;
}

bool CelsEventSignal(CelsSession *targetSession, uint64_t typeHash, const void *payload, size_t size)
{
    if (targetSession == NULL) {
        return false;
    }
    CelsEventQueue *const q = &targetSession->eventQueue;

    /* Check any task fiber in target session waiting on this signal */
    CheckTaskWaiters(targetSession, typeHash, CELS_EVENT_SCOPE_SIGNAL, payload, size);

    if (q->count >= CELS_EVENT_QUEUE_CAPACITY) {
        return false;
    }

    CelsEventRecord *const rec = &q->records[q->count++];
    rec->typeHash = typeHash;
    rec->scope = CELS_EVENT_SCOPE_SIGNAL;
    rec->size = (uint32_t)((size > CELS_EVENT_MAX_PAYLOAD) ? CELS_EVENT_MAX_PAYLOAD : size);
    rec->sourceGroupId = 0;
    rec->readCount = 0;
    rec->consumedCount = 0;
    if (payload != NULL && rec->size > 0) {
        memcpy(rec->payload, payload, rec->size);
    }

    /* Invalidate target session listeners */
    for (uint32_t i = 0; i < q->listenerCount; ++i) {
        if (q->listeners[i].typeHash == typeHash && (q->listeners[i].scopeMask & CELS_EVENT_SCOPE_SIGNAL)) {
            CelsSessionInvalidateKey(targetSession, q->listeners[i].key);
        }
    }

    /* Ensure target session will recompose to process signal */
    if (targetSession->attachedCount > 0) {
        CelsSessionInvalidateKey(targetSession, targetSession->attachedCompositions[0].key);
    }

    return true;
}

bool CelsEventBroadcast(CelsSession *session, uint64_t typeHash, const void *payload, size_t size)
{
    if (session != NULL && session->engine != NULL) {
        return CelsEngineBroadcast(session->engine, typeHash, payload, size);
    }

    /* Fallback if no engine is bound: deliver directly to active session */
    if (session != NULL) {
        CelsEventQueue *const q = &session->eventQueue;
        CheckTaskWaiters(session, typeHash, CELS_EVENT_SCOPE_BROADCAST, payload, size);

        if (q->count >= CELS_EVENT_QUEUE_CAPACITY) {
            return false;
        }

        CelsEventRecord *const rec = &q->records[q->count++];
        rec->typeHash = typeHash;
        rec->scope = CELS_EVENT_SCOPE_BROADCAST;
        rec->size = (uint32_t)((size > CELS_EVENT_MAX_PAYLOAD) ? CELS_EVENT_MAX_PAYLOAD : size);
        rec->sourceGroupId = 0;
        rec->readCount = 0;
        if (payload != NULL && rec->size > 0) {
            memcpy(rec->payload, payload, rec->size);
        }

        for (uint32_t i = 0; i < q->listenerCount; ++i) {
            if (q->listeners[i].typeHash == typeHash && (q->listeners[i].scopeMask & CELS_EVENT_SCOPE_BROADCAST)) {
                CelsSessionInvalidateKey(session, q->listeners[i].key);
            }
        }
        return true;
    }

    return false;
}

const void *CelsEventPoll(CelsSession *session, uint64_t typeHash, uint32_t scope)
{
    if (session == NULL) {
        return NULL;
    }
    CelsRegisterEventListener(session, typeHash, scope);

    const uint64_t currentKey = CelsGetCurrentGroupKey(session);
    CelsEventQueue *const q = &session->eventQueue;
    for (uint32_t i = 0; i < q->count; ++i) {
        if (q->records[i].typeHash == typeHash && (q->records[i].scope & scope)) {
            /* Deduplicate: check if this listening group already consumed this record */
            bool alreadyConsumed = false;
            for (uint32_t c = 0; c < q->records[i].consumedCount; ++c) {
                if (q->records[i].consumedKeys[c] == currentKey) {
                    alreadyConsumed = true;
                    break;
                }
            }
            if (alreadyConsumed) {
                continue;
            }

            if (q->records[i].consumedCount < CELS_MAX_EVENT_CONSUMERS) {
                q->records[i].consumedKeys[q->records[i].consumedCount++] = currentKey;
            }
            q->pollCursor = i;
            q->records[i].readCount++;
            session->isHandlingEvent = true;
            return q->records[i].payload;
        }
    }
    session->isHandlingEvent = false;
    return NULL;
}

const void *CelsEventNext(CelsSession *session, uint64_t typeHash, uint32_t scope)
{
    if (session == NULL) {
        return NULL;
    }
    const uint64_t currentKey = CelsGetCurrentGroupKey(session);
    CelsEventQueue *const q = &session->eventQueue;
    for (uint32_t i = q->pollCursor + 1; i < q->count; ++i) {
        if (q->records[i].typeHash == typeHash && (q->records[i].scope & scope)) {
            bool alreadyConsumed = false;
            for (uint32_t c = 0; c < q->records[i].consumedCount; ++c) {
                if (q->records[i].consumedKeys[c] == currentKey) {
                    alreadyConsumed = true;
                    break;
                }
            }
            if (alreadyConsumed) {
                continue;
            }

            if (q->records[i].consumedCount < CELS_MAX_EVENT_CONSUMERS) {
                q->records[i].consumedKeys[q->records[i].consumedCount++] = currentKey;
            }
            q->pollCursor = i;
            q->records[i].readCount++;
            session->isHandlingEvent = true;
            return q->records[i].payload;
        }
    }
    session->isHandlingEvent = false;
    return NULL;
}

void CelsEventRetireConsumed(CelsSession *session)
{
    if (session == NULL) {
        return;
    }
    CelsEventQueue *const q = &session->eventQueue;

    /* Compact or clear records that have been read by at least one listener */
    uint32_t writeIdx = 0;
    for (uint32_t i = 0; i < q->count; ++i) {
        if (q->records[i].readCount == 0) {
            /* Keep unread events */
            if (writeIdx != i) {
                q->records[writeIdx] = q->records[i];
            }
            writeIdx++;
        }
    }
    q->count = writeIdx;
    q->pollCursor = 0;
}

void CelsTaskWaitForEvent(CelsSession *session,
                          void *taskState,
                          uint64_t groupKey,
                          uint64_t typeHash,
                          uint32_t scopeMask,
                          void *outBuffer,
                          size_t outSize,
                          uint32_t timeoutMs)
{
    (void)taskState;
    if (session == NULL) {
        return;
    }
    CelsEventQueue *const q = &session->eventQueue;

    /* Check if matching event is already waiting in queue */
    for (uint32_t i = 0; i < q->count; ++i) {
        if (q->records[i].typeHash == typeHash && (q->records[i].scope & scopeMask)) {
            if (outBuffer != NULL && q->records[i].size > 0) {
                const size_t copySize = (q->records[i].size < outSize) ? q->records[i].size : outSize;
                memcpy(outBuffer, q->records[i].payload, copySize);
            }
            q->records[i].readCount++;
            return;
        }
    }

    /* Register waiter */
    for (uint32_t i = 0; i < q->waiterCount; ++i) {
        if (q->waiters[i].groupKey == groupKey) {
            q->waiters[i] = (CelsTaskEventWaiter){
                .isWaiting = true,
                .groupKey = groupKey,
                .typeHash = typeHash,
                .scopeMask = scopeMask,
                .outBuffer = outBuffer,
                .outSize = outSize,
                .timeoutDeadlineMs = (timeoutMs > 0) ? (CelsGetTimeMs() + timeoutMs) : 0,
                .received = false,
                .timedOut = false
            };
            if (timeoutMs > 0) {
                CelsSessionInvalidateKey(session, groupKey);
            }
            return;
        }
    }

    if (q->waiterCount < CELS_MAX_EVENT_LISTENERS) {
        q->waiters[q->waiterCount++] = (CelsTaskEventWaiter){
            .isWaiting = true,
            .groupKey = groupKey,
            .typeHash = typeHash,
            .scopeMask = scopeMask,
            .outBuffer = outBuffer,
            .outSize = outSize,
            .timeoutDeadlineMs = (timeoutMs > 0) ? (CelsGetTimeMs() + timeoutMs) : 0,
            .received = false,
            .timedOut = false
        };
        if (timeoutMs > 0) {
            CelsSessionInvalidateKey(session, groupKey);
        }
    }
}

bool CelsTaskEventShouldWait(CelsSession *session, void *taskState, uint64_t groupKey)
{
    (void)taskState;
    if (session == NULL) {
        return false;
    }
    CelsEventQueue *const q = &session->eventQueue;
    for (uint32_t i = 0; i < q->waiterCount; ++i) {
        CelsTaskEventWaiter *const w = &q->waiters[i];
        if (w->groupKey == groupKey && w->isWaiting) {
            if (w->timeoutDeadlineMs > 0 && CelsGetTimeMs() >= w->timeoutDeadlineMs) {
                w->timedOut = true;
                w->isWaiting = false;
                return false; /* Timeout elapsed; ready to resume */
            }
            /* Still waiting for event */
            CelsSessionInvalidateKey(session, groupKey);
            return true;
        }
    }
    return false;
}

bool CelsTaskEventWasReceived(CelsSession *session, uint64_t groupKey)
{
    if (session == NULL) {
        return false;
    }
    CelsEventQueue *const q = &session->eventQueue;
    for (uint32_t i = 0; i < q->waiterCount; ++i) {
        if (q->waiters[i].groupKey == groupKey) {
            return q->waiters[i].received && !q->waiters[i].timedOut;
        }
    }
    return false;
}

bool CelsTaskWaitForTimeout(CelsSession *session,
                            void *taskState,
                            uint64_t groupKey,
                            uint64_t typeHash,
                            uint32_t scopeMask,
                            void *outBuffer,
                            size_t outSize,
                            uint32_t timeoutMs)
{
    CelsTaskWaitForEvent(session, taskState, groupKey, typeHash, scopeMask, outBuffer, outSize, timeoutMs);
    CelsTaskState *const state = (CelsTaskState*)taskState;
    if (state != NULL && state->callerFiber != NULL) {
        CelsFiberSwitch((CelsFiber*)state->callerFiber);
    }
    return CelsTaskEventWasReceived(session, groupKey);
}
