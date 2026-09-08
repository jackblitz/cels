#pragma once

/**
 * @file composer.h
 * @brief Inline diffing and composition engine wrapping the slot table.
 *
 * Declarative DSL usage:
 * @code
 *     CelsSlotTable table;
 *     // Initialize slab and table...
 *
 *     // Root view (like Android Compose setContent)
 *     CEL_Composition(&table, KEY_APP_ROOT) {
 *         // cel_watch diffs parameter state; automatically skips subtree in O(1)
 *         // if identical, or executes the block if changed.
 *         cel_watch(*playerState) {
 *             CEL_Compose(KEY_MESH) {
 *                 // Render mesh...
 *             }
 *             if (playerState->has_shield) {
 *                 CEL_Compose(KEY_SHIELD) {
 *                     // Render shield VFX...
 *                 }
 *             }
 *         }
 *     }
 * @endcode
 *
 * Thread safety: CelsComposer operates on an exclusive CelsSlotTable pass.
 * Ambient composer context is thread-local. Concurrent composition across
 * threads requires separate slot tables.
 */

#include "composition/slottable/slot_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CELS_COMPOSER_MAX_DEPTH 64u

/* Token-pasting helpers for unique loop identifiers */
#define _CEL_CAT2(a, b) a##b
#define _CEL_CAT(a, b) _CEL_CAT2(a, b)

struct ecs_world_t;
typedef struct CelsComposer CelsComposer;
typedef CelsComposer Composer;

/**
 * Inline composer cursor tracking hierarchical diffing and pruning.
 * Fields ordered largest to smallest (pointers -> 32-bit arrays/integers).
 */
struct CelsComposer {
    CelsSlotTable *table;
    struct ecs_world_t *stage;
    uint32_t readerIndex;
    uint32_t currentSlot;
    uint32_t parentStackTop;
    uint32_t skipCount;
    uint32_t parentStack[CELS_COMPOSER_MAX_DEPTH];
    uint32_t groupEndStack[CELS_COMPOSER_MAX_DEPTH];
    uint64_t entityStack[CELS_COMPOSER_MAX_DEPTH];
};

/**
 * Stack scope guard for ambient composer context during root composition passes.
 * Fields ordered largest to smallest (pointers -> 32-bit integer).
 */
typedef struct CelsComposerScope {
    CelsComposer *prev;
    CelsComposer *curr;
    int32_t active;
} CelsComposerScope;

/**
 * String descriptor representing an element or composable label,
 * inspired by Nic Barker's Clay UI.
 */
typedef struct CelsString {
    bool isStatic;
    uint32_t length;
    const char *chars;
} CelsString;

/**
 * Element / Composable ID inspired by Nic Barker's Clay UI.
 */
typedef struct CelsId {
    uint32_t id;          // Computed 32-bit FNV-1a hash key
    uint32_t offset;      // Numerical index / offset (e.g. for dynamic lists)
    uint32_t baseId;      // Base/parent group hash (0 if global/root)
    CelsString stringId;  // Source string
} CelsId;

/**
 * Composition completion result returned by root view functions.
 */
typedef struct CEL_CompositionScope {
    CelsId id;
    uint64_t entity;
    uint32_t groupCount;
} CEL_CompositionScope;
typedef CEL_CompositionScope CelsCompositionScope;

/**
 * 32-bit FNV-1a string hashing for stable composable keys and Clay-style element IDs.
 *
 * @param str    Null-terminated string name/label. Non-NULL.
 * @param offset Optional numeric index/offset to mix into the hash (0 for base name).
 * @return 32-bit FNV-1a hash key.
 */
static inline uint32_t
CelsHashString(const char *str, uint32_t offset)
{
    uint32_t hash = 2166136261u;
    if (str != NULL) {
        while (*str != '\0') {
            hash ^= (uint8_t)(*str++);
            hash *= 16777619u;
        }
    }
    if (offset != 0) {
        hash ^= offset;
        hash *= 16777619u;
    }
    return hash;
}

/**
 * 32-bit FNV-1a string hashing with a base/parent key for locally scoped names.
 *
 * @param str    Null-terminated string name/label. Non-NULL.
 * @param baseId Parent group's key to scope the hash under (0 for global).
 * @param offset Optional numeric index/offset.
 * @return 32-bit scoped FNV-1a hash key.
 */
static inline uint32_t
CelsHashStringWithBase(const char *str, uint32_t baseId, uint32_t offset)
{
    uint32_t hash = 2166136261u;
    if (baseId != 0) {
        hash ^= baseId;
        hash *= 16777619u;
    }
    if (str != NULL) {
        while (*str != '\0') {
            hash ^= (uint8_t)(*str++);
            hash *= 16777619u;
        }
    }
    if (offset != 0) {
        hash ^= offset;
        hash *= 16777619u;
    }
    return hash;
}

