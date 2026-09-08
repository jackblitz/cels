#pragma once

/**
 * @file state.h
 * @brief Reactive state cells written from outside composition.
 *
 * A cell holds one value and the list of composables that read it. Writing a
 * cell does not compose anything: it compares, writes, and queues an
 * invalidation for every subscriber. All work happens inside the next
 * CelsSessionRecompose.
 *
 * Typical usage:
 * @code
 *     CEL_Mutable(PlayerData) {
 *         int hp;
 *     };
 *
 *     static PlayerData *g_player;
 *
 *     void PlayerView(PlayerData *state)
 *     {
 *         CEL_Compose(CEL_Name("Player")) {
 *             const PlayerData player = CEL_Watch(state);
 *             if (player.hp <= 0) {
 *                 CEL_Compose(CEL_Name("DeathScreen")) { }
 *             }
 *         }
 *     }
 *
 *     // Elsewhere — no active composable, so this is just a read:
 *     void OnDamage(int amount)
 *     {
 *         PlayerData next = CEL_Watch(g_player);
 *         next.hp -= amount;
 *         cel_update(g_player, next);
 *     }
 *
 *     int main(void) { g_player = CEL_MutableState(PlayerData, initial); }
 * @endcode
 *
 * Cells come from a fixed static pool; nothing here allocates. A cell outlives
 * the sessions that watch it, which is why every path that destroys a group
 * must unsubscribe it — see CelsMutableStateUnsubscribe.
 *
 * Thread safety: none. Cells are read and written on the composition thread
 * only. CelsMutableStateUpdate appends to a host's invalidation queue without
 * locking, so calling it from another thread races with recomposition.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "composition/slottable/slot_table.h"

/** Maximum number of live cells across the whole process. */
#define CELS_MAX_MUTABLE_CELLS 256u

/**
 * Subscribers tracked individually per cell. A cell that exceeds this stops
 * tracking individuals and invalidates its watchers' whole hosts instead —
 * coarser, still correct. See CelsMutableStateUpdate.
 */
#define CELS_MAX_WATCHERS_PER_CELL 8u

/** Largest value a single cell can hold, in bytes. */
#define CELS_MUTABLE_VALUE_CAPACITY 192u

/**
 * Guard word written into every cell header, checked on each access in debug
 * builds. It is the only defence against a caller passing a pointer that did
 * not come from CelsMutableStateCreate — C's type system cannot tell the two
 * apart, since both are just a Type *.
 */
#define CELS_MUTABLE_CELL_SENTINEL 0x43454C53544154EFull

/* CelsCompositionHost is forward-declared in slot_table.h. */

/**
 * One subscriber: a composable, and the host whose queue an invalidation for
 * it must be appended to.
 */
typedef struct CelsWatcherRef {
    CelsCompositionHost *host;      // Host owning the subscribed composable
    CelsComposableId composable;    // Logical group index within that host
    uint32_t _padding;              // Explicit tail padding
} CelsWatcherRef;

/**
 * Bookkeeping stored immediately BEFORE a cell's value.
 *
 * CelsMutableStateCreate returns a pointer to the value, not to this struct,
 * so that a cell reads as an ordinary Type * at the call site. Accessors
 * recover the header by negative offset.
 * Fields ordered largest to smallest.
 */
typedef struct CelsMutableCell {
    uint64_t sentinel;                                   // CELS_MUTABLE_CELL_SENTINEL
    CelsWatcherRef watchers[CELS_MAX_WATCHERS_PER_CELL]; // Reverse index
    size_t valueSize;                                    // Byte size of the value
    uint16_t watcherCount;                               // Entries used in watchers
    bool overflowed;                                     // Watcher list overran; dirty whole hosts
    bool inUse;                                          // Slot is live in the pool
    uint8_t _padding[4];                                 // Explicit tail padding
} CelsMutableCell;

/* ========================================================================= */
/* Cell lifecycle                                                            */
/* ========================================================================= */

