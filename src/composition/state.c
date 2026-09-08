#include "composition/state.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "composition/composer.h"
#include "composition/session.h"

#define CELS_ASSERT(condition) assert(condition)

/** Host currently composing, or NULL outside a composition walk. */
static CelsCompositionHost *s_invalidationHost = NULL;

void
CelsInvalidationContextSet(CelsCompositionHost *host)
{
    s_invalidationHost = host;
}

CelsCompositionHost *
CelsInvalidationContextGet(void)
{
    return s_invalidationHost;
}

/** Mount/prune callbacks published for the active composition walk. */
static CelsTransactionContext s_transactionContext;
static bool s_transactionContextSet = false;

void
CelsTransactionContextSet(const CelsTransactionContext *context)
{
    if (context == NULL) {
        memset(&s_transactionContext, 0, sizeof(s_transactionContext));
        s_transactionContextSet = false;
        return;
    }
    s_transactionContext = *context;
    s_transactionContextSet = true;
}

const CelsTransactionContext *
CelsTransactionContextGet(void)
{
    return s_transactionContextSet ? &s_transactionContext : NULL;
}

void
CelsTransactionNotifyCreate(CelsComposableId composable,
                            CelsComposableId parent,
                            uint32_t key)
{
    if (s_transactionContextSet && s_transactionContext.onCreate != NULL) {
        s_transactionContext.onCreate(composable, parent, key,
                                      s_transactionContext.userdata);
    }
}

void
CelsTransactionNotifyDestroy(CelsComposableId composable)
{
    if (s_transactionContextSet && s_transactionContext.onDestroy != NULL) {
        s_transactionContext.onDestroy(composable,
                                       s_transactionContext.userdata);
    }
}

/* ========================================================================= */
/* Static cell pool                                                          */
/* ========================================================================= */

/**
 * One pool entry: a cell header immediately followed by its value bytes.
 *
 * The value bytes share a union with a uint64_t so the value storage inherits
 * 8-byte alignment whatever the caller stores in it. The header comes first
 * because CelsMutableStateCreate hands out a pointer to the value, and every
 * accessor recovers the header by stepping back from it.
 */
typedef struct CelsMutableCellSlot {
    CelsMutableCell header;
    union {
        uint64_t alignment;
        uint8_t bytes[CELS_MUTABLE_VALUE_CAPACITY];
    } value;
} CelsMutableCellSlot;

/** Distance from a slot's base to its value bytes — the negative offset. */
#define CELS_MUTABLE_CELL_VALUE_OFFSET offsetof(CelsMutableCellSlot, value)

/**
 * Every cell in the process. Static, so nothing here allocates and a cell
 * outlives every session that watches it — which is exactly why every path
 * destroying a group has to unsubscribe.
 */
static CelsMutableCellSlot s_cellPool[CELS_MAX_MUTABLE_CELLS];

/**
 * Recovers a cell header from a pointer to its value.
 *
 * The pointer is checked for provenance before anything is dereferenced: it
 * must land inside the static pool, at exactly a slot's value offset. That
 * rejects the documented footgun — a stack variable or a malloc block passed
 * where a cell was expected — without ever reading memory the caller did not
 * mean to hand over. Only once the pointer is known to name a pool slot is the
 * sentinel checked, and a wrong sentinel there means the header was actually
 * overwritten, so that case asserts as well as failing.
 *
 * @param value Candidate value pointer. NULL is accepted and rejected.
 * @return The owning cell header, or NULL if value is not a live cell.
 */
static CelsMutableCell *
MutableCellResolve(const void *value)
{
    if (value == NULL) {
        return NULL;
    }

    const uintptr_t address = (uintptr_t)value;
    const uintptr_t poolBase = (uintptr_t)s_cellPool;
    if (address < poolBase || address >= poolBase + sizeof(s_cellPool)) {
        return NULL;
    }

    const uintptr_t offset = address - poolBase;
    if (offset % sizeof(CelsMutableCellSlot)
            != (uintptr_t)CELS_MUTABLE_CELL_VALUE_OFFSET) {
        return NULL;
    }

    CelsMutableCellSlot *const slot =
        &s_cellPool[offset / sizeof(CelsMutableCellSlot)];

    CELS_ASSERT(slot->header.sentinel == CELS_MUTABLE_CELL_SENTINEL);
    CELS_ASSERT(slot->header.inUse);
    if (slot->header.sentinel != CELS_MUTABLE_CELL_SENTINEL
            || !slot->header.inUse) {
        return NULL;
    }
    return &slot->header;
}

/**
 * Collapses a cell's watcher list to one entry per distinct host.
 *
 * Called once, when the list overflows. Each surviving entry keeps its host
 * and carries CELS_COMPOSABLE_ID_INVALID as its composable, which is what
 * CelsMutableStateUpdate then passes to CelsCompositionHostInvalidate to dirty
 * the whole host. Compacting in place is safe because the write index never
 * runs ahead of the read index.
 *
 * @param cell Cell whose watcher list is full. Non-NULL.
 */
