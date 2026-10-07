#include "cels/runtime/event.h"
#include "cels/runtime/session.h"
#include "cels/runtime/task.h"
#include "cels/runtime/thread.h"
#include "cels/engine.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

/**
 * Initializes a session event queue to an empty state.
 *
 * Zeroes all internal records, poll cursors, listener registrations,
 * and task fiber waiter slots. Safe to call on uninitialized memory.
 *
 * @param queue Pointer to the CelsEventQueue to initialize. Safe if NULL.
 */
void CelsEventQueueInit(CelsEventQueue *queue)
{
    if (queue == NULL) {
        return;
    }
    memset(queue, 0, sizeof(CelsEventQueue));
}

/**
 * Resets the active event record count and cursor in a queue without clearing listeners.
 *
 * Retains registered composable listeners and task fiber waiters while clearing
 * all queued event payloads.
 *
 * @param queue Pointer to the target CelsEventQueue. Safe if NULL.
 */
void CelsEventQueueClear(CelsEventQueue *queue)
{
    if (queue == NULL) {
        return;
    }
    queue->count = 0;
    queue->pollCursor = 0;
}

/**
 * Registers the active composable group as a listener for a specific event type.
 *
 * Checks if the current group is already subscribed to typeHash; if so, merges scopeMask.
 * Otherwise, appends a new listener entry to the session's event queue listener table.
 *
 * @param session   Target session. Safe if NULL.
 * @param typeHash  64-bit event type identifier.
 * @param scopeMask Bitmask of event scopes (e.g. CELS_EVENT_SCOPE_LOCAL, SIGNAL, BROADCAST).
 */
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

/**
 * Checks all active task event waiters against an incoming event emission.
 *
 * If a matching waiter is found for typeHash and scopeMask, copies payload into the
 * waiter's output buffer, marks it received and unblocked, and schedules the waiter's
 * composable group for recomposition.
 *
 * @param session  Target session. Safe if NULL.
 * @param typeHash 64-bit event type identifier.
 * @param scope    Scope mask of the emitted event.
 * @param payload  Pointer to event payload bytes. Safe if NULL.
 * @param size     Size in bytes of the event payload.
 */
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

/**
 * Emits a local tree event within the active session.
 *
 * Checks any task fibers waiting on this event type, records the event payload
 * in the session's queue under CELS_EVENT_SCOPE_LOCAL, and immediately triggers
 * invalidation on registered ancestor/local listeners in the composable tree.
 *
 * @param session   Target session. Non-NULL.
 * @param typeHash  64-bit type name hash identifying the event type.
 * @param payload   Pointer to source event struct payload. May be NULL if size is 0.
 * @param size      Size in bytes of payload struct (clamped to CELS_EVENT_MAX_PAYLOAD).
 * @return True if event was successfully queued; false if session is NULL or queue is full.
 */
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

/**
 * Sends a point-to-point signal directly into another session's event inbox.
 *
 * Enqueues the event into targetSession with CELS_EVENT_SCOPE_SIGNAL, resolves any
 * waiting task fibers in that session, and invalidates registered signal listeners
 * to trigger recomposition on targetSession's next evaluation frame.
 *
 * @param targetSession Destination session receiving the signal. Non-NULL.
 * @param typeHash      64-bit type name hash identifying the signal type.
 * @param payload       Pointer to source signal payload data.
 * @param size          Payload size in bytes (clamped to CELS_EVENT_MAX_PAYLOAD).
 * @return True if signal was queued successfully; false if targetSession is NULL or full.
 */
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

/**
 * Emits a global broadcast event to all sessions managed by the host engine.
 *
 * If session is attached to an engine, routes through CelsEngineBroadcast with
 * thread-safe mutex locking. If no engine is attached, falls back to direct
 * delivery into the current session.
 *
 * @param session  Source session. Non-NULL.
 * @param typeHash 64-bit type name hash identifying the broadcast type.
 * @param payload  Pointer to payload data buffer.
 * @param size     Payload size in bytes (clamped to CELS_EVENT_MAX_PAYLOAD).
 * @return True if broadcast succeeded; false otherwise.
 */
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