/**
 * Constructs a CelsId from a string label and optional numeric offset.
 */
static inline CelsId
CelsIdMake(const char *str, uint32_t offset)
{
    CelsId cid;
    cid.id = CelsHashString(str, offset);
    cid.offset = offset;
    cid.baseId = 0;
    cid.stringId.isStatic = true;
    cid.stringId.chars = str;
    uint32_t len = 0;
    if (str != NULL) {
        while (str[len] != '\0') {
            len++;
        }
    }
    cid.stringId.length = len;
    return cid;
}

/**
 * Constructs a CelsId from an existing 32-bit key.
 */
static inline CelsId
CelsIdFromKey(uint32_t key)
{
    CelsId cid;
    cid.id = key;
    cid.offset = 0;
    cid.baseId = 0;
    cid.stringId.isStatic = true;
    cid.stringId.chars = NULL;
    cid.stringId.length = 0;
    return cid;
}

/**
 * Reference to a found composition group in the slot table.
 */
typedef struct CelsCompositionRef {
    bool found;
    uint32_t key;
    uint32_t logicalIndex;
    uint16_t slotCount;
    uint16_t groupSize;
    uint16_t nodeCount;
    CelsSlotGroup *group;
    CelsSlotValue *slots;
} CelsCompositionRef;

/* ========================================================================= */
/* Core Lifecycle & Traversal API                                            */
/* ========================================================================= */

/**
 * Initializes the composer for an execution pass over the table.
 *
 * @param cmp   Pointer to the composer struct. Non-NULL.
 * @param table Pointer to the target slot table. Non-NULL.
 */
void CelsComposerBegin(CelsComposer *cmp, CelsSlotTable *table);

/**
 * Enters an existing group matching id, or inserts a new group into the gap.
 * Automatically creates and parents an associated Flecs entity if a stage is bound.
 *
 * @param cmp Pointer to the composer. Non-NULL.
 * @param id  Stable callsite ID (hash key + optional string name).
 * @return true if entered successfully, false on capacity exhaustion.
 */
bool CelsComposerGroupStartId(CelsComposer *cmp, CelsId id);

/**
 * Enters an existing group matching key, or inserts a new group into the gap.
 *
 * @param cmp Pointer to the composer. Non-NULL.
 * @param key Stable callsite key / hash.
 * @return true if entered successfully, false on capacity exhaustion.
 */
bool CelsComposerGroupStart(CelsComposer *cmp, uint32_t key);

/**
 * Leaves the current group, updating subtree span and pruning vanished nodes.
 *
 * @param cmp Pointer to the composer. Non-NULL.
 */
void CelsComposerGroupEnd(CelsComposer *cmp);

/**
 * Diffs input data against cached state for the active group.
 * Automatically uses 32/64-bit scalar register comparisons when size is 4 or 8.
 * Overwrites cached slots in-place if changed.
 *
 * @param cmp  Pointer to the composer. Non-NULL.
 * @param data Pointer to input data. Non-NULL.
 * @param size Byte size of input data.
 * @return true if data changed or newly inserted, false if identical.
 */
bool CelsComposerChanged(CelsComposer *cmp, const void *data, size_t size);

/**
 * Skips the active group's nested subtree in O(1) time.
 *
 * @param cmp Pointer to the composer. Non-NULL.
 */
void CelsComposerGroupSkip(CelsComposer *cmp);

/**
 * Returns the number of O(1) subtree skips performed by this composer.
 *
 * @param cmp Pointer to composer. Non-NULL.
 * @return Total skip count.
 */
uint32_t CelsComposerSkipCount(const CelsComposer *cmp);

/* ========================================================================= */
/* Ambient Context & DSL Support API                                         */
/* ========================================================================= */

/**
 * Returns the active ambient composer for the calling thread.
 *
 * @return Pointer to current composer, or NULL if outside a composition pass.
 */
CelsComposer *CelsComposerGetCurrent(void);

/**
 * Sets the active ambient composer for the calling thread.
 *
 * @param cmp Pointer to the new active composer, or NULL.
 */
void CelsComposerSetCurrent(CelsComposer *cmp);

/**
 * Returns a thread-local static composer instance for default passes.
 *
 * @return Pointer to default thread-local composer. Never NULL.
 */
CelsComposer *CelsComposerGetDefault(void);

/**
 * Returns the Flecs stage associated with the composer, if any.
 *
 * @param cmp Pointer to composer. If NULL, queries current ambient composer.
 * @return Flecs stage handle (struct ecs_world_t*), or NULL if no stage is bound.
 */
struct ecs_world_t *CelsComposerGetStage(const CelsComposer *cmp);

/**
 * Returns the Flecs stage associated with the current ambient composer.
 *
 * @return Flecs stage handle (struct ecs_world_t*), or NULL if outside pass or no stage.
 */
struct ecs_world_t *CelsComposerGetCurrentStage(void);

