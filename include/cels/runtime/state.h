#pragma once

/**
 * @file state.h
 * @brief Keyed double-buffered reactive state registry and mutation tracking.
 *
 * CELS state is owned, allocated, and tracked by the CEL_Session in cache-aligned slabs.
 * State is double-buffered:
 * - Reads (cel_watch, cel_get_state) read from the published front buffer (lock-free, zero torn reads).
 * - Mutations (cel_mutate) write to the staging back buffer and flag the state dirty.
 * - Dirty states are published atomically at frame boundaries in CelsSessionRecompose.
 *
 * Typical usage:
 * @code
 *     // In a composable: read and subscribe to state changes
 *     CEL_Composable(ScoreDisplay) {
 *         const GameScore *score = cel_watch(GameScore, SCORE_ID);
 *         printf("Current Score: %d\n", score->points);
 *     }
 *
 *     // In an event handler or task: mutate state
 *     cel_mutate(session, SCORE_ID, GameScore) {
 *         this->points += 100;
 *     }
 * @endcode
 *
 * Thread safety: Front-buffer reads (`CelsStateWatch`, `CelsStateGet`) are lock-free
 * and safe to read during recomposition. Mutations (`CelsStateMutate`) write to the
 * back-buffer and publish atomically at the frame boundary.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cels/runtime/slot_table.h"

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
#define CELS_STATE_MAGIC 0x43454C53u /**< 'CELS' validation tag for hoisted state headers */

/**
 * Header preceding the front-buffer of an aligned reactive state instance.
 * Allows O(1) identity, session, and back-buffer resolution from a raw state pointer.
 */
typedef struct CelsStateHeader {
    uint32_t magic;                 /**< Validation tag (CELS_STATE_MAGIC) */
    uint32_t size;                  /**< User struct size in bytes */
    struct CelsSession *session;    /**< Owning session pointer */
    CEL_Id id;                      /**< 64-bit unique cell key */
    uint64_t reserved;              /**< Alignment padding to 32 bytes */
} CelsStateHeader;

/**
 * Resolves the state header preceding a state instance pointer in O(1).
 *
 * @param ptr Pointer to user state struct. Safe if NULL.
 * @return Pointer to header if valid, or NULL if not a CELS state instance.
 */
static inline CelsStateHeader *CelsGetStateHeader(const void *ptr)
{
    if (ptr == NULL) return NULL;
    CelsStateHeader *hdr = (CelsStateHeader*)((const uint8_t*)ptr - sizeof(CelsStateHeader));
    if (hdr->magic == CELS_STATE_MAGIC) {
        return hdr;
    }
    return NULL;
}

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

/**
 * Subscribes the active composable group to a reactive state instance pointer.
 *
 * Resolves the preceding CelsStateHeader in O(1) and registers the calling group
 * as a watcher on the state cell.
 *
 * @param session Owning or active session. Safe if NULL (resolves active session).
 * @param ptr     Pointer to reactive state struct. Safe if NULL.
 * @return The same ptr for assignment / pass-through, or NULL if invalid.
 */
const void *CelsWatchStateInstance(CelsSession *session, const void *ptr);

/**
 * Obtains a mutable pointer into the staging back buffer for a state instance.
 *
 * Resolves the preceding CelsStateHeader in O(1) to locate the owning session and cell ID.
 * Must NOT be called inside an active composable or composition body during evaluation.
 *
 * @param ptr Pointer to user state struct. Safe if NULL.
 * @return Mutable back-buffer pointer, or NULL on error.
 */
void *CelsMutateStateInstance(void *ptr);

/**
 * Allocates or resolves a state instance pinned to the calling composable's slot.
 *
 * Stores the auto-generated unique ID in the slot table. On initial mount, initializes
 * the double buffers with initVal. On subsequent passes, returns the persistent instance.
 * Automatically cleans up the state cell when the composable unmounts from the tree.
 *
 * @param session Owning or active session. Safe if NULL.
 * @param size    Size in bytes of user struct.
 * @param initVal Initial values buffer. Safe if NULL.
 * @return Pointer to front-buffer user payload.
 */
void *CelsResolveStateInstance(CelsSession *session, size_t size, const void *initVal);

#ifdef __cplusplus
}
#endif
