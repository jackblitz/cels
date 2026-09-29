#pragma once

/**
 * @file state.h
 * @brief Keyed double-buffered reactive state registry and mutation tracking.
 *
 * CELS state is owned, allocated, and tracked by the CEL_Session in cache-aligned slabs.
 * State is double-buffered:
 * - Reads (cel_watch, cel_get_state) read from the published front buffer (lock-free, zero torn reads).
 * - Mutations (cel_mutate) write to the staging back buffer and flag the state dirty.
 * - Dirty states are published atomically at frame boundaries in CelSessionRecompose.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cels/slot_table.h"

#ifndef CELS_MAX_STATES
#define CELS_MAX_STATES 2048u
#endif

#ifndef CELS_MAX_WATCHERS
#define CELS_MAX_WATCHERS 32u
#endif

#ifndef CELS_MAX_QUEUE
#define CELS_MAX_QUEUE 2048u
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of owning session */
#ifndef CELS_SESSION_TYPEDEF_DEFINED
#define CELS_SESSION_TYPEDEF_DEFINED
typedef struct CelsSession CelsSession;
typedef struct CelsSession CEL_Session;
#endif

/**
 * Double-buffered reactive state cell binding a unique CEL_Id to front and back memory buffers.
 */
typedef struct CelsStateCell {
    CEL_Id   id;
    void    *frontBuffer;   /**< Published snapshot read by cel_watch / cel_get_state */
    void    *backBuffer;    /**< Staging buffer written by cel_mutate */
    size_t   size;          /**< Size in bytes of the state struct */
    bool     isDirty;       /**< True if backBuffer has pending mutations to publish */
    bool     inUse;         /**< True if cell slot is occupied */
    uint64_t watcherKeys[CELS_MAX_WATCHERS]; /**< Subscribed composable/composition group keys */
    uint16_t watcherCount;
} CelsStateCell;

/**
 * Registry of active reactive state cells for a session.
 */
typedef struct CelsStateRegistry {
    CelsStateCell cells[CELS_MAX_STATES];
    uint32_t      cellCount;
} CelsStateRegistry;

/**
 * Initializes a reactive state registry.
 *
 * @param registry Target registry. Non-NULL.
 */
void CelsStateRegistryInit(CelsStateRegistry *registry);

/**
 * Finds an existing reactive state cell by its unique CEL_Id.
 *
 * @param registry Target registry. Non-NULL.
 * @param id       Unique 64-bit state identifier.
 * @return Pointer to state cell, or NULL if not found.
 */
CelsStateCell *CelsStateRegistryFindCell(CelsStateRegistry *registry, CEL_Id id);

/**
 * Resolves or creates a double-buffered reactive state cell by CEL_Id.
 *
 * @param session    Owning session. Non-NULL.
 * @param id         Unique 64-bit state identifier.
 * @param size       Size in bytes of state struct.
 * @param defaultVal Pointer to default initial values. May be NULL.
 * @return Pointer to resolved state cell, or NULL if capacity exceeded.
 */
CelsStateCell *CelsStateGetOrCreateCell(CelsSession *session,
                                        CEL_Id id,
                                        size_t size,
                                        const void *defaultVal);

/**
 * Subscribes the currently active composable to state id and returns a typed read-only pointer.
 *
 * @param session Active or target session.
 * @param id      Unique 64-bit state identifier.
 * @param size    Expected size of state struct for schema verification.
 * @return Read-only pointer into the published front buffer, or NULL if not found.
 */
const void *CelsStateWatch(CelsSession *session, CEL_Id id, size_t size);

/**
 * Passively reads the published snapshot of state id without subscribing.
 *
 * @param session Target session.
 * @param id      Unique 64-bit state identifier.
 * @param size    Expected size of state struct.
 * @return Read-only pointer into the published front buffer, or NULL if not found.
 */
const void *CelsStateGet(CelsSession *session, CEL_Id id, size_t size);

/**
 * Obtains a mutable pointer into the staging back buffer and schedules invalidations.
 *
 * Must NOT be called inside an active composable or composition body during evaluation.
 *
 * @param session Target session.
 * @param id      Unique 64-bit state identifier.
 * @param size    Expected size of state struct.
 * @return Mutable pointer into the staging back buffer, or NULL if not found.
 */
void *CelsStateMutate(CelsSession *session, CEL_Id id, size_t size);

/**
 * Publishes all dirty state cells (synchronizes backBuffer -> frontBuffer and clears dirty flag).
 *
 * Invoked by CelSessionRecompose at frame boundaries.
 *
 * @param session Target session. Non-NULL.
 */
void CelsStatePublishDirty(CelsSession *session);

/**
 * Unsubscribes a groupKey from all reactive state cells in registry.
 *
 * @param registry Target registry. Non-NULL.
 * @param groupKey Group key to unsubscribe.
 */
void CelsStateRegistryUnsubscribeKey(CelsStateRegistry *registry, uint64_t groupKey);

#ifdef __cplusplus
}
#endif