/**
 * Acquires or creates a persistent CelsSlotTable for a given composition root key.
 * If a table already exists for the key, it is reused to preserve recomposition caching.
 *
 * @param key Composition root key.
 * @return Pointer to acquired CelsSlotTable, or NULL on allocation failure.
 */
CelsSlotTable *CelsTableRegistryAcquire(uint32_t key);

/**
 * Releases and frees the slot table associated with a composition root key.
 *
 * @param key Composition root key to release.
 */
void CelsTableRegistryRelease(uint32_t key);

/**
 * Resets and frees all tables in the registry.
 */
void CelsTableRegistryReset(void);

/**
 * Searches all active tables in the registry for a group matching the given key.
 *
 * @param key      Key or hashed name.
 * @param outTable Optional destination to receive the table containing the group.
 * @return Pointer to CelsSlotGroup, or NULL if not found.
 */
CelsSlotGroup *CelsTableRegistryFindGroup(uint32_t key, CelsSlotTable **outTable);

/**
 * Returns the Flecs entity associated with the currently active cel.
 *
 * @return Active ecs_entity_t handle, or 0 if outside cel or no stage.
 */
uint64_t CelsComposerGetCurrentEntity(void);

/**
 * Returns the Flecs entity associated with the composer's currently active group.
 *
 * @param cmp Pointer to composer. If NULL, queries current ambient composer.
 * @return Active ecs_entity_t handle, or 0 if outside cel or no stage.
 */
uint64_t CelsComposerGetEntity(const CelsComposer *cmp);

/**
 * Returns the completed composition result from the last root scope.
 */
CEL_CompositionScope CelsComposerGetLastCompositionScope(void);
CEL_CompositionScope CelsComposerGetLastComposition(void);

/**
 * Initializes ambient context and begins root composition for CEL_CompositionScope.
 * If table is NULL, automatically acquires/assigns its own persistent table for id.
 *
 * @param cmp   Optional explicit composer (NULL uses thread-local default).
 * @param table Optional target slot table (NULL automatically acquires from registry).
 * @param id    Root callsite ID.
 * @return Initialized scope tracking ambient restoration and group status.
 */
CelsComposerScope CelsComposerScopeEnter(CelsComposer *cmp,
                                         CelsSlotTable *table,
                                         CelsId id);

/**
 * Backwards compatibility wrapper for integer keys.
 */
CelsComposerScope CelsComposerScopeEnterKey(CelsComposer *cmp,
                                            CelsSlotTable *table,
                                            uint32_t key);

/**
 * Concludes root composition pass and restores previous ambient context.
 *
 * @param scope Pointer to active composition scope. Non-NULL.
 */
void CelsComposerScopeExit(CelsComposerScope *scope);

/**
 * Enters child group in the current ambient composer for CEL_Compose.
 *
 * @param id Stable callsite ID.
 * @return 1 on success, 0 on failure or missing ambient context.
 */
int32_t CelsComposerGroupEnter(CelsId id);

/**
 * Backwards compatibility wrapper for integer keys.
 */
int32_t CelsComposerGroupEnterKey(uint32_t key);

/**
 * Leaves child group in the current ambient composer for CEL_Compose.
 */
void CelsComposerGroupLeave(void);

/**
 * Enters child group with explicit composer for CEL_Compose.
 *
 * @param cmp Target composer. Non-NULL.
 * @param id  Stable callsite ID.
 * @return 1 on success, 0 on failure.
 */
int32_t CelsComposerGroupEnterExplicit(CelsComposer *cmp, CelsId id);

/**
 * Leaves child group with explicit composer for CEL_Compose.
 *
 * @param cmp Target composer. Non-NULL.
 */
void CelsComposerGroupLeaveExplicit(CelsComposer *cmp);

/**
 * Returns the stable key of the currently active group in the composer.
 *
 * @param cmp Optional composer (NULL uses current ambient composer).
 * @return Active group key, or 0 if outside a group or no ambient context.
 */
uint32_t CelsComposerGetCurrentKey(const CelsComposer *cmp);

/**
 * Searches the active table (or registry) for a group matching key.
 *
 * @param cmp           Optional composer (NULL uses ambient or table registry).
 * @param key           Key or hashed name (e.g. CEL_Name("HealthBar")).
 * @param outLogicalIdx Optional destination for logical index.
 * @return Pointer to CelsSlotGroup, or NULL if not found.
 */
CelsSlotGroup *CelsComposerFindGroup(const CelsComposer *cmp,
                                     uint32_t key,
                                     uint32_t *outLogicalIdx);

/**
 * Searches the active table (or registry) for a composition matching key.
 *
 * @param cmp Optional composer (NULL uses ambient or table registry).
 * @param key Key or hashed name.
 * @return CelsCompositionRef with found status, slots, and metadata.
 */
CelsCompositionRef CelsComposerFind(const CelsComposer *cmp, uint32_t key);