/**
 * Creates a cell holding a copy of an initial value.
 *
 * Takes a slot from the static pool, writes the header and the value, and
 * returns a pointer to the value. The returned pointer is what every other
 * function in this header expects; it is never freed individually.
 *
 * @param initialValue Value to copy in. Non-NULL.
 * @param valueSize    Byte size of the value, in (0, CELS_MUTABLE_VALUE_CAPACITY].
 * @param outValue     On success, receives a pointer to the stored value.
 *                     Untouched on failure.
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or CELS_ERROR_CAPACITY_EXCEEDED
 *         when the pool is exhausted.
 */
CelsResult CelsMutableStateCreate(const void *initialValue,
                                  size_t valueSize,
                                  void **outValue);

/**
 * Reads a cell's current value, subscribing the active composable to it.
 *
 * If a composition walk is in progress, the composable currently being composed
 * is added to the cell's watcher list; subscribing is idempotent, so reading the
 * same cell twice in one body registers once. Outside composition this is a
 * plain read and nothing is subscribed.
 *
 * @param value     Pointer returned by CelsMutableStateCreate. Non-NULL.
 * @param outValue  Receives a copy of the current value. Non-NULL.
 * @param valueSize Byte size to copy; must match the cell's valueSize.
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or CELS_ERROR_INVALID_STATE if
 *         value does not carry a valid cell header.
 */
CelsResult CelsMutableStateRead(const void *value,
                                void *outValue,
                                size_t valueSize);

/**
 * Writes a cell and queues an invalidation for every composable that read it.
 *
 * Compares the whole value with memcmp first: an update that changes nothing
 * queues nothing. Composes nothing itself — the queued work is picked up by the
 * next CelsSessionRecompose, or by the next drain iteration of one already
 * running.
 *
 * A cell whose watcher list overflowed invalidates each watcher's whole host
 * rather than individual composables.
 *
 * @param value     Pointer returned by CelsMutableStateCreate. Non-NULL.
 * @param newValue  Value to copy in. Non-NULL.
 * @param valueSize Byte size to copy; must match the cell's valueSize.
 * @return CELS_OK, CELS_ERROR_INVALID_ARGUMENT, or CELS_ERROR_INVALID_STATE if
 *         value does not carry a valid cell header.
 */
CelsResult CelsMutableStateUpdate(void *value,
                                  const void *newValue,
                                  size_t valueSize);

/**
 * Removes one composable from every cell that it subscribed to.
 *
 * Must be called before a group's slot space is reclaimed. Ids are reused, so a
 * subscription that outlives its composable does not leak harmlessly — it
 * aliases onto whatever composable next occupies that slot, and the next
 * update to that cell would invalidate the wrong one.
 *
 * @param host       Host owning the composable. NULL is accepted and ignored.
 * @param composable Logical group index being destroyed.
 */
void CelsMutableStateUnsubscribe(CelsCompositionHost *host,
                                 CelsComposableId composable);

/**
 * Removes every subscription belonging to a host, whatever the composable.
 *
 * Called by CelsSessionDestroy: cells outlive sessions, and a surviving
 * watcher would hold a pointer into a freed slab.
 *
 * @param host Host being torn down. NULL is accepted and ignored.
 */
void CelsMutableStateUnsubscribeHost(CelsCompositionHost *host);

/**
 * Releases every cell in the pool.
 *
 * Intended for test isolation; a normal program never needs it, since cells
 * live for the life of the process.
 */
void CelsMutableStateResetPool(void);

/* ========================================================================= */
/* Ambient composition context                                               */
/* ========================================================================= */

/**
 * Publishes which host is currently composing, so CelsMutableStateRead can
 * record a subscription against it.
 *
 * The recompose walk sets this before running a host's root composable and
 * clears it (NULL) afterwards. Reading a cell while this is NULL is a plain
 * read with no subscription — which is exactly what makes the same CEL_Watch
 * usable from a network handler and from inside a composable body.
 *
 * @param host Host about to compose, or NULL to leave composition.
 */
void CelsInvalidationContextSet(CelsCompositionHost *host);

/**
 * Returns the host currently composing, or NULL outside a composition walk.
 */
CelsCompositionHost *CelsInvalidationContextGet(void);

