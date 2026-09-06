#ifndef CELS_H
#define CELS_H

/**
 * @file cels.h
 * @brief Public API and Declarative DSL Macros for Cels Composition Engine.
 *
 * Cels is a high-performance Jetpack Compose-style reactive composition engine
 * for C99, powered by a dual-gap buffer Slot Table and ambient context.
 */

#include "composition/composer.h"
#include "composition/recomposition_dispatcher.h"
#include "composition/slottable/slot_table.h"

/* ========================================================================= */
/* 1. Header Declaration Macros (for .h files)                               */
/* ========================================================================= */

#define _CEL_DECL_DISPATCH(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME

/* --- Composition Scope Declaration (CEL_CompositionScope_Decl) --- */

#define _CEL_SCOPE_DECL_1(name)                                                \
    CEL_CompositionScope name(void)

#define _CEL_SCOPE_DECL_MORE(name, ...)                                        \
    CEL_CompositionScope name(__VA_ARGS__)

/**
 * @brief Declares a root composition scope prototype in a header (.h) file.
 *
 * CEL_Composition manages its own slot table automatically without
 * requiring a CelsSlotTable* parameter.
 *
 * Usage in header (.h):
 * @code
 *     // With state parameter(s):
 *     CEL_CompositionScope_Decl(PlayerHudScope, const PlayerState *state);
 *     // Emits: CEL_CompositionScope PlayerHudScope(const PlayerState *state);
 *
 *     // Without additional parameters:
 *     CEL_CompositionScope_Decl(AppRootScope);
 *     // Emits: CEL_CompositionScope AppRootScope(void);
 * @endcode
 */
#define CEL_CompositionScope_Decl(...)                                         \
    _CEL_DECL_DISPATCH(__VA_ARGS__,                                            \
                        _CEL_SCOPE_DECL_MORE,                                  \
                        _CEL_SCOPE_DECL_MORE,                                  \
                        _CEL_SCOPE_DECL_MORE,                                  \
                        _CEL_SCOPE_DECL_MORE,                                  \
                        _CEL_SCOPE_DECL_MORE,                                  \
                        _CEL_SCOPE_DECL_MORE,                                  \
                        _CEL_SCOPE_DECL_MORE,                                  \
                        _CEL_SCOPE_DECL_1, 0)(__VA_ARGS__)

#define CEL_COMPOSITION_SCOPE_DECL(...) CEL_CompositionScope_Decl(__VA_ARGS__)
#define CEL_Composition_Decl(...) CEL_CompositionScope_Decl(__VA_ARGS__)
#define CEL_COMPOSITION_DECL(...) CEL_CompositionScope_Decl(__VA_ARGS__)

/* --- Composable Function Declaration (CEL_Compose_Decl) --- */

#define _CEL_COMPOSE_DECL_1(name)                                              \
    void name(void)

#define _CEL_COMPOSE_DECL_MORE(name, ...)                                      \
    void name(__VA_ARGS__)

/**
 * @brief Declares an inner composable function prototype in a header (.h) file.
 *
 * Composable components execute within the ambient composer context established
 * by the root composition scope. They do not require an explicit CelsSlotTable*
 * parameter.
 *
 * Usage in header (.h):
 * @code
 *     // With parameter(s):
 *     CEL_Compose_Decl(ComposablePlayerCard, const PlayerState *state);
 *     // Emits: void ComposablePlayerCard(const PlayerState *state);
 *
 *     // Without parameters:
 *     CEL_Compose_Decl(ComposableEmptyView);
 *     // Emits: void ComposableEmptyView(void);
 * @endcode
 */
#define CEL_Compose_Decl(...)                                                  \
    _CEL_DECL_DISPATCH(__VA_ARGS__,                                            \
                        _CEL_COMPOSE_DECL_MORE,                                \
                        _CEL_COMPOSE_DECL_MORE,                                \
                        _CEL_COMPOSE_DECL_MORE,                                \
                        _CEL_COMPOSE_DECL_MORE,                                \
                        _CEL_COMPOSE_DECL_MORE,                                \
                        _CEL_COMPOSE_DECL_MORE,                                \
                        _CEL_COMPOSE_DECL_MORE,                                \
                        _CEL_COMPOSE_DECL_1, 0)(__VA_ARGS__)

#define CEL_COMPOSE_DECL(...) CEL_Compose_Decl(__VA_ARGS__)