/**
 * Enters child group and diffs input state in the current ambient composer.
 * If the state is identical, automatically skips the group in O(1) and leaves
 * the group immediately, returning 0 so the child block never executes.
 *
 * @param id        Stable callsite ID.
 * @param stateData Pointer to state struct/variable to diff. Non-NULL.
 * @param stateSize Size of state struct/variable in bytes.
 * @return 1 if changed or first mount (enter block), 0 if skipped or failed.
 */
int32_t CelsComposerGroupEnterStateful(CelsId id,
                                       const void *stateData,
                                       size_t stateSize);

/**
 * Explicit composer variant of CelsComposerGroupEnterStateful.
 *
 * @param cmp       Target composer. Non-NULL.
 * @param id        Stable callsite ID.
 * @param stateData Pointer to state struct/variable to diff. Non-NULL.
 * @param stateSize Size of state struct/variable in bytes.
 * @return 1 if changed or first mount (enter block), 0 if skipped or failed.
 */
int32_t CelsComposerGroupEnterStatefulExplicit(CelsComposer *cmp,
                                               CelsId id,
                                               const void *stateData,
                                               size_t stateSize);

/**
 * Diffs data against active group; skips subtree in O(1) if unchanged.
 *
 * @param cmp  Optional composer (NULL uses ambient composer).
 * @param data Pointer to input data. Non-NULL.
 * @param size Byte size of input data.
 * @return true if data changed (enter block), false if identical (skipped).
 */
bool CelsComposerWatchEnter(CelsComposer *cmp, const void *data, size_t size);

/**
 * Diffs an ECS query against active group's slot memory; skips subtree in O(1) if unchanged.
 *
 * Flecs Integration Notes:
 * - In Flecs, ecs_query_t tracks table match ticks and archetype versions.
 * - When bridging to Flecs, CelsComposerQueryEnter checks ecs_query_changed(q) or
 *   compares the query's match tick against the slot table cache.
 * - If unchanged: calls CelsComposerGroupSkip and returns false (bypassing the loop).
 * - If changed: updates slot cache and returns true (recomposes matching entities).
 *
 * @param cmp       Optional composer (NULL uses ambient composer).
 * @param queryData Pointer to query descriptor, handle, or tick struct. Non-NULL.
 * @param querySize Size of query descriptor in bytes.
 * @return true if query results changed (enter block), false if unchanged (skip in O(1)).
 */
bool CelsComposerQueryEnter(CelsComposer *cmp,
                            const void *queryData,
                            size_t querySize);

/**
 * Diffs an observable state or Flecs query against the slot table cache.
 * If unchanged: skips the active group in O(1) time and returns false.
 * If changed: copies the BEFORE-recomposition data from the slot table into
 * outPrevious (if non-NULL), updates the slot table cache in-place with the new
 * data, and returns true.
 *
 * @param cmp         Optional composer (NULL uses ambient composer).
 * @param data        Pointer to current data snapshot. Non-NULL.
 * @param size        Byte size of data.
 * @param outPrevious Optional destination to receive the data from BEFORE recomposition.
 * @return true if data changed (enter block), false if unchanged (skipped in O(1)).
 */
bool CelsComposerObservableEnter(CelsComposer *cmp,
                                 const void *data,
                                 size_t size,
                                 void *outPrevious);

/**
 * Memoizes ephemeral UI state in the active group's slot table storage.
 * On initial mount, allocates slots into the slot gap and seeds with initialData.
 * On subsequent recompositions, returns a pointer to the existing slot without
 * overwriting changes.
 *
 * Flecs Architecture Notes:
 * - Ephemeral UI state (e.g. isHovered, scrollOffset) is retained in the slot table
 *   without polluting the Flecs world with throwaway entities.
 * - When bridging to Flecs-backed entities, CelsSlotGroup.entityId stores the
 *   Flecs ecs_entity_t handle. When the group vanishes from the slot table,
 *   CelsComposerGroupEnd can automatically trigger ecs_delete on that handle.
 *
 * @param cmp         Optional composer (NULL uses ambient composer).
 * @param initialData Pointer to initial seed data, or NULL for zero-init.
 * @param size        Size of data in bytes.
 * @return Pointer to the persistent slot memory, or NULL on capacity error.
 */
void *CelsComposerRemember(CelsComposer *cmp,
                           const void *initialData,
                           size_t size);

/* ========================================================================= */
/* Declarative DSL Macros                                                    */
/* ========================================================================= */
/* Declarative DSL Macros                                                    */
/* ========================================================================= */

#define _CEL_COMPOSITION_SCOPE_IMPL(cmp, table, id, uid)                      \
    for (CelsComposerScope uid = CelsComposerScopeEnter((cmp), (table), (id)); \
         uid.active;                                                           \
         CelsComposerScopeExit(&uid))