static void
MutableCellWatchersCollapse(CelsMutableCell *cell)
{
    CELS_ASSERT(cell != NULL);

    uint16_t distinct = 0;
    for (uint16_t i = 0; i < cell->watcherCount; i++) {
        CelsCompositionHost *const host = cell->watchers[i].host;

        bool seen = false;
        for (uint16_t j = 0; j < distinct; j++) {
            if (cell->watchers[j].host == host) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        cell->watchers[distinct].host = host;
        cell->watchers[distinct].composable = CELS_COMPOSABLE_ID_INVALID;
        cell->watchers[distinct]._padding = 0u;
        distinct++;
    }
    cell->watcherCount = distinct;
}

/**
 * Records the composable currently being composed as a watcher of a cell.
 *
 * Does nothing outside a composition walk — no published host, or no active
 * composable — which is what lets the same read serve a network handler and a
 * composable body. Registration is idempotent on (host, composable), so
 * reading one cell twice in one body registers once.
 *
 * When the list is full and the arriving watcher is genuinely new, the cell
 * degrades: it stops tracking individuals, collapses what it holds to distinct
 * hosts, and records this host too. Coarser, still correct.
 *
 * @param cell Cell being read. Non-NULL.
 */
static void
MutableCellWatcherAdd(CelsMutableCell *cell)
{
    CELS_ASSERT(cell != NULL);

    CelsCompositionHost *const host = CelsInvalidationContextGet();
    if (host == NULL) {
        return;
    }

    const CelsComposableId composable =
        CelsComposerGetCurrentComposable(NULL);
    if (composable == CELS_COMPOSABLE_ID_INVALID) {
        return;
    }

    // An overflowed cell holds hosts, not composables, so that is what an
    // entry for this read would look like.
    const CelsComposableId recorded =
        cell->overflowed ? CELS_COMPOSABLE_ID_INVALID : composable;

    for (uint16_t i = 0; i < cell->watcherCount; i++) {
        if (cell->watchers[i].host == host
                && cell->watchers[i].composable == recorded) {
            return;
        }
    }

    if (cell->watcherCount < CELS_MAX_WATCHERS_PER_CELL) {
        CelsWatcherRef *const watcher = &cell->watchers[cell->watcherCount];
        watcher->host = host;
        watcher->composable = recorded;
        watcher->_padding = 0u;
        cell->watcherCount++;
        return;
    }

    if (!cell->overflowed) {
        cell->overflowed = true;
        MutableCellWatchersCollapse(cell);

        for (uint16_t i = 0; i < cell->watcherCount; i++) {
            if (cell->watchers[i].host == host) {
                return;
            }
        }
        if (cell->watcherCount < CELS_MAX_WATCHERS_PER_CELL) {
            CelsWatcherRef *const watcher =
                &cell->watchers[cell->watcherCount];
            watcher->host = host;
            watcher->composable = CELS_COMPOSABLE_ID_INVALID;
            watcher->_padding = 0u;
            cell->watcherCount++;
            return;
        }
    }

    // CELS_MAX_WATCHERS_PER_CELL distinct HOSTS already watch this cell, so
    // even host granularity has nowhere left to go. Nothing is recorded for
    // this host; one cell shared across that many sessions is the modelling
    // smell CELS.md §3 describes, and this is where it stops being tracked.
}

/**
 * Removes one watcher by index, swapping the last entry into its place.
 *
 * Order in the list carries no meaning — the queue decides which subtrees run,
 * the slot-table walk decides the order — so a swap-remove keeps removal O(1).
 * A cell that loses its last watcher also loses its overflowed state, so it
 * starts tracking individuals again if it is read afresh.
 *
 * @param cell  Cell to remove from. Non-NULL.
 * @param index Entry to drop, in [0, cell->watcherCount).
 */
static void
MutableCellWatcherRemoveAt(CelsMutableCell *cell, uint16_t index)
{
    CELS_ASSERT(cell != NULL);
    CELS_ASSERT(index < cell->watcherCount);

    cell->watcherCount--;
    cell->watchers[index] = cell->watchers[cell->watcherCount];
    memset(&cell->watchers[cell->watcherCount], 0, sizeof(CelsWatcherRef));

    if (cell->watcherCount == 0u) {
        cell->overflowed = false;
    }
}

/* ========================================================================= */
/* Cell lifecycle                                                            */
/* ========================================================================= */

CelsResult
CelsMutableStateCreate(const void *initialValue,
                       size_t valueSize,
                       void **outValue)
{
    CELS_ASSERT(initialValue != NULL);
    CELS_ASSERT(outValue != NULL);

    if (initialValue == NULL || outValue == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    if (valueSize == 0u || valueSize > CELS_MUTABLE_VALUE_CAPACITY) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    for (uint32_t i = 0; i < CELS_MAX_MUTABLE_CELLS; i++) {
        CelsMutableCellSlot *const slot = &s_cellPool[i];
        if (slot->header.inUse) {
            continue;
        }

        memset(slot, 0, sizeof(*slot));
        slot->header.sentinel = CELS_MUTABLE_CELL_SENTINEL;
        slot->header.valueSize = valueSize;
        slot->header.inUse = true;
        memcpy(slot->value.bytes, initialValue, valueSize);

        *outValue = slot->value.bytes;
        return CELS_OK;
    }

    return CELS_ERROR_CAPACITY_EXCEEDED;
}

CelsResult
CelsMutableStateRead(const void *value, void *outValue, size_t valueSize)
{
    CELS_ASSERT(value != NULL);
    CELS_ASSERT(outValue != NULL);

    if (value == NULL || outValue == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    CelsMutableCell *const cell = MutableCellResolve(value);
    if (cell == NULL) {
        return CELS_ERROR_INVALID_STATE;
    }
    if (valueSize != cell->valueSize) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    MutableCellWatcherAdd(cell);
    memcpy(outValue, value, valueSize);
    return CELS_OK;
}

CelsResult
CelsMutableStateUpdate(void *value, const void *newValue, size_t valueSize)
{
    CELS_ASSERT(value != NULL);
    CELS_ASSERT(newValue != NULL);

    if (value == NULL || newValue == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    CelsMutableCell *const cell = MutableCellResolve(value);
    if (cell == NULL) {
        return CELS_ERROR_INVALID_STATE;
    }
    if (valueSize != cell->valueSize) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    // Load-bearing: a write that changes nothing queues nothing. Without it
    // any periodic writer would invalidate its subscribers every tick.
    if (memcmp(value, newValue, valueSize) == 0) {
        return CELS_OK;
    }
    memcpy(value, newValue, valueSize);

    for (uint16_t i = 0; i < cell->watcherCount; i++) {
        const CelsWatcherRef *const watcher = &cell->watchers[i];
        const CelsComposableId target = cell->overflowed
            ? CELS_COMPOSABLE_ID_INVALID
            : watcher->composable;
        // Best-effort: the only failure is a NULL host, and a NULL host is
        // never recorded as a watcher in the first place.
        (void)CelsCompositionHostInvalidate(watcher->host, target);
    }

    return CELS_OK;
}

void
CelsMutableStateUnsubscribe(CelsCompositionHost *host,
                            CelsComposableId composable)
{
    if (host == NULL) {
        return;
    }

    for (uint32_t i = 0; i < CELS_MAX_MUTABLE_CELLS; i++) {
        CelsMutableCell *const cell = &s_cellPool[i].header;
        if (!cell->inUse) {
            continue;
        }

        uint16_t watcherIndex = 0;
        while (watcherIndex < cell->watcherCount) {
            if (cell->watchers[watcherIndex].host == host
                    && cell->watchers[watcherIndex].composable == composable) {
                // Re-test this index: the swap-remove moved a new entry in.
                MutableCellWatcherRemoveAt(cell, watcherIndex);
                continue;
            }
            watcherIndex++;
        }
    }
}

void
CelsMutableStateShiftComposables(CelsCompositionHost *host,
                                 CelsComposableId threshold,
                                 int32_t delta)
{
    if (host == NULL || delta == 0) {
        return;
    }

    for (uint32_t i = 0; i < CELS_MAX_MUTABLE_CELLS; i++) {
        CelsMutableCell *const cell = &s_cellPool[i].header;
        if (!cell->inUse) {
            continue;
        }

        for (uint16_t watcherIndex = 0; watcherIndex < cell->watcherCount;
             watcherIndex++) {
            CelsWatcherRef *const watcher = &cell->watchers[watcherIndex];
            if (watcher->host != host
                || watcher->composable == CELS_COMPOSABLE_ID_INVALID
                || watcher->composable < threshold) {
                continue;
            }
            watcher->composable =
                (CelsComposableId)((int64_t)watcher->composable + delta);
        }
    }
}

void
CelsMutableStateUnsubscribeHost(CelsCompositionHost *host)
{
    if (host == NULL) {
        return;
    }

    for (uint32_t i = 0; i < CELS_MAX_MUTABLE_CELLS; i++) {
        CelsMutableCell *const cell = &s_cellPool[i].header;
        if (!cell->inUse) {
            continue;
        }

        uint16_t watcherIndex = 0;
        while (watcherIndex < cell->watcherCount) {
            if (cell->watchers[watcherIndex].host == host) {
                MutableCellWatcherRemoveAt(cell, watcherIndex);
                continue;
            }
            watcherIndex++;
        }
    }
}

void
CelsMutableStateResetPool(void)
{
    memset(s_cellPool, 0, sizeof(s_cellPool));
}

/* ========================================================================= */
/* Macro support shims                                                       */
/* ========================================================================= */

void *
CelsMutableStateCreateOrNull(const void *initialValue, size_t valueSize)
{
    void *value = NULL;
    if (CelsMutableStateCreate(initialValue, valueSize, &value) != CELS_OK) {
        return NULL;
    }
    return value;
}

CelsResult
CelsMutableStateReadOrZero(const void *value, size_t valueSize)
{
    CELS_ASSERT(value != NULL);

    if (value == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    CelsMutableCell *const cell = MutableCellResolve(value);
    if (cell == NULL) {
        return CELS_ERROR_INVALID_STATE;
    }
    if (valueSize != cell->valueSize) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    MutableCellWatcherAdd(cell);
    return CELS_OK;
}