/* ========================================================================= */
/* 2. Source Block & State Macros (for .c files)                              */
/* ========================================================================= */

/*
 * --- CEL_Composition ---
 * Root composition view scope (Android Compose setContent equivalent).
 * Establishes ambient context on '{' and finalizes pass on '}'.
 *
 * Usage:
 *     CEL_Composition(KEY) { ... }
 *     CEL_Composition(table, KEY) { ... }
 *     CEL_Composition(cmp, table, KEY) { ... }
 */
/* Inherited directly from composer.h: CEL_Composition, CEL_Scope */

/*
 * --- CEL_Compose ---
 * Child composable node. Supports stateless and Style B stateful skipping.
 *
 * Usage:
 *     CEL_Compose(KEY) { ... }
 *     CEL_Compose(KEY, state) { ... }
 */
/* Inherited directly from composer.h: CEL_Compose */

/*
 * --- cel_watch / CEL_watch ---
 * Parameter diffing reactive guard with automatic O(1) subtree skipping.
 *
 * Usage:
 *     cel_watch(*state) { ... }
 *     CEL_watch(*state) { ... }
 */
/* Inherited directly from composer.h: cel_watch, CEL_watch */

/*
 * --- cel_remember / CEL_remember ---
 * Ephemeral UI state memoized directly in the slot table.
 *
 * Usage:
 *     bool *isHovered = cel_remember(false);
 *     int *counter = cel_remember_type(int, 0);
 */
#define cel_remember(initial_var) CEL_remember(initial_var)
#define cel_remember_type(Type, initial_var) CEL_remember_type(Type, initial_var)

/*
 * --- CEL_query / cel_query ---
 * Reactive ECS query diff guard with automatic O(1) subtree skipping.
 *
 * Flecs Architecture Notes:
 * - Diffs an ECS query match tick or result set against the active slot group.
 * - If unchanged: skips the entire query subtree in O(1) time.
 * - If results changed: enters the block to iterate and compose matching entities.
 *
 * Usage:
 *     CEL_query(enemyQuery) {
 *         // Compose matching entities...
 *     }
 */
/* Inherited directly from composer.h: CEL_query, cel_query, CEL_Query */

/*
 * --- CEL_observable / CEL_Observeable / cel_observable ---
 * Reactive observable diff guard for state and Flecs queries.
 *
 * Supports diffing new data against the slot table cache, optionally exposing
 * the BEFORE-recomposition value:
 * - CEL_observable(obs): Skips in O(1) if clean, recomposes if changed.
 * - CEL_observable(obs, prev_var): Populates prev_var with the data from
 *   BEFORE recomposition, showing what changed!
 *
 * Usage:
 *     CEL_observable(enemyQuery, prevQuery) {
 *         printf("Query changed! Count before: %d, count now: %d\n",
 *                prevQuery.count, enemyQuery.count);
 *     }
 */
/* Inherited directly from composer.h: CEL_observable, CEL_Observable, CEL_Observeable, cel_observable */

/*
 * --- CEL_Dispose / cel_dispose ---
 * Releases and frees the internal slot table associated with a composition root key
 * once it goes out of scope or is destroyed.
 *
 * Usage:
 *     CEL_Dispose(KEY);
 *     cel_dispose(KEY);
 */
/* Inherited directly from composer.h: CEL_Dispose, cel_dispose, CELS_DISPOSE */

/*
 * --- CEL_Name / cel_name (inspired by Nic Barker's Clay UI) ---
 * Generates a stable 32-bit FNV-1a hash key from a unique string name.
 * Replaces hardcoded numbers (e.g. Compose(0x0101) -> Compose(CEL_Name("Unique Name"))).
 *
 * Usage:
 *     CEL_Compose(CEL_Name("HealthBar")) { ... }
 *     CEL_Compose(CEL_Name("InventorySlot", index)) { ... }
 *     CEL_Compose(CEL_NameI("Item", i)) { ... }
 *     CEL_Compose(CEL_NameLocal("CloseButton")) { ... }
 */
/* Inherited directly from composer.h: CEL_Name, cel_name, CEL_NameI, cel_name_i,
                                      CEL_NameLocal, cel_name_local, CEL_NameScoped */