#define _CEL_COMPOSITION_SCOPE_3(cmp, table, id)                               \
    _CEL_COMPOSITION_SCOPE_IMPL((cmp), (table), (id),                          \
                                _CEL_CAT(_cel_scope_, __LINE__))

#define _CEL_COMPOSITION_SCOPE_2(table, id)                                    \
    _CEL_COMPOSITION_SCOPE_IMPL(NULL, (table), (id),                           \
                                _CEL_CAT(_cel_scope_, __LINE__))

#define _CEL_COMPOSITION_SCOPE_1(id)                                           \
    _CEL_COMPOSITION_SCOPE_IMPL(NULL, NULL, (id),                              \
                                _CEL_CAT(_cel_scope_, __LINE__))

#define _CEL_COMPOSITION_SCOPE_DISPATCH(_1, _2, _3, NAME, ...) NAME

/**
 * @brief Root composition view scope (Android Compose setContent equivalent).
 *
 * Supports:
 * - 1-argument: CEL_Composition(id) -> automatically acquires and manages its own slot table!
 * - 2-argument: CEL_Composition(table, id) -> uses caller's explicit slot table.
 * - 3-argument: CEL_Composition(cmp, table, id) -> uses explicit composer and table.
 *
 * Automatically initializes the pass, opens root group on '{', and closes
 * group & restores ambient context on '}'.
 */
#define CEL_Composition(...)                                                   \
    _CEL_COMPOSITION_SCOPE_DISPATCH(                                           \
        __VA_ARGS__, _CEL_COMPOSITION_SCOPE_3, _CEL_COMPOSITION_SCOPE_2,        \
        _CEL_COMPOSITION_SCOPE_1, 0)(__VA_ARGS__)
#define CELS_COMPOSITION(...) CEL_Composition(__VA_ARGS__)

/* Shorthand aliases */
#define CEL_Scope(...) CEL_Composition(__VA_ARGS__)
#define CELS_SCOPE(...) CEL_Composition(__VA_ARGS__)

/* Disposal / Freeing macro */
#define CEL_Dispose(_cels_id_expr) CelsTableRegistryRelease((_cels_id_expr).id)
#define cel_dispose(_cels_id_expr) CEL_Dispose(_cels_id_expr)
#define CELS_DISPOSE(_cels_id_expr) CEL_Dispose(_cels_id_expr)


/* --- CEL_Compose Implementation (Style B & Stateless) --- */

#define _CEL_COMPOSE_IMPL_STATEFUL(id, state_ptr, state_size, uid)             \
    for (int32_t uid = CelsComposerGroupEnterStateful(                         \
             (id), (state_ptr), (state_size));                                 \
         uid;                                                                  \
         CelsComposerGroupLeave(), uid = 0)

#define _CEL_COMPOSE_IMPL_STATELESS(id, uid)                                   \
    for (int32_t uid = CelsComposerGroupEnter(id); uid;                        \
         CelsComposerGroupLeave(), uid = 0)

#define _CEL_COMPOSE_IMPL_EXPLICIT_STATEFUL(cmp, id, state_ptr, state_size, uid) \
    for (int32_t uid = CelsComposerGroupEnterStatefulExplicit(                 \
             (cmp), (id), (state_ptr), (state_size));                          \
         uid;                                                                  \
         CelsComposerGroupLeaveExplicit(cmp), uid = 0)

#define _CEL_COMPOSE_3(cmp, id, state)                                         \
    _CEL_COMPOSE_IMPL_EXPLICIT_STATEFUL((cmp), (id), &(state), sizeof(state),  \
                                        _CEL_CAT(_cel_cmp_, __LINE__))

#define _CEL_COMPOSE_2(id, state)                                              \
    _CEL_COMPOSE_IMPL_STATEFUL((id), &(state), sizeof(state),                  \
                               _CEL_CAT(_cel_cmp_, __LINE__))

#define _CEL_COMPOSE_1(id)                                                     \
    _CEL_COMPOSE_IMPL_STATELESS((id), _CEL_CAT(_cel_cmp_, __LINE__))

#define _CEL_COMPOSE_DISPATCH(_1, _2, _3, NAME, ...) NAME

/**
 * @brief Declare a child composable node in the composition hierarchy.
 *
 * Supports:
 * - Stateless: CEL_Compose(id)
 * - Style B (Stateful): CEL_Compose(id, state) - diffs state and skips in O(1)
 * - Explicit: CEL_Compose(cmp, id, state)
 *
 * Automatically opens group on '{' and closes/prunes on '}'.
 */
#define CEL_Compose(...)                                                       \
    _CEL_COMPOSE_DISPATCH(__VA_ARGS__, _CEL_COMPOSE_3, _CEL_COMPOSE_2,         \
                          _CEL_COMPOSE_1, 0)(__VA_ARGS__)
#define CELS_COMPOSE(...) CEL_Compose(__VA_ARGS__)

/* --- CEL_watch Implementation --- */