/**
 * Polls for the first unconsumed event matching typeHash and scope mask.
 *
 * Registers the active composable as an interested listener for this event type.
 * Checks queued records, skips records already consumed by this composable group,
 * records this group as a consumer, updates pollCursor, and returns the payload.
 *
 * @param session  Active session context. Safe if NULL.
 * @param typeHash 64-bit type name hash.
 * @param scope    Bitmask of accepted scopes (CELS_EVENT_SCOPE_LOCAL, SIGNAL, BROADCAST).
 * @return Read-only pointer to event payload if found; NULL if no matching unconsumed event.
 */
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

/**
 * Fetches the next matching event after pollCursor in the queue.
 *
 * Used to iterate through multiple events of the same type within a single frame
 * (e.g. processing multiple queued keyboard keystrokes or packet arrivals).
 *
 * @param session  Active session context. Safe if NULL.
 * @param typeHash 64-bit type name hash.
 * @param scope    Bitmask of accepted scopes.
 * @return Read-only pointer to event payload, or NULL when no more matching events remain.
 */
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

/**
 * Compacts the session event queue by retiring records consumed during the frame.
 *
 * Retains records that have not been read by any listener (readCount == 0),
 * discarding consumed events and resetting the queue count and poll cursor.
 *
 * @param session Target session. Safe if NULL.
 */
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

/**
 * Registers an asynchronous event waiter for a task fiber.
 *
 * Checks if a matching event already exists in the queue: if so, immediately copies
 * the payload into outBuffer, marks the event read, and returns. If not present,
 * creates or updates a CelsTaskEventWaiter record with an optional monotonic timeout
 * deadline and invalidates the session to drive polling.
 *
 * @param session    Owning session. Non-NULL.
 * @param taskState  Pointer to task state structure.
 * @param groupKey   64-bit group key of the waiting task.
 * @param typeHash   64-bit FNV-1a hash of the event type to wait for.
 * @param scopeMask  Bitmask of accepted scopes (LOCAL, SIGNAL, BROADCAST).
 * @param outBuffer  Optional buffer receiving the event payload upon arrival.
 * @param outSize    Size in bytes of outBuffer.
 * @param timeoutMs  Timeout in milliseconds (0 for infinite wait).
 */
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

/**
 * Checks whether an event waiter associated with groupKey is still waiting.
 *
 * If the deadline has expired without receiving an event, marks the waiter as timedOut
 * and returns false to allow the task fiber to resume. If still waiting, queues
 * session invalidation on the group key and returns true.
 *
 * @param session   Owning session. Safe if NULL.
 * @param taskState Pointer to task state structure.
 * @param groupKey  64-bit group key of the task.
 * @return True if task is actively waiting; false if ready to resume (event arrived or timeout).
 */
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

/**
 * Checks whether a task fiber waiter successfully received its awaited event.
 *
 * @param session  Owning session. Safe if NULL.
 * @param groupKey 64-bit group key of the task.
 * @return True if event was received without timing out; false otherwise.
 */
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

/**
 * Suspends the calling task fiber until a matching event arrives or a timeout expires.
 *
 * Sets up the event waiter via CelsTaskWaitForEvent, switches execution context
 * back to the caller fiber, and upon resumption returns true if the event was received.
 *
 * @param session    Owning session. Non-NULL.
 * @param taskState  Pointer to task state structure with valid callerFiber.
 * @param groupKey   64-bit group key of the waiting task.
 * @param typeHash   64-bit FNV-1a hash of the awaited event type.
 * @param scopeMask  Bitmask of accepted scopes.
 * @param outBuffer  Optional buffer receiving event payload.
 * @param outSize    Size in bytes of outBuffer.
 * @param timeoutMs  Timeout in milliseconds (0 for infinite).
 * @return True if event was received; false if wait timed out.
 */
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