/**
 * Publishes the mount/prune callbacks the composition walk should fire.
 *
 * The recompose walk sets this from the session's own context before composing
 * and clears it (NULL) afterwards. The composer fires through it inline, at the
 * exact moment a composable mounts or is pruned, so a body can always rely on
 * its own onCreate having already run this same pass.
 *
 * Ambient rather than threaded through the composer because the composer and
 * the session must not depend on each other's headers.
 *
 * @param context Callbacks to publish, or NULL to clear.
 */
void CelsTransactionContextSet(const CelsTransactionContext *context);

/**
 * Returns the active mount/prune callbacks, or NULL if none are published.
 */
const CelsTransactionContext *CelsTransactionContextGet(void);

/**
 * Fires onCreate, if one is published. Safe to call unconditionally.
 *
 * @param composable Composable that just mounted.
 * @param parent     Its parent, or CELS_COMPOSABLE_ID_INVALID for a root.
 * @param key        The callsite key it mounted under.
 */
void CelsTransactionNotifyCreate(CelsComposableId composable,
                                 CelsComposableId parent,
                                 uint32_t key);

/**
 * Fires onDestroy, if one is published. Safe to call unconditionally.
 *
 * @param composable Composable that was just pruned.
 */
void CelsTransactionNotifyDestroy(CelsComposableId composable);

/* ========================================================================= */
/* DSL                                                                       */
/* ========================================================================= */

/**
 * Declares the shape of a reactive value.
 *
 * @code
 *     CEL_Mutable(PlayerData) {
 *         int hp;
 *         int mana;
 *     };
 * @endcode
 */
#define CEL_Mutable(Name) typedef struct Name Name; struct Name

/**
 * Creates a cell and yields a Type * to its value, or NULL if the pool is full.
 *
 * The initial value is named rather than passed as a compound literal, per the
 * project's initialization rules.
 *
 * @code
 *     const PlayerData initial = { .hp = 100 };
 *     g_player = CEL_MutableState(PlayerData, initial);
 * @endcode
 */
#define CEL_MutableState(Type, initial_var)                                    \
    ((Type *)CelsMutableStateCreateOrNull(&(initial_var), sizeof(Type)))

/**
 * Reads a cell, subscribing the active composable if there is one.
 *
 * Yields a value of the cell's type, so it reads like an ordinary assignment:
 * `const PlayerData player = CEL_Watch(state);`
 */
#define CEL_Watch(cell_ptr)                                                    \
    (*(CelsMutableStateReadOrZero((cell_ptr), sizeof(*(cell_ptr))),            \
       (cell_ptr)))

/**
 * Writes a cell and queues invalidations for its subscribers.
 *
 * @code
 *     PlayerData next = CEL_Watch(g_player);
 *     next.hp -= amount;
 *     cel_update(g_player, next);
 * @endcode
 */
#define cel_update(cell_ptr, new_value_var)                                    \
    ((void)CelsMutableStateUpdate((cell_ptr), &(new_value_var),                \
                                  sizeof(*(cell_ptr))))

/** PascalCase alias, for callers preferring the API casing. */
#define CEL_Update(cell_ptr, new_value_var) cel_update(cell_ptr, new_value_var)

/* ========================================================================= */
/* Macro support shims                                                       */
/* ========================================================================= */

/**
 * Creates a cell and returns the value pointer directly, or NULL on failure.
 *
 * Exists so CEL_MutableState can be an expression. Callers wanting the failure
 * reason should use CelsMutableStateCreate.
 *
 * @param initialValue Value to copy in. Non-NULL.
 * @param valueSize    Byte size of the value.
 * @return Pointer to the stored value, or NULL if the pool is exhausted.
 */
void *CelsMutableStateCreateOrNull(const void *initialValue, size_t valueSize);

/**
 * Subscribes the active composable to a cell without copying the value out.
 *
 * Exists so CEL_Watch can subscribe and then yield the cell's own storage as
 * the expression result, avoiding a temporary of unknown type.
 *
 * @param value     Pointer returned by CelsMutableStateCreate. Non-NULL.
 * @param valueSize Byte size of the value; must match the cell's valueSize.
 * @return CELS_OK, or an error if value carries no valid cell header.
 */
CelsResult CelsMutableStateReadOrZero(const void *value, size_t valueSize);