#define _CEL_WATCH_IMPL(cmp, data_ptr, size, uid)                             \
    for (int32_t uid = (CelsComposerWatchEnter((cmp), (data_ptr), (size))      \
                            ? 1                                                \
                            : 0);                                              \
         uid;                                                                  \
         uid = 0)

#define _CEL_WATCH_2(cmp, variable)                                            \
    _CEL_WATCH_IMPL((cmp), &(variable), sizeof(variable),                     \
                    _CEL_CAT(_cel_wtc_, __LINE__))

#define _CEL_WATCH_1(variable)                                                 \
    _CEL_WATCH_IMPL(NULL, &(variable), sizeof(variable),                      \
                    _CEL_CAT(_cel_wtc_, __LINE__))

#define _CEL_WATCH_DISPATCH(_1, _2, NAME, ...) NAME

/**
 * @brief Parameter diffing reactive scope with automatic O(1) subtree skipping.
 *
 * This is a PULL primitive: it memcmps a value against the slot-table cache at
 * the point of read and skips the block when nothing changed. It is unrelated
 * to the PUSH primitive of the same-sounding name in state.h — CEL_Watch(cell)
 * there reads a reactive cell and subscribes the active composable to it.
 *
 * The two were briefly spelled alike. They are not alternatives:
 *   cel_watch(value) { ... }   diff a value you already hold, skip if equal
 *   CEL_Watch(cell)            read a cell, and be re-run when it changes
 */
#define cel_watch(...)                                                         \
    _CEL_WATCH_DISPATCH(__VA_ARGS__, _CEL_WATCH_2, _CEL_WATCH_1, 0)(__VA_ARGS__)
#define CEL_watch(...) cel_watch(__VA_ARGS__)
#define CELS_WATCH(...) cel_watch(__VA_ARGS__)

/** Preferred PascalCase spelling of the diffing scope, since CEL_Watch is the
 *  reactive-cell read in state.h. */
#define CEL_Changed(...) cel_watch(__VA_ARGS__)
#define cel_changed(...) cel_watch(__VA_ARGS__)

/* --- CEL_query Implementation (Reactive ECS Query Observer) --- */

#define _CEL_QUERY_IMPL(cmp, query_ptr, query_size, uid)                      \
    for (int32_t uid = (CelsComposerQueryEnter((cmp), (query_ptr), (query_size)) \
                            ? 1                                                \
                            : 0);                                              \
         uid;                                                                  \
         uid = 0)

#define _CEL_QUERY_2(cmp, query)                                               \
    _CEL_QUERY_IMPL((cmp), &(query), sizeof(query),                            \
                    _CEL_CAT(_cel_qry_, __LINE__))

#define _CEL_QUERY_1(query)                                                    \
    _CEL_QUERY_IMPL(NULL, &(query), sizeof(query),                             \
                    _CEL_CAT(_cel_qry_, __LINE__))

#define _CEL_QUERY_DISPATCH(_1, _2, NAME, ...) NAME

/**
 * @brief Reactive ECS query diff guard with automatic O(1) subtree skipping.
 *
 * Evaluates whether an ECS query's result set has changed (entities added,
 * removed, or component data mutated). If the query is clean, the entire subtree
 * is skipped in O(1) time.
 *
 * Flecs Architecture Notes:
 * - Tier 1 (Macro level): CEL_query checks ecs_query_changed() or archetype ticks.
 *   If no entities in the query changed, 10,000 entities skip in 1 nanosecond.
 * - Tier 2 (Entity level): When the query changes, iteration uses CEL_Compose(entityId, state)
 *   to only recompose modified entities, while unchanged entities skip individually in O(1).
 *
 * Usage:
 * @code
 *     CEL_query(enemyQuery) {
 *         // Iterates query matches...
 *         CEL_Compose(enemy.id, enemy.state) {
 *             DrawHealthBar(enemy.health);
 *         }
 *     }
 * @endcode
 */
#define CEL_query(...)                                                         \
    _CEL_QUERY_DISPATCH(__VA_ARGS__, _CEL_QUERY_2, _CEL_QUERY_1, 0)(__VA_ARGS__)
#define CEL_Query(...) CEL_query(__VA_ARGS__)
#define cel_query(...) CEL_query(__VA_ARGS__)
#define CELS_QUERY(...) CEL_query(__VA_ARGS__)

/* --- CEL_observable Implementation (Observable State & Flecs Query Diff) --- */

#define _CEL_OBSERVABLE_IMPL(cmp, data_ptr, size, prev_ptr, uid)               \
    for (int32_t uid = (CelsComposerObservableEnter(                           \
                            (cmp), (data_ptr), (size), (prev_ptr))             \
                            ? 1                                                \
                            : 0);                                              \
         uid;                                                                  \
         uid = 0)