/*
 * --- CEL_Find / CEL_FindGroup / CEL_FindByName ---
 * Locates a composition group across the active table or registry to inspect metadata
 * or remembered slot state.
 *
 * Usage:
 *     CelsCompositionRef ref = CEL_Find(CEL_Name("HealthBar"));
 *     CelsSlotGroup *group = CEL_FindGroup(CEL_Name("HealthBar"));
 *     CelsCompositionRef refByName = CEL_FindByName("HealthBar");
 */
/* Inherited directly from composer.h: CEL_Find, cel_find, CEL_FindGroup,
                                      cel_find_group, CEL_FindByName */

/*
 * --- CEL_Composition ---
 * Root composition view scope (Android Compose setContent equivalent).
 * Establishes ambient context on '{' and finalizes pass on '}'.
 *
 * Usage:
 *     CEL_Composition(KEY) { ... }
 *     CEL_Composition(table, KEY) { ... }
 *     CEL_Composition(cmp, table, KEY) { ... }
 */
/* Inherited directly from composer.h: CEL_Composition, CEL_Scope */

/*
 * --- CEL_Compose ---
 * Child composable node. Supports stateless and Style B stateful skipping.
 *
 * Usage:
 *     CEL_Compose(KEY) { ... }
 *     CEL_Compose(KEY, state) { ... }
 *     CEL_Compose(cmp, KEY, state) { ... }
 */
/* Inherited directly from composer.h: CEL_Compose, CELS_COMPOSE */

/*
 * --- CEL_Watch ---
 * Parameter diffing block. Diffs variable/state against the slot cache.
 * Skips the enclosed block in O(1) time if state is unchanged.
 *
 * Usage:
 *     CEL_Watch(state) { ... }
 *     CEL_Watch(cmp, state) { ... }
 */
/* Inherited directly from composer.h: CEL_Watch, CEL_watch, cel_watch */

/*
 * --- CEL_Query ---
 * Flecs query diff guard. Skips entire query subtree in O(1) time if
 * query match tick has not changed.
 *
 * Usage:
 *     CEL_Query(query) { ... }
 *     CEL_Query(cmp, query) { ... }
 */
/* Inherited directly from composer.h: CEL_Query, cel_query */

/*
 * --- CEL_Remember ---
 * Memoizes ephemeral UI state into the slot table without Flecs entity overhead.
 *
 * Usage:
 *     int *count = CEL_Remember(initialCount);
 *     ScrollState *scroll = CEL_Remember(defaultScroll);
 */
/* Inherited directly from composer.h: CEL_Remember, cel_remember */

/*
 * --- CEL_Component & CEL_RegisterComponent ---
 * Declarative component definition and registration with Flecs.
 *
 * Usage:
 *     CEL_Component(Position) {
 *         float x;
 *         float y;
 *     };
 *
 *     CEL_RegisterComponent(world, Position);
 */
/* Inherited directly from composer.h: CEL_Component, CEL_RegisterComponent */

/*
 * --- CEL_Has & CEL_Tag & CEL_Entity ---
 * Declarative component mutation and tagging on the active cel entity.
 *
 * Usage:
 *     CEL_Compose(CEL_Name("PlayerHud")) {
 *         CEL_Has(Position, { .x = 10.0f, .y = 20.0f });
 *         CEL_Tag(IsPlayer);
 *     }
 */
/* Inherited directly from composer.h: CEL_Has, CEL_Tag, CEL_Entity, cel_entity */

/*
 * --- CEL_CompositionScopeDone & CelsSession ---
 * Root composition view return value and session coordinator.
 *
 * Usage:
 *     CEL_CompositionScope MyRootView(void) {
 *         CEL_Composition(CEL_Name("AppRoot")) {
 *             CEL_Compose(CEL_Name("PlayerHud")) { ... }
 *         }
 *         return CEL_CompositionDone();
 *     }
 *
 *     CelsSession session;
 *     CelsSessionInit(&session, world, &(CelsSessionConfig){
 *         .workerCount = 4,
 *         .compositionScope = MyRootView,
 *         .slabSize = 4096, // 64-byte aligned dual-gap buffer memory (min: 4096)
 *         .maxGroups = 32
 *     });
 *     size_t size = CelsSessionGetSlabSize(&session); // 4096
 *     CEL_CompositionScope scope = CelsSessionGetCompositionScope(&session);
 */
/* Inherited directly from composer.h / recomposition_dispatcher.h */

#endif /* CELS_H */
