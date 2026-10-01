#pragma once

/**
 * @file event.h
 * @brief Discrete Events, Targeted Signals, and Global Engine Broadcasts for CELS.
 *
 * Provides a unified, zero-allocation messaging subsystem across 3 communication tiers:
 * 1. Tree Events: cel_event(Type, ...) & cel_listen(Type, ev) { ... }
 *    - Hierarchical communication bubbling from child composables to ancestors.
 * 2. Targeted Signals: cel_signal(&session, Type, ...) & cel_connect(Type, sig) { ... }
 *    - Point-to-point communication directly addressed to another session's inbox.
 * 3. Engine Broadcasts: cel_broadcast(Type, ...) & cel_bind(Type, bcast) { ... }
 *    - Global, cross-thread publish-subscribe bus drained at engine frame ticks.
 *
 * Also provides task fiber integration for non-blocking asynchronous awaiting:
 * - cel_wait_for(Type, outPtr)
 * - cel_wait_signal(Type, outPtr)
 * - cel_wait_broadcast(Type, outPtr)
 * - cel_wait_for_timeout(Type, outPtr, timeoutMs)
 */

#include "cels/runtime/slot_table.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CELS_EVENT_MAX_PAYLOAD
#define CELS_EVENT_MAX_PAYLOAD 128u
#endif

#ifndef CELS_EVENT_QUEUE_CAPACITY
#define CELS_EVENT_QUEUE_CAPACITY 64u
#endif

#ifndef CELS_MAX_EVENT_LISTENERS
#define CELS_MAX_EVENT_LISTENERS 64u
#endif

/**
 * Communication scope flags distinguishing delivery routing.
 */
typedef enum CelsEventScope {
    CELS_EVENT_SCOPE_NONE      = 0,
    CELS_EVENT_SCOPE_LOCAL     = (1u << 0), /**< Local tree event (cel_event / cel_listen) */
    CELS_EVENT_SCOPE_SIGNAL    = (1u << 1), /**< Targeted session signal (cel_signal / cel_connect) */
    CELS_EVENT_SCOPE_BROADCAST = (1u << 2), /**< Engine-wide global broadcast (cel_broadcast / cel_bind) */
    CELS_EVENT_SCOPE_ANY       = (CELS_EVENT_SCOPE_LOCAL | CELS_EVENT_SCOPE_SIGNAL | CELS_EVENT_SCOPE_BROADCAST)
} CelsEventScope;

#ifndef CELS_MAX_EVENT_CONSUMERS
#define CELS_MAX_EVENT_CONSUMERS 8u
#endif

/**
 * Discrete event record stored in session or engine ring buffers.
 */
typedef struct CelsEventRecord {
    uint64_t typeHash;                         /**< 64-bit FNV-1a hash of C struct type name */
    uint32_t scope;                            /**< Bitmask of CelsEventScope */
    uint32_t size;                             /**< Size of payload in bytes */
    uint32_t sourceGroupId;                    /**< Source composable group ID (or 0) */
    uint32_t readCount;                        /**< Number of listeners that consumed this event */
    uint64_t consumedKeys[CELS_MAX_EVENT_CONSUMERS]; /**< Composable group keys that have read this event */
    uint32_t consumedCount;                    /**< Number of distinct group keys that consumed this event */
    uint8_t  payload[CELS_EVENT_MAX_PAYLOAD];  /**< Inlined contiguous payload */
} CelsEventRecord;

/**
 * Registration tracking an active composable group listening for a specific event type.
 */
typedef struct CelsEventListener {
    uint64_t key;       /**< 64-bit group key of the listening composable */
    uint64_t typeHash;  /**< 64-bit FNV-1a hash of the event type */
    uint32_t scopeMask; /**< Scopes accepted by this listener */
} CelsEventListener;

/**
 * Task fiber waiter tracking a task suspended via cel_wait_for / cel_wait_signal / cel_wait_broadcast.
 */
typedef struct CelsTaskEventWaiter {
    bool     isWaiting;         /**< True while task is suspended waiting for an event */
    uint64_t groupKey;          /**< 64-bit group key of the waiting task */
    uint64_t typeHash;          /**< 64-bit FNV-1a hash of the awaited type */
    uint32_t scopeMask;         /**< Scopes accepted by this waiter */
    void    *outBuffer;         /**< Target buffer to copy payload into upon arrival */
    size_t   outSize;           /**< Size of target buffer */
    uint64_t timeoutDeadlineMs; /**< Monotonic deadline timestamp (0 = infinite) */
    bool     received;          /**< True if event was received before timeout */
    bool     timedOut;          /**< True if wait expired without receiving event */
} CelsTaskEventWaiter;

/**
 * Session-level event mailbox and dispatch queue.
 */
typedef struct CelsEventQueue {
    CelsEventRecord     records[CELS_EVENT_QUEUE_CAPACITY];
    uint32_t            count;
    uint32_t            pollCursor;
    CelsEventListener   listeners[CELS_MAX_EVENT_LISTENERS];
    uint32_t            listenerCount;
    CelsTaskEventWaiter waiters[CELS_MAX_EVENT_LISTENERS];
    uint32_t            waiterCount;
} CelsEventQueue;

/* Forward declaration of CelsSession */
struct CelsSession;