#define _CEL_OBSERVABLE_2(observable, prev_var)                                \
    _CEL_OBSERVABLE_IMPL(NULL, &(observable), sizeof(observable),              \
                         &(prev_var), _CEL_CAT(_cel_obs_, __LINE__))

#define _CEL_OBSERVABLE_1(observable)                                          \
    _CEL_OBSERVABLE_IMPL(NULL, &(observable), sizeof(observable),              \
                         NULL, _CEL_CAT(_cel_obs_, __LINE__))

#define _CEL_OBSERVABLE_DISPATCH(_1, _2, NAME, ...) NAME

/**
 * @brief Reactive observable diff guard for state and Flecs queries.
 *
 * Supports both 1-argument and 2-argument forms:
 * - CEL_observable(obs): Diffs obs against the slot table. If unchanged, skips
 *   in O(1). If changed, enters the block and recomposes.
 * - CEL_observable(obs, prev_var): Diffs obs and populates prev_var with the
 *   data from BEFORE recomposition, allowing direct comparison of old vs new values!
 *
 * Usage:
 * @code
 *     CEL_observable(enemyQuery, prevQuery) {
 *         printf("Query changed! Count before: %d, count now: %d\n",
 *                prevQuery.count, enemyQuery.count);
 *     }
 * @endcode
 */
#define CEL_observable(...)                                                    \
    _CEL_OBSERVABLE_DISPATCH(__VA_ARGS__, _CEL_OBSERVABLE_2,                    \
                             _CEL_OBSERVABLE_1, 0)(__VA_ARGS__)

/* Aliases */
#define CEL_Observable(...) CEL_observable(__VA_ARGS__)
#define cel_observable(...) CEL_observable(__VA_ARGS__)
#define CEL_Observeable(...) CEL_observable(__VA_ARGS__)
#define cel_observeable(...) CEL_observable(__VA_ARGS__)
#define CELS_OBSERVABLE(...) CEL_observable(__VA_ARGS__)

/* --- CEL_remember Implementation (Slot Table Ephemeral State) --- */

/**
 * @brief Memoize ephemeral UI state in the active group's slot table.
 *
 * Retains state across recompositions without polluting the Flecs world.
 * Returns a pointer to the persistent slot memory holding the value.
 *
 * Flecs Integration Notes:
 * - For pure UI state (isHovered, scrollOffset), state lives in the slot table.
 * - When bridging to Flecs, CelsSlotGroup.entityId tracks the Flecs entity.
 *
 * Example:
 * @code
 *     bool initHover = false;
 *     bool *isHovered = CEL_remember(initHover);
 * @endcode
 */
#define CEL_remember(initial_var)                                              \
    CelsComposerRemember(NULL, &(initial_var), sizeof(initial_var))
#define cel_remember(initial_var) CEL_remember(initial_var)
#define CEL_Remember(initial_var) CEL_remember(initial_var)
#define CELS_REMEMBER(initial_var) CEL_remember(initial_var)

#define CEL_remember_type(Type, initial_var)                                   \
    ((Type *)CelsComposerRemember(NULL, &(initial_var), sizeof(Type)))
#define cel_remember_type(Type, initial_var) CEL_remember_type(Type, initial_var)
#define CEL_RememberType(Type, initial_var) CEL_remember_type(Type, initial_var)
#define CELS_REMEMBER_TYPE(Type, initial_var) CEL_remember_type(Type, initial_var)

/**
 * @brief Boolean dirty check without automatic block skipping.
 */
#define cel_is_dirty(variable)                                                 \
    CelsComposerChanged(CelsComposerGetCurrent(), &(variable), sizeof(variable))
#define CEL_IsDirty(variable) cel_is_dirty(variable)
#define CELS_IS_DIRTY(variable) cel_is_dirty(variable)

/**
 * @brief Manual O(1) group subtree skip.
 */
#define cel_skip() CelsComposerGroupSkip(CelsComposerGetCurrent())
#define CEL_Skip() cel_skip()
#define CELS_SKIP() cel_skip()

/**
 * @brief Convenience macro for explicit composer diffing.
 */
#define CELS_CHANGED(cmp, variable)                                            \
    CelsComposerChanged((cmp), &(variable), sizeof(variable))

/* ========================================================================= */
/* ========================================================================= */
/* Name, ID & Entity Macros (inspired by Nic Barker's Clay UI)               */
/* ========================================================================= */

#define _CEL_NAME_1(str) CelsIdMake((str), 0)
#define _CEL_NAME_2(str, idx) CelsIdMake((str), (uint32_t)(idx))
#define _CEL_NAME_DISPATCH(_1, _2, NAME, ...) NAME

/**
 * @brief Produces a unique CelsId containing a 32-bit FNV-1a hash key and string name.
 * Inspired by Nic Barker's Clay UI (CLAY_ID / CLAY_IDI).
 *
 * Supports:
 * - 1-argument: CEL_Name("HealthBar")
 * - 2-argument: CEL_Name("InventorySlot", index)
 */
