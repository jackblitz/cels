#pragma once

/**
 * @file state.h
 * @brief Reactive state registry and mutation tracking.
 *
 * Typical usage:
 * @code
 *     CEL_State(WindowState) {
 *         bool isOpen;
 *         int width;
 *     };
 *
 *     WindowState state = { .isOpen = true, .width = 800 };
 *     WindowState current = cel_watch(&state);
 *
 *     cel_mutate(session, &state) {
 *         this->width = 1024;
 *     }
 * @endcode
 *
 * Thread safety: CelsState is managed within an active CelsSession. State
 * reads and mutations must occur on the session's thread.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cels/slot_table.h"

#ifndef CELS_MAX_STATES
#define CELS_MAX_STATES 2048u
#endif

#ifndef CELS_MAX_WATCHERS
#define CELS_MAX_WATCHERS 16u
#endif

#ifndef CELS_MAX_QUEUE
#define CELS_MAX_QUEUE 2048u
#endif

/* ========================================================================= */
/* State Definitions                                                         */
/* ========================================================================= */

#ifndef CEL_State
#define CEL_State(TypeName) \
    typedef struct TypeName TypeName; \
    struct TypeName

#define CEL_LifecycleState(TypeName) CEL_State(TypeName)
#define CEL_Observer(TypeName)       CEL_State(TypeName)
#define CEL_Module(TypeName)         CEL_State(TypeName)
#endif

/**
 * Forward declaration of owning session.
 */
typedef struct CelsSession CelsSession;

/**
 * Watcher key tracking header for a single reactive state cell.
 */
typedef struct CelsStateHeader {
    uint64_t watcherKeys[CELS_MAX_WATCHERS];
    uint16_t watcherCount;
} CelsStateHeader;

/**
 * Reactive state cell binding an arbitrary memory address to its subscribers.
 */
typedef struct CelsStateCell {
    const void *ptr;
    CelsStateHeader header;
} CelsStateCell;

/**
 * Registry of active reactive state cells for a session.
 */
typedef struct CelsStateRegistry {
    CelsStateCell cells[CELS_MAX_STATES];
    uint32_t cellCount;
} CelsStateRegistry;

/**
 * Initializes a reactive state registry to empty.
 *
 * @param registry Target registry. Non-NULL.
 */
void CelsStateRegistryInit(CelsStateRegistry *registry);

/**
 * Records a read of statePtr by the active composition group in session.
 *
 * @param session  Target session. If NULL, current session is used.
 * @param statePtr Address of read state. Non-NULL.
 */
void CelsStateRead(CelsSession *session, const void *statePtr);

/**
 * Compares current value at statePtr with oldVal. If different, queues
 * invalidations for all subscribing composition keys in session.
 *
 * @param session  Target session. If NULL, current session is used.
 * @param statePtr Address of mutated state. Non-NULL.
 * @param oldVal   Pointer to snapshot of state before mutation. Non-NULL.
 * @param size     Byte size of mutated state.
 */
void CelsStateCommitMutation(CelsSession *session,
                             const void *statePtr,
                             const void *oldVal,
                             size_t size);

/**
 * Unsubscribes groupKey from all state cells in registry.
 *
 * @param registry Target registry. Non-NULL.
 * @param groupKey Group key to unsubscribe.
 */
void CelsStateRegistryUnsubscribeKey(CelsStateRegistry *registry,
                                     uint64_t groupKey);

/**
 * Removes any state cells whose pointer falls within [first, end).
 *
 * @param registry Target registry. Non-NULL.
 * @param first    Starting memory address (inclusive).
 * @param end      Ending memory address (exclusive).
 */
void CelsStateRegistryReleaseRange(CelsStateRegistry *registry,
                                   uintptr_t first,
                                   uintptr_t end);