/* ========================================================================= */
/* Session Event Queue Operations                                            */
/* ========================================================================= */

/**
 * Initializes a session event queue.
 */
void CelsEventQueueInit(CelsEventQueue *queue);

/**
 * Resets/clears all event records and poll cursors in a queue.
 */
void CelsEventQueueClear(CelsEventQueue *queue);

/**
 * Emits a local tree event into the active session.
 *
 * @param session   Active session. Non-NULL.
 * @param typeHash  64-bit type name hash.
 * @param payload   Pointer to payload struct data.
 * @param size      Payload size in bytes (clamped to CELS_EVENT_MAX_PAYLOAD).
 * @return True if staged successfully; false if queue full.
 */
bool CelsEventEmit(struct CelsSession *session, uint64_t typeHash, const void *payload, size_t size);

/**
 * Sends a targeted signal directly into another session's inbox.
 *
 * @param targetSession Target recipient session. Non-NULL.
 * @param typeHash      64-bit type name hash.
 * @param payload       Pointer to payload struct data.
 * @param size          Payload size in bytes.
 * @return True if staged successfully; false if target queue full.
 */
bool CelsEventSignal(struct CelsSession *targetSession, uint64_t typeHash, const void *payload, size_t size);

/**
 * Broadcasts an event globally across the engine and all registered sessions.
 *
 * @param session   Calling session (or NULL if called from external thread).
 * @param typeHash  64-bit type name hash.
 * @param payload   Pointer to payload struct data.
 * @param size      Payload size in bytes.
 * @return True if staged successfully.
 */
bool CelsEventBroadcast(struct CelsSession *session, uint64_t typeHash, const void *payload, size_t size);

/**
 * Begins polling events of a specific type and scope inside a composable block.
 *
 * Registers the current composable group as a listener for future occurrences,
 * and returns a pointer to the first unconsumed event payload, or NULL.
 *
 * @param session   Active session. Non-NULL.
 * @param typeHash  64-bit type name hash.
 * @param scope     Allowed CelsEventScope flags.
 * @return Pointer to payload struct, or NULL if no matching events.
 */
const void *CelsEventPoll(struct CelsSession *session, uint64_t typeHash, uint32_t scope);

/**
 * Advances to the next unconsumed event matching type and scope.
 *
 * @param session   Active session. Non-NULL.
 * @param typeHash  64-bit type name hash.
 * @param scope     Allowed CelsEventScope flags.
 * @return Pointer to next payload struct, or NULL when complete.
 */
const void *CelsEventNext(struct CelsSession *session, uint64_t typeHash, uint32_t scope);

/**
 * Retires consumed events and resets the queue at the end of recomposition.
 *
 * @param session Active session. Non-NULL.
 */
void CelsEventRetireConsumed(struct CelsSession *session);

/* ========================================================================= */
/* Task Fiber Asynchronous Waiting                                           */
/* ========================================================================= */

/**
 * Suspends a task fiber until an event/signal/broadcast of matching type arrives.
 *
 * @param session   Active session. Non-NULL.
 * @param taskState Pointer to active task state. Non-NULL.
 * @param groupKey  Unique 64-bit key of the task group.
 * @param typeHash  64-bit type name hash.
 * @param scopeMask Scope filter flags.
 * @param outBuffer Target buffer to write received payload into.
 * @param outSize   Size of target buffer.
 * @param timeoutMs Timeout in milliseconds (0 for indefinite).
 */
void CelsTaskWaitForEvent(struct CelsSession *session,
                          void *taskState,
                          uint64_t groupKey,
                          uint64_t typeHash,
                          uint32_t scopeMask,
                          void *outBuffer,
                          size_t outSize,
                          uint32_t timeoutMs);

/**
 * Checks whether a waiting task fiber should resume.
 *
 * @param session   Active session. Non-NULL.
 * @param taskState Pointer to task state. Non-NULL.
 * @param groupKey  Unique 64-bit key of the task group.
 * @return True if task is still waiting; false if ready to resume.
 */
bool CelsTaskEventShouldWait(struct CelsSession *session, void *taskState, uint64_t groupKey);

/**
 * Checks whether an event was received by a task waiter (as opposed to timing out).
 *
 * @param session  Active session. Non-NULL.
 * @param groupKey Unique 64-bit key of the task group.
 * @return True if event was received; false if timed out or not waiting.
 */
bool CelsTaskEventWasReceived(struct CelsSession *session, uint64_t groupKey);

/**
 * Suspends task fiber for an event with a timeout and returns whether it was received.
 *
 * @param session   Active session. Non-NULL.
 * @param taskState Pointer to active task state. Non-NULL.
 * @param groupKey  Unique 64-bit key of the task group.
 * @param typeHash  64-bit type name hash.
 * @param scopeMask Scope filter flags.
 * @param outBuffer Target buffer to write received payload into.
 * @param outSize   Size of target buffer.
 * @param timeoutMs Timeout in milliseconds.
 * @return True if event was received before timeout; false if timed out.
 */
bool CelsTaskWaitForTimeout(struct CelsSession *session,
                            void *taskState,
                            uint64_t groupKey,
                            uint64_t typeHash,
                            uint32_t scopeMask,
                            void *outBuffer,
                            size_t outSize,
                            uint32_t timeoutMs);

#ifdef __cplusplus
}
#endif