#define CEL_Name(...)                                                          \
    _CEL_NAME_DISPATCH(__VA_ARGS__, _CEL_NAME_2, _CEL_NAME_1)(__VA_ARGS__)
#define cel_name(...) CEL_Name(__VA_ARGS__)
#define CEL_NAME(...) CEL_Name(__VA_ARGS__)

#define CEL_NameI(str, idx) CelsIdMake((str), (uint32_t)(idx))
#define cel_name_i(str, idx) CelsIdMake((str), (uint32_t)(idx))

/**
 * @brief Creates a CelsId from an existing 32-bit integer key.
 */
#define CEL_Key(k) CelsIdFromKey((uint32_t)(k))
#define cel_key(k) CEL_Key(k)

/**
 * @brief Produces a unique CelsId scoped under the currently active parent group.
 */
#define CEL_NameLocal(str)                                                     \
    CelsIdMake((str), CelsComposerGetCurrentKey(NULL))
#define cel_name_local(str) CEL_NameLocal(str)

#define CEL_NameLocalI(str, idx)                                               \
    CelsIdMake((str), CelsComposerGetCurrentKey(NULL) ^ (uint32_t)(idx))
#define cel_name_local_i(str, idx) CEL_NameLocalI(str, idx)

#define CEL_NameScoped(parent_name, child_name)                                \
    CelsIdMake((child_name), CelsHashString((parent_name), 0))
#define cel_name_scoped(parent_name, child_name) CEL_NameScoped(parent_name, child_name)

/**
 * @brief Finds the CelsSlotGroup associated with a composition name or key.
 */
#define CEL_FindGroup(_cels_id_expr) CelsComposerFindGroup(NULL, (_cels_id_expr).id, NULL)
#define cel_find_group(_cels_id_expr) CEL_FindGroup(_cels_id_expr)

/**
 * @brief Finds a composition and returns a CelsCompositionRef.
 */
#define CEL_Find(_cels_id_expr) CelsComposerFind(NULL, (_cels_id_expr).id)
#define cel_find(_cels_id_expr) CEL_Find(_cels_id_expr)

/**
 * @brief Convenience string-literal lookup macro.
 */
#define CEL_FindByName(str) CEL_Find(CEL_Name(str))

/* ========================================================================= */
/* Component DSL & Entity Management Macros                                   */
/* ========================================================================= */

/**
 * @brief Declares a component struct and its Flecs component identifier.
 *
 * Example:
 * @code
 *     CEL_Component(Position) {
 *         float x;
 *         float y;
 *     };
 * @endcode
 */
#define CEL_Component(Type)                                                    \
    typedef struct Type Type;                                                  \
    ECS_COMPONENT_DECLARE(Type);                                               \
    struct Type

/**
 * @brief Registers a component with the Flecs world.
 *
 * Example:
 * @code
 *     CEL_RegisterComponent(world, Position);
 * @endcode
 */
#define CEL_RegisterComponent(world, Type)                                     \
    do {                                                                        \
        ECS_COMPONENT_DEFINE((world), Type);                                    \
        const ecs_entity_t _cels_dummy = ecs_new(world);                        \
        Type _cels_val;                                                         \
        memset(&_cels_val, 0, sizeof(_cels_val));                               \
        ecs_set_id((world), _cels_dummy, ecs_id(Type), sizeof(Type), &_cels_val); \
        ecs_delete((world), _cels_dummy);                                       \
    } while (0)

/**
 * @brief Returns the active cel's Flecs entity handle.
 */
#define CEL_Entity() CelsComposerGetCurrentEntity()
#define cel_entity() CelsComposerGetCurrentEntity()

/**
 * @brief Sets component data on the active cel's Flecs entity.
 *
 * Automatically resolves the active worker stage and current cel entity.
 *
 * Example:
 * @code
 *     CEL_Has(Position, { .x = 10.0f, .y = 20.0f });
 * @endcode
 */
#define CEL_Has(Component, ...)                                                \
    ecs_set(CelsComposerGetCurrentStage(),                                     \
            (ecs_entity_t)CelsComposerGetCurrentEntity(),                      \
            Component,                                                         \
            __VA_ARGS__)

/**
 * @brief Adds a tag / marker component to the active cel's Flecs entity.
 *
 * Example:
 * @code
 *     CEL_Tag(IsPlayer);
 * @endcode
 */
#define CEL_Tag(Tag)                                                           \
    ecs_add(CelsComposerGetCurrentStage(),                                     \
            (ecs_entity_t)CelsComposerGetCurrentEntity(),                      \
            Tag)

/**
 * @brief Concludes root composition view and returns the completed composition scope.
 */
#define CEL_CompositionScopeDone() CelsComposerGetLastCompositionScope()
#define CEL_CompositionDone() CelsComposerGetLastCompositionScope()
