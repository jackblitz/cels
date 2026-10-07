#pragma once

/**
 * @file cels.h
 * @brief Public API and Declarative DSL Macros for CELS.
 *
 * CELS (Composition, Evaluation, Lifecycle, State) is a high-performance,
 * cache-aligned declarative composition engine for ISO C99.
 *
 * Core Concepts & Mental Model:
 * - C: Composition
 *      Root compositions (`CEL_Composition`, `cel_attach`), child composable widgets
 *      (`CEL_Composable`), and inline child-aware container blocks (`cel_container`).
 * - E: Evaluation
 *      Fine-grained, dirty-driven recomposition walks (`CelsSessionRecompose`, `CEL_Evaluate`).
 *      Only composables observing mutated state are re-evaluated; untouched subtrees are skipped.
 * - L: Lifecycle
 *      Deterministic, topological mount and unmount tracking (`CEL_Lifecycle`, `cel_lifecycle`).
 *      Tears down native resources in strictly reverse (LIFO) order of registration.
 * - S: State
 *      Double-buffered, cache-aligned reactive state (`CEL_State`, `cel_remember`,
 *      `cel_state`, `cel_remember_state`, `cel_watch`, `cel_get_state`, `cel_mutate`).
 * - Context & Ambient Cascading:
 *      Pass environment variables, theme tokens, and input maps down the composable tree
 *      without parameter drilling (`cel_set_context`, `cel_get_context`, `cel_context`).
 * - Declarative ECS Reconciliation:
 *      Attach and reconcile entity components and tags within composables (`cel_key`,
 *      `cel_has`, `cel_has_tag`, `cel_get`, `cel_is`, `cel_id`).
 * - Custom Layout Primitives:
 *      Engine and library authors author custom container archetypes (like `CEL_Layout`,
 *      `CEL_FlexBox`, `CEL_Grid`) using `cel_container(Name, onStart, onEnd, layoutData)`.
 *
 * Typical Usage Guide:
 * @code
 *     // 1. Declare double-buffered reactive state
 *     CEL_State(CounterState) {
 *         int count;
 *     };
 *
 *     // 2. Declare a child composable widget
 *     CEL_Composable(CounterWidget) {
 *         const CounterState *state = cel_watch_state(CounterState);
 *         int *localClicks = cel_remember(int, 0);
 *
 *         if (cel_button("Increment")) {
 *             cel_mutate(state) {
 *                 this->count++;
 *             }
 *             (*localClicks)++;
 *         }
 *         printf("Count: %d, Clicks: %d\n", state ? state->count : 0, *localClicks);
 *     }
 *
 *     // 3. Declare a root composition
 *     CEL_Composition(AppRoot, void *userData) {
 *         CounterWidget();
 *     }
 *
 *     // 4. Attach to session and run recomposition
 *     CelsSession session;
 *     CelsSessionInit(&session, NULL);
 *     cel_attach(&session, AppRoot);
 *     CelsSessionRecompose(&session);
 *     CelsSessionDestroy(&session);
 * @endcode
 *
 * Thread Safety & Concurrency Invariants:
 * - Single-Threaded Recomposition: Root composition walks and child composable execution
 *   run single-threaded on their assigned host thread. No locks are acquired on hot paths.
 * - Lock-Free Snapshot Reads: Front-buffer state (`cel_watch`, `cel_get_state`) is immutable
 *   and completely lock-free to read across threads.
 * - Cross-Thread Mutations: Staged via double-buffered transaction batches (`CelsSessionSwapTransactionBatches`)
 *   or inter-session signals (`cel_signal`), processed safely at frame boundaries.
 * - Zero Heap Allocations: During active composition walks, all state and groups are allocated
 *   from contiguous slot slabs and nonmoving data arenas. No malloc/free on hot paths.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cels/runtime/slot_table.h"
#include "cels/runtime/state.h"
#include "cels/runtime/session.h"
#include "cels/runtime/log.h"
#include "cels/engine.h"
#include "cels/app.h"
#include "cels/runtime/module.h"
#include "cels/runtime/thread.h"
#include "cels/runtime/task.h"
#include "cels/runtime/transaction.h"
#include "cels/runtime/transition.h"
#include "cels/runtime/event.h"
#include "cels/runtime/version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Macro Helpers for Arity Dispatch                                          */
/* ========================================================================= */

#define _CEL_GET_MACRO_2(_1, _2, NAME, ...) NAME
#define _CEL_GET_MACRO_3(_1, _2, _3, NAME, ...) NAME
#define _CEL_GET_MACRO_4(_1, _2, _3, _4, NAME, ...) NAME
#define _CEL_GET_MACRO_5(_1, _2, _3, _4, _5, NAME, ...) NAME
#define _CEL_GET_MACRO_6(_1, _2, _3, _4, _5, _6, NAME, ...) NAME

#ifndef CELS_UNUSED
    #if defined(__GNUC__) || defined(__clang__)
        #define CELS_UNUSED __attribute__((unused))
    #else
        #define CELS_UNUSED
    #endif
#endif

/* ========================================================================= */
/* Reactive State Declaration (CEL_State)                                    */
/* ========================================================================= */

/**
 * @def CEL_State
 * @brief Declares a double-buffered reactive state structure type.
 *
 * What it does:
 * Generates both a typedef and struct declaration for a named reactive state model.
 * The declared type is used with cel_watch(), cel_get_state(), and cel_mutate().
 *
 * Expected outcome:
 * Defines a clean, cache-aligned C99 struct type recognized by the CELS state registry.
 *
 * Where to use:
 * Declare in shared header files at file scope. Do NOT declare inside functions.
 *
 * Example:
 * @code
 *     CEL_State(PlayerState) {
 *         float posX;
 *         float posY;
 *         int health;
 *         bool isAlive;
 *     };
 * @endcode
 */
#ifndef CEL_State
#define CEL_State(TypeName) \
    typedef struct TypeName TypeName; \
    struct TypeName
#endif

/**
 * @def CEL_Module
 * @brief Declares a host engine subsystem module structure type.
 *
 * What it does:
 * Generates a typedef and struct declaration for persistent engine subsystem modules
 * (e.g. renderer, windowing, audio, physics, ECS worlds) that survive DLL reloads.
 *
 * Expected outcome:
 * Defines a struct type registered with CEL_RegisterModule() and retrieved via CEL_GetModule().
 *
 * Where to use:
 * Declare in header files shared between the host executable and reloadable application modules.
 *
 * Example:
 * @code
 *     CEL_Module(VulkanRenderer) {
 *         VkInstance instance;
 *         VkDevice device;
 *     };
 * @endcode
 */
#ifndef CEL_Module
#define CEL_Module(TypeName) \
    typedef struct TypeName TypeName; \
    struct TypeName
#endif

/* ========================================================================= */
/* Root Composition Declaration (CEL_Composition)                            */
/* ========================================================================= */

#define _CEL_COMPOSITION_1(CompName) \
    void CompName(void *cels_userData CELS_UNUSED)

#define _CEL_COMPOSITION_2(CompName, Arg) \
    void CompName(Arg)

#define _CEL_COMPOSITION_3(CompName, Type, ParamName) \
    void CompName(Type ParamName)

/**
 * @def CEL_Composition
 * @brief Declares the top-level root composition function of an application or window.
 *
 * What it does:
 * Declares a root function signature attached to a CelsSession via cel_attach() or
 * returned from an application manifest's onStart hook. Represents the entry point
 * of a declarative UI, window, or simulation hierarchy.
 *
 * Signatures supported:
 * - CEL_Composition(WindowRoot): Zero-parameter signature. No unused parameter warnings!
 * - CEL_Composition(WindowRoot, AppContext*, ctx): Type-safe injected userData context.
 * - CEL_Composition(WindowRoot, void *userData): Traditional raw pointer signature.
 *
 * Example:
 * @code
 *     // Standard clean composition (state hoisted via cel_state)
 *     CEL_Composition(GameAppRoot) {
 *         TitleBar();
 *         GameViewport();
 *     }
 *
 *     // Composition with injected host/instance userData
 *     CEL_Composition(ViewportView, ViewportConfig*, cfg) {
 *         RenderViewport(cfg->cameraIndex);
 *     }
 * @endcode
 */
#define CEL_Composition(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_COMPOSITION_3, _CEL_COMPOSITION_2, _CEL_COMPOSITION_1)(__VA_ARGS__)

/* ========================================================================= */
/* Composition Evaluation Predicate (CEL_Evaluate)                           */
/* ========================================================================= */

/**
 * @def CEL_Evaluate
 * @brief Evaluates whether a composition branch should execute during recomposition.
 *
 * What it does:
 * Invokes an evaluation predicate function pointer. If Predicate is NULL, defaults to true.
 *
 * Expected outcome:
 * Returns true if the composition branch is active; false if the branch should be
 * pruned and unmounted.
 *
 * Where to use:
 * Used when attaching compositions with cel_attach() or in CelsCompositionRef.
 */
#define CEL_Evaluate(Predicate, ctx) ((Predicate) ? (Predicate)((ctx)) : true)

#define _CEL_EVALUATION_1(Name) \
    bool Name(void *cels_evalCtx CELS_UNUSED)

#define _CEL_EVALUATION_2(Name, ArgDecl) \
    bool Name(ArgDecl)

#define _CEL_EVALUATION_3(Name, ctxType, ctxName) \
    bool Name(ctxType ctxName)

/**
 * @def CEL_Evaluation
 * @brief Declares a lifecycle evaluation predicate function for a composition.
 *
 * Signatures:
 * - CEL_Evaluation(Name): Clean signature without unused context parameter warnings.
 * - CEL_Evaluation(Name, void *ctx): Generic pointer signature.
 * - CEL_Evaluation(Name, Type*, ctx): Type-safe injected context pointer signature.
 *
 * Example:
 * @code
 *     CEL_Evaluation(WindowEval) {
 *         const WindowState *win = cel_get_state(CEL_Window, WindowState);
 *         return win && win->isOpen;
 *     }
 * @endcode
 */
#define CEL_Evaluation(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_EVALUATION_3, _CEL_EVALUATION_2, _CEL_EVALUATION_1)(__VA_ARGS__)

#define CEL_EvaluateFn(...) CEL_Evaluation(__VA_ARGS__)

/* ========================================================================= */
/* Root Composition Attachment (cel_attach)                                  */
/* ========================================================================= */

#define _CEL_ATTACH_2(s, comp) \
    CelsSessionAttachComposition((s), CelsHashKey(#comp), (void(*)(void*))(comp), NULL, NULL, NULL)

#define _CEL_ATTACH_3(s, comp, eval) \
    CelsSessionAttachComposition((s), CelsHashKey(#comp), (void(*)(void*))(comp), NULL, (bool(*)(void*))(eval), NULL)

#define _CEL_ATTACH_4(s, comp, eval, userData) \
    CelsSessionAttachComposition((s), CelsHashKey(#comp), (void(*)(void*))(comp), (void*)(userData), (bool(*)(void*))(eval), NULL)

#define _CEL_ATTACH_5(s, comp, eval, userData, evalCtx) \
    CelsSessionAttachComposition((s), CelsHashKey(#comp), (void(*)(void*))(comp), (void*)(userData), (bool(*)(void*))(eval), (void*)(evalCtx))

/**
 * @def cel_attach
 * @brief Attaches a root composition to a CelsSession.
 *
 * What it does:
 * Registers the composition in the session's attached list with an automatically
 * derived 64-bit key (hashed from #comp), optional lifecycle evaluation predicate,
 * and optional user context pointer.
 *
 * Expected outcome:
 * The composition will be executed on subsequent CelsSessionRecompose() passes whenever
 * its evaluation predicate returns true.
 *
 * Where to use:
 * Call from host initialization, application onStart, or dynamic setup.
 *
 * Overloads:
 * - cel_attach(session, comp)
 * - cel_attach(session, comp, evalPredicate)
 * - cel_attach(session, comp, evalPredicate, userData)
 * - cel_attach(session, comp, evalPredicate, userData, evalCtx)
 *
 * Example:
 * @code
 *     cel_attach(session, WindowComposition, WindowEval);
 *     cel_attach(session, ToolPaletteComposition);
 * @endcode
 */
#define cel_attach(...) \
    _CEL_GET_MACRO_5(__VA_ARGS__, _CEL_ATTACH_5, _CEL_ATTACH_4, _CEL_ATTACH_3, _CEL_ATTACH_2, _UNUSED)(__VA_ARGS__)

/**
 * @def cel_attach_keyed
 * @brief Attaches a root composition with an explicitly provided 64-bit key ID.
 */
#define cel_attach_keyed(s, id, comp, ...) \
    CelsSessionAttachComposition((s), (id), (void(*)(void*))(comp), NULL, NULL, NULL)

/* ========================================================================= */
/* Child Composables (CEL_Composable)                                        */
/* ========================================================================= */

#ifndef CELS_UNUSED
    #if defined(__GNUC__) || defined(__clang__)
        #define CELS_UNUSED __attribute__((unused))
    #else
        #define CELS_UNUSED
    #endif
#endif

#define _CEL_COMPOSABLE_VOID(FnName, ...) \
    static void _cels_body_##FnName(void); \
    static inline void FnName(void) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(sess, CelsHashKey(#FnName))) { \
            _cels_body_##FnName(); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(void)

#define _CEL_COMPOSABLE_1_ARG(FnName, Type1, Arg1) \
    static void _cels_body_##FnName(Type1 Arg1); \
    static inline void FnName(Type1 Arg1) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(sess, CelsHashKey(#FnName))) { \
            _cels_body_##FnName(Arg1); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(Type1 Arg1)

#define _CEL_COMPOSABLE_2_ARGS(FnName, Type1, Arg1, Type2, Arg2) \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2); \
    static inline void FnName(Type1 Arg1, Type2 Arg2) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(sess, CelsHashKey(#FnName))) { \
            _cels_body_##FnName(Arg1, Arg2); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2)

#define _CEL_COMPOSABLE_3_ARGS(FnName, Type1, Arg1, Type2, Arg2, Type3, Arg3) \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3); \
    static inline void FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(sess, CelsHashKey(#FnName))) { \
            _cels_body_##FnName(Arg1, Arg2, Arg3); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3)

#define _CEL_COMPOSABLE_4_ARGS(FnName, Type1, Arg1, Type2, Arg2, Type3, Arg3, Type4, Arg4) \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4); \
    static inline void FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(sess, CelsHashKey(#FnName))) { \
            _cels_body_##FnName(Arg1, Arg2, Arg3, Arg4); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4)

#define _CEL_COMPOSABLE_KEYED_0_ARG(FnName, IdName) \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED); \
    static inline void FnName(uint64_t IdName) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        uint64_t _cels_grp_key = CelsKeyIndex(CelsHashKey(#FnName), (uint64_t)(IdName)); \
        if (CelsEnterComposable(sess, _cels_grp_key)) { \
            CelsSetActiveEntityId(sess, (uint64_t)(IdName)); \
            _cels_body_##FnName(IdName); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED)

#define _CEL_COMPOSABLE_KEYED_1_ARG(FnName, IdName, Type1, Arg1) \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1); \
    static inline void FnName(uint64_t IdName, Type1 Arg1) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        uint64_t _cels_grp_key = CelsKeyIndex(CelsHashKey(#FnName), (uint64_t)(IdName)); \
        if (CelsEnterComposable(sess, _cels_grp_key)) { \
            CelsSetActiveEntityId(sess, (uint64_t)(IdName)); \
            _cels_body_##FnName(IdName, Arg1); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1)

#define _CEL_COMPOSABLE_KEYED_2_ARGS(FnName, IdName, Type1, Arg1, Type2, Arg2) \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2); \
    static inline void FnName(uint64_t IdName, Type1 Arg1, Type2 Arg2) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        uint64_t _cels_grp_key = CelsKeyIndex(CelsHashKey(#FnName), (uint64_t)(IdName)); \
        if (CelsEnterComposable(sess, _cels_grp_key)) { \
            CelsSetActiveEntityId(sess, (uint64_t)(IdName)); \
            _cels_body_##FnName(IdName, Arg1, Arg2); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2)

#define _CEL_COMPOSABLE_KEYED_3_ARGS(FnName, IdName, Type1, Arg1, Type2, Arg2, Type3, Arg3) \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2, Type3 Arg3); \
    static inline void FnName(uint64_t IdName, Type1 Arg1, Type2 Arg2, Type3 Arg3) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        uint64_t _cels_grp_key = CelsKeyIndex(CelsHashKey(#FnName), (uint64_t)(IdName)); \
        if (CelsEnterComposable(sess, _cels_grp_key)) { \
            CelsSetActiveEntityId(sess, (uint64_t)(IdName)); \
            _cels_body_##FnName(IdName, Arg1, Arg2, Arg3); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2, Type3 Arg3)

#define _CEL_COMPOSABLE_KEYED_4_ARGS(FnName, IdName, Type1, Arg1, Type2, Arg2, Type3, Arg3, Type4, Arg4) \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4); \
    static inline void FnName(uint64_t IdName, Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        uint64_t _cels_grp_key = CelsKeyIndex(CelsHashKey(#FnName), (uint64_t)(IdName)); \
        if (CelsEnterComposable(sess, _cels_grp_key)) { \
            CelsSetActiveEntityId(sess, (uint64_t)(IdName)); \
            _cels_body_##FnName(IdName, Arg1, Arg2, Arg3, Arg4); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4)

#define _CEL_GET_COMPOSABLE_MACRO(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, NAME, ...) NAME

/**
 * @def CEL_Composable
 * @brief Declares a reusable, reactive child composable node in the composition hierarchy.
 *
 * What it does:
 * Automatically manages slot table hierarchy navigation for this function. Supports both
 * unkeyed (auto-generated ID) and keyed (explicit 64-bit ID parameter) declarations.
 * When invoked, it keys the composable group in the session's slot gap buffer, evaluates
 * its body if dirty or newly mounted, and automatically calls CelsExitGroup() on exit.
 *
 * Identity & Keying:
 * - Unkeyed forms (e.g. CEL_Composable(StatusBar)): Auto-keys group by function name hash,
 *   auto-generates cel_id() if no parent ID is active, and accepts standard typed arguments.
 * - Keyed forms (e.g. CEL_Composable(TodoItemCard, id, const TodoItem*, item)): First parameter
 *   is `uint64_t id`. Callers invoke `TodoItemCard(item->id, item)`. Binds the node's `cel_id()`
 *   to `id` and allows list reconciliation and component attachment (`cel_has`) directly to `id`.
 *
 * Supported signatures:
 * @code
 *     // Unkeyed 0 arguments:
 *     CEL_Composable(StatusBar) {
 *         // widgets, slots, cel_id() is auto-generated
 *     }
 *
 *     // Keyed 0 extra arguments:
 *     CEL_Composable(PlayerEntity, id) {
 *         cel_has(Position, { .x = 0, .y = 0 });
 *     }
 *
 *     // Keyed with typed arguments:
 *     CEL_Composable(TodoItemCard, id, const TodoItem*, item) {
 *         cel_has(Position, { .x = item->x, .y = item->y });
 *     }
 *
 *     // Calling inside parent composition or composable:
 *     CEL_Composition(MainScreen) {
 *         StatusBar();
 *         PlayerEntity(1001);
 *         for (int i = 0; i < count; i++) {
 *             TodoItemCard(items[i].id, &items[i]);
 *         }
 *     }
 * @endcode
 */
#define CEL_Composable(...) \
    _CEL_GET_COMPOSABLE_MACRO(__VA_ARGS__, \
        _CEL_COMPOSABLE_KEYED_4_ARGS, \
        _CEL_COMPOSABLE_4_ARGS, \
        _CEL_COMPOSABLE_KEYED_3_ARGS, \
        _CEL_COMPOSABLE_3_ARGS, \
        _CEL_COMPOSABLE_KEYED_2_ARGS, \
        _CEL_COMPOSABLE_2_ARGS, \
        _CEL_COMPOSABLE_KEYED_1_ARG, \
        _CEL_COMPOSABLE_1_ARG, \
        _CEL_COMPOSABLE_KEYED_0_ARG, \
        _CEL_COMPOSABLE_VOID, \
        _UNUSED)(__VA_ARGS__)

/* ========================================================================= */
/* Non-inline Composable Definition for Multi-File Translation Units         */
/* ========================================================================= */

/**
 * @def CEL_ComposableDecl
 * @brief (Optional) Declares an external composable prototype in a header file.
 *
 * NOTE: In CELS, composable functions are standard C functions returning void.
 * You can write standard C prototypes directly in headers:
 *     void UserCard(const char *name, int karma);
 *     void TodoItemCard(uint64_t id, const TodoItem *item);
 * CEL_ComposableDecl is provided as an optional convenience macro.
 */
#define CEL_ComposableDecl(FnName, ...) void FnName(__VA_ARGS__)

#define _CEL_COMPOSABLE_DEF_VOID(FnName, ...) \
    static void _cels_body_##FnName(void); \
    void FnName(void) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(sess, CelsHashKey(#FnName))) { \
            _cels_body_##FnName(); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(void)

#define _CEL_COMPOSABLE_DEF_1_ARG(FnName, Type1, Arg1) \
    static void _cels_body_##FnName(Type1 Arg1); \
    void FnName(Type1 Arg1) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(sess, CelsHashKey(#FnName))) { \
            _cels_body_##FnName(Arg1); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(Type1 Arg1)

#define _CEL_COMPOSABLE_DEF_2_ARGS(FnName, Type1, Arg1, Type2, Arg2) \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2); \
    void FnName(Type1 Arg1, Type2 Arg2) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(sess, CelsHashKey(#FnName))) { \
            _cels_body_##FnName(Arg1, Arg2); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2)

#define _CEL_COMPOSABLE_DEF_3_ARGS(FnName, Type1, Arg1, Type2, Arg2, Type3, Arg3) \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3); \
    void FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(sess, CelsHashKey(#FnName))) { \
            _cels_body_##FnName(Arg1, Arg2, Arg3); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3)

#define _CEL_COMPOSABLE_DEF_4_ARGS(FnName, Type1, Arg1, Type2, Arg2, Type3, Arg3, Type4, Arg4) \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4); \
    void FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(sess, CelsHashKey(#FnName))) { \
            _cels_body_##FnName(Arg1, Arg2, Arg3, Arg4); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4)

#define _CEL_COMPOSABLE_DEF_KEYED_0_ARG(FnName, IdName) \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED); \
    void FnName(uint64_t IdName) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        uint64_t _cels_grp_key = CelsKeyIndex(CelsHashKey(#FnName), (uint64_t)(IdName)); \
        if (CelsEnterComposable(sess, _cels_grp_key)) { \
            CelsSetActiveEntityId(sess, (uint64_t)(IdName)); \
            _cels_body_##FnName(IdName); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED)

#define _CEL_COMPOSABLE_DEF_KEYED_1_ARG(FnName, IdName, Type1, Arg1) \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1); \
    void FnName(uint64_t IdName, Type1 Arg1) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        uint64_t _cels_grp_key = CelsKeyIndex(CelsHashKey(#FnName), (uint64_t)(IdName)); \
        if (CelsEnterComposable(sess, _cels_grp_key)) { \
            CelsSetActiveEntityId(sess, (uint64_t)(IdName)); \
            _cels_body_##FnName(IdName, Arg1); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1)

#define _CEL_COMPOSABLE_DEF_KEYED_2_ARGS(FnName, IdName, Type1, Arg1, Type2, Arg2) \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2); \
    void FnName(uint64_t IdName, Type1 Arg1, Type2 Arg2) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        uint64_t _cels_grp_key = CelsKeyIndex(CelsHashKey(#FnName), (uint64_t)(IdName)); \
        if (CelsEnterComposable(sess, _cels_grp_key)) { \
            CelsSetActiveEntityId(sess, (uint64_t)(IdName)); \
            _cels_body_##FnName(IdName, Arg1, Arg2); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2)

#define _CEL_COMPOSABLE_DEF_KEYED_3_ARGS(FnName, IdName, Type1, Arg1, Type2, Arg2, Type3, Arg3) \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2, Type3 Arg3); \
    void FnName(uint64_t IdName, Type1 Arg1, Type2 Arg2, Type3 Arg3) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        uint64_t _cels_grp_key = CelsKeyIndex(CelsHashKey(#FnName), (uint64_t)(IdName)); \
        if (CelsEnterComposable(sess, _cels_grp_key)) { \
            CelsSetActiveEntityId(sess, (uint64_t)(IdName)); \
            _cels_body_##FnName(IdName, Arg1, Arg2, Arg3); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2, Type3 Arg3)

#define _CEL_COMPOSABLE_DEF_KEYED_4_ARGS(FnName, IdName, Type1, Arg1, Type2, Arg2, Type3, Arg3, Type4, Arg4) \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4); \
    void FnName(uint64_t IdName, Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4) { \
        CelsSession *sess = CelsGetCurrentSession(); \
        assert(sess != NULL && #FnName " called outside of an active CelsSession"); \
        uint64_t _cels_grp_key = CelsKeyIndex(CelsHashKey(#FnName), (uint64_t)(IdName)); \
        if (CelsEnterComposable(sess, _cels_grp_key)) { \
            CelsSetActiveEntityId(sess, (uint64_t)(IdName)); \
            _cels_body_##FnName(IdName, Arg1, Arg2, Arg3, Arg4); \
        } \
        CelsExitGroup(sess); \
    } \
    static void _cels_body_##FnName(uint64_t IdName CELS_UNUSED, Type1 Arg1, Type2 Arg2, Type3 Arg3, Type4 Arg4)

/**
 * @def CEL_ComposableDef
 * @brief Implements an externally linkable composable function in a .c source file.
 *
 * What it does:
 * Generates an externally visible (non-static) C function `void FnName(...)` that
 * wraps the body with CelsEnterComposable() and CelsExitGroup(). Supports both unkeyed
 * and keyed signatures matching CEL_Composable.
 */
#define CEL_ComposableDef(...) \
    _CEL_GET_COMPOSABLE_MACRO(__VA_ARGS__, \
        _CEL_COMPOSABLE_DEF_KEYED_4_ARGS, \
        _CEL_COMPOSABLE_DEF_4_ARGS, \
        _CEL_COMPOSABLE_DEF_KEYED_3_ARGS, \
        _CEL_COMPOSABLE_DEF_3_ARGS, \
        _CEL_COMPOSABLE_DEF_KEYED_2_ARGS, \
        _CEL_COMPOSABLE_DEF_2_ARGS, \
        _CEL_COMPOSABLE_DEF_KEYED_1_ARG, \
        _CEL_COMPOSABLE_DEF_1_ARG, \
        _CEL_COMPOSABLE_DEF_KEYED_0_ARG, \
        _CEL_COMPOSABLE_DEF_VOID, \
        _UNUSED)(__VA_ARGS__)

/* ========================================================================= */
/* Pure Lifecycles (CEL_Lifecycle & cel_lifecycle)                           */
/* ========================================================================= */

#define CEL_LIFECYCLE_PHASE_MOUNT   1
#define CEL_LIFECYCLE_PHASE_UNMOUNT 2

/**
 * @def mount
 * @brief Encloses code executed exclusively during node mounting.
 *
 * What it does:
 * Evaluates the enclosed block only during the CEL_LIFECYCLE_PHASE_MOUNT phase, which
 * occurs exactly once when the parent composable is first entered into the session tree.
 *
 * Where to use:
 * Inside a CEL_Lifecycle declaration body.
 */
#define mount   if (_cels_lifecycle_phase == CEL_LIFECYCLE_PHASE_MOUNT)

/**
 * @def unmount
 * @brief Encloses code executed exclusively during node unmounting/destruction.
 *
 * What it does:
 * Evaluates the enclosed block only during the CEL_LIFECYCLE_PHASE_UNMOUNT phase, which
 * occurs when the parent composable is pruned, excluded, or the session is destroyed.
 *
 * Where to use:
 * Inside a CEL_Lifecycle declaration body.
 */
#define unmount if (_cels_lifecycle_phase == CEL_LIFECYCLE_PHASE_UNMOUNT)

/**
 * @def CEL_Lifecycle
 * @brief Declares a reusable, parameterized lifecycle controller with mount and unmount blocks.
 *
 * What it does:
 * Defines a named lifecycle handler that pairs resource acquisition on mount with
 * guaranteed release on unmount. Unmount destructors are executed in reverse order of mounting.
 *
 * Expected outcome:
 * Manages external non-declarative resources (file handles, audio streams, socket connections,
 * native window hooks) tied directly to a composable's lifetime in the slot table.
 *
 * Where to use:
 * Declare at file/global scope. Attach within a composable using cel_lifecycle().
 *
 * Example:
 * @code
 *     CEL_Lifecycle(NetworkStreamLifecycle, SocketHandle *socket) {
 *         mount {
 *             SocketConnect(socket, "api.example.com", 443);
 *         }
 *         unmount {
 *             SocketDisconnect(socket);
 *         }
 *     }
 *
 *     CEL_Composable(NetworkWidget, SocketHandle *socket) {
 *         cel_lifecycle(NetworkStreamLifecycle, socket);
 *         // widget UI logic
 *     }
 * @endcode
 */
#define _CEL_LIFECYCLE_1(Name) \
    static void _cels_lifecycle_impl_##Name(int _cels_lifecycle_phase); \
    static void _cels_lifecycle_clean_##Name(void *instance, CelsSession *session) { \
        (void)instance; (void)session; \
        _cels_lifecycle_impl_##Name(CEL_LIFECYCLE_PHASE_UNMOUNT); \
    } \
    static inline void _cels_lifecycle_attach_##Name(CelsSession *session, void *param) { \
        (void)param; \
        if (session == NULL || session->currentDepth == 0) return; \
        if (CelsIsFreshMount(session)) { \
            _cels_lifecycle_impl_##Name(CEL_LIFECYCLE_PHASE_MOUNT); \
            CelsSessionRegisterLifecycle(session, (void*)1, NULL, _cels_lifecycle_clean_##Name); \
        } else { \
            CelsSessionUpdateLifecycle(session, (void*)1, _cels_lifecycle_clean_##Name); \
        } \
    } \
    static void _cels_lifecycle_impl_##Name(int _cels_lifecycle_phase)

#define _CEL_LIFECYCLE_2(Name, ParamDecl) \
    static void _cels_lifecycle_impl_##Name(int _cels_lifecycle_phase, ParamDecl); \
    static void _cels_lifecycle_clean_##Name(void *instance, CelsSession *session) { \
        (void)session; \
        _cels_lifecycle_impl_##Name(CEL_LIFECYCLE_PHASE_UNMOUNT, instance); \
    } \
    static inline void _cels_lifecycle_attach_##Name(CelsSession *session, void *param) { \
        if (session == NULL || session->currentDepth == 0) return; \
        if (CelsIsFreshMount(session)) { \
            _cels_lifecycle_impl_##Name(CEL_LIFECYCLE_PHASE_MOUNT, param); \
            CelsSessionRegisterLifecycle(session, param, NULL, _cels_lifecycle_clean_##Name); \
        } else { \
            CelsSessionUpdateLifecycle(session, param, _cels_lifecycle_clean_##Name); \
        } \
    } \
    static void _cels_lifecycle_impl_##Name(int _cels_lifecycle_phase, ParamDecl)

#define _CEL_LIFECYCLE_3(Name, ParamType, ParamName) \
    static void _cels_lifecycle_impl_##Name(int _cels_lifecycle_phase, ParamType ParamName); \
    static void _cels_lifecycle_clean_##Name(void *instance, CelsSession *session) { \
        (void)session; \
        _cels_lifecycle_impl_##Name(CEL_LIFECYCLE_PHASE_UNMOUNT, (ParamType)(uintptr_t)instance); \
    } \
    static inline void _cels_lifecycle_attach_##Name(CelsSession *session, ParamType param) { \
        if (session == NULL || session->currentDepth == 0) return; \
        if (CelsIsFreshMount(session)) { \
            _cels_lifecycle_impl_##Name(CEL_LIFECYCLE_PHASE_MOUNT, param); \
            CelsSessionRegisterLifecycle(session, (void*)(uintptr_t)param, NULL, _cels_lifecycle_clean_##Name); \
        } else { \
            CelsSessionUpdateLifecycle(session, (void*)(uintptr_t)param, _cels_lifecycle_clean_##Name); \
        } \
    } \
    static void _cels_lifecycle_impl_##Name(int _cels_lifecycle_phase, ParamType ParamName)

#define CEL_Lifecycle(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_LIFECYCLE_3, _CEL_LIFECYCLE_2, _CEL_LIFECYCLE_1)(__VA_ARGS__)

#define _CEL_LIFECYCLE_INVOKE_2(Name, handle) \
    _cels_lifecycle_attach_##Name(CelsGetCurrentSession(), (handle))

#define _CEL_LIFECYCLE_INVOKE_1(Name) \
    _cels_lifecycle_attach_##Name(CelsGetCurrentSession(), NULL)

/**
 * @def cel_lifecycle
 * @brief Attaches an instance of a CEL_Lifecycle controller to the current composable.
 *
 * What it does:
 * Invokes the lifecycle controller's internal attachment function for the active session.
 * If the current composable node is being mounted for the first time (fresh mount), the
 * lifecycle's mount block is executed immediately, and its unmount callback is registered
 * in the session's lifecycle registry. On subsequent recomposition passes, the registration
 * is refreshed. When the node leaves the hierarchy, the unmount block executes.
 *
 * Expected outcome:
 * Binds external resource acquisition and teardown directly to the enclosing composable's
 * presence in the composition tree.
 *
 * Where to use:
 * Call inside a CEL_Composable body. Can be passed 0 or 1 parameter matching the
 * corresponding CEL_Lifecycle declaration.
 *
 * Overloads:
 * - cel_lifecycle(LifecycleName)
 * - cel_lifecycle(LifecycleName, argument)
 *
 * Example:
 * @code
 *     CEL_Composable(AudioPlayer, const char *trackPath) {
 *         cel_lifecycle(TrackPlaybackLifecycle, trackPath);
 *         // player UI
 *     }
 * @endcode
 */
#define cel_lifecycle(...) \
    _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_LIFECYCLE_INVOKE_2, _CEL_LIFECYCLE_INVOKE_1)(__VA_ARGS__)

#define CEL_Attach cel_attach
#define CEL_GetState cel_get_state
#define CEL_None NULL

/* ========================================================================= */
/* Addressable Persistent State (cel_remember_state)                         */
/* ========================================================================= */

/**
 * @def cel_remember_state
 * @brief Allocates or resolves globally addressable persistent state in the ambient session.
 *
 * What it does:
 * Looks up a persistent state cell identified by 64-bit `id` within the session's
 * state slab. If found, returns a pointer to the existing state. If not found, allocates
 * contiguous memory for `Type` and copies default values into it.
 *
 * Expected outcome:
 * Guarantees a stable, nonmoving pointer to state that survives across recompositions,
 * composable unmounts, and hot reload cycles.
 *
 * Where to use:
 * Call from composables, root compositions, or module initialization to store shared
 * application state that is identified by a known key (e.g. CEL_ID("GameState")).
 *
 * Example:
 * @code
 *     NetworkState *net = cel_remember_state(CEL_NetworkState, NetworkState, {
 *         .status = NET_DISCONNECTED,
 *         .pingMs = 0
 *     });
 * @endcode
 */
#define cel_remember_state(Type, ...) \
    ((Type*)CelsSessionRememberState(CelsGetCurrentSession(), CelsHashKey(#Type), sizeof(Type), (const Type[]){ __VA_ARGS__ }))

#define cel_remember_state_keyed(id, Type, ...) \
    ((Type*)CelsSessionRememberState(CelsGetCurrentSession(), (id), sizeof(Type), (const Type[]){ __VA_ARGS__ }))


/* ========================================================================= */
/* State Hoisting (cel_state)                                                */
/* ========================================================================= */

#define _CEL_STATE_INSTANCE_1(Type) \
    ((Type*)CelsResolveStateInstance(CelsGetCurrentSession(), sizeof(Type), NULL))

#define _CEL_STATE_INSTANCE_2(Type, Init) \
    ((Type*)CelsResolveStateInstance(CelsGetCurrentSession(), sizeof(Type), (const Type[]){ Init }))

#define _CEL_GET_STATE_INSTANCE_MACRO(_1, _2, NAME, ...) NAME

/**
 * @def cel_state
 * @brief Allocates and hoists a reactive state instance pinned to this composable's slot.
 *
 * What it does:
 * Allocates a persistent double-buffered reactive state instance whose identity is derived
 * automatically from its slot table position (no string keys or manual IDs required).
 * Preceded by a CelsStateHeader, the returned pointer can be passed down to child composables,
 * subscribed to with cel_watch(ptr), and mutated anywhere with cel_mutate(ptr).
 * When the enclosing composable leaves the active hierarchy, the state cell is automatically
 * cleaned up and deactivated.
 *
 * Overloads:
 * - cel_state(Type)
 * - cel_state(Type, { .field = val, ... })
 *
 * Example:
 * @code
 *     WindowState *win = cel_state(WindowState, {
 *         .width = 800,
 *         .height = 600,
 *         .isOpen = true
 *     });
 *     WindowContent(win);
 * @endcode
 */
#define cel_state(...) \
    _CEL_GET_STATE_INSTANCE_MACRO(__VA_ARGS__, _CEL_STATE_INSTANCE_2, _CEL_STATE_INSTANCE_1)(__VA_ARGS__)

/* ========================================================================= */
/* Private Local Memory (cel_remember)                                       */
/* ========================================================================= */

#define _CEL_REMEMBER_2(Type, Init) \
    ((Type*)CelsResolveSlotWithCleanup(CelsGetCurrentSession(), sizeof(Type), (const Type[]){ Init }, NULL))

#define _CEL_REMEMBER_3(Type, Init, OnDestroy) \
    ((Type*)CelsResolveSlotWithCleanup(CelsGetCurrentSession(), sizeof(Type), (const Type[]){ Init }, (void (*)(void*, CelsSession*))(OnDestroy)))

#define _CEL_GET_REMEMBER_MACRO(_1, _2, _3, NAME, ...) NAME

/**
 * @def cel_remember
 * @brief Allocates or resolves position-dependent private slot memory in the active session.
 *
 * What it does:
 * Allocates a contiguous slot in the session's slot gap buffer aligned with the current
 * composable's execution order. On the first pass (fresh mount), initializes the slot
 * with `Init` and binds the optional `OnDestroy` cleanup callback. On subsequent passes,
 * returns the existing slot pointer without reinitializing. When the enclosing composable
 * is pruned from the hierarchy, `OnDestroy` is automatically invoked.
 *
 * Expected outcome:
 * Preserves local composable state (e.g. counters, toggle flags, scratch buffers, handles)
 * across recomposition passes without needing global keys or manual free calls.
 *
 * Where to use:
 * Call exclusively inside a CEL_Composable or CEL_Composition body during an active recomposition.
 *
 * Overloads:
 * - cel_remember(Type, InitValue)
 * - cel_remember(Type, InitValue, OnDestroyCallback)
 *
 * Examples:
 * @code
 *     // Primitive counter:
 *     int *clickCount = cel_remember(int, 0);
 *     (*clickCount)++;
 *
 *     // Struct with custom unmount destructor:
 *     void CleanupResource(void *instance, CelsSession *session) {
 *         NativeResource *res = (NativeResource*)instance;
 *         NativeDestroy(res);
 *     }
 *     NativeResource *res = cel_remember(NativeResource, InitResource(), CleanupResource);
 * @endcode
 */
#define cel_remember(...) \
    _CEL_GET_REMEMBER_MACRO(__VA_ARGS__, _CEL_REMEMBER_3, _CEL_REMEMBER_2)(__VA_ARGS__)

/* ========================================================================= */
/* Reactive Observation & State Reading (cel_watch & cel_get_state)          */
/* ========================================================================= */

#define _CEL_WATCH_1(ptr) \
    CelsWatchStateInstance(CelsGetCurrentSession(), (ptr))

#define _CEL_WATCH_2(Type, id) \
    ((const Type*)CelsStateWatch(CelsGetCurrentSession(), (id), sizeof(Type)))

#define _CEL_WATCH_3(session, Type, id) \
    ((const Type*)CelsStateWatch((session), (id), sizeof(Type)))

/**
 * @def cel_watch
 * @brief Reads reactive state and registers an automatic recomposition dependency.
 *
 * What it does:
 * Looks up the front-buffer snapshot of a reactive state cell and registers the calling
 * composable's group key as an observer of that cell in the session dependency graph.
 * Supports hoisted state instance pointers (zero string keys) and legacy keyed lookups.
 *
 * Overloads:
 * - cel_watch(instancePtr): Subscribes to a hoisted state instance pointer.
 * - cel_watch(Type, id): Looks up by explicit 64-bit key.
 * - cel_watch(session, Type, id): Operates on an explicit session pointer.
 *
 * Examples:
 * @code
 *     // Hoisted state instance:
 *     cel_watch(win);
 *
 *     // Keyed entity state watch:
 *     const PlayerState *player = cel_watch(PlayerState, playerId);
 * @endcode
 */
#define cel_watch(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_WATCH_3, _CEL_WATCH_2, _CEL_WATCH_1)(__VA_ARGS__)

#define cel_watch_state(Type) \
    ((const Type*)CelsStateWatch(CelsGetCurrentSession(), CelsHashKey(#Type), sizeof(Type)))

#define _CEL_GET_STATE_1(Type) \
    ((const Type*)CelsStateGet(CelsGetCurrentSession(), CelsHashKey(#Type), sizeof(Type)))

#define _CEL_GET_STATE_2(session, Type) \
    ((const Type*)CelsStateGet((session), CelsHashKey(#Type), sizeof(Type)))

#define _CEL_GET_STATE_KEYED_2(id, Type) \
    ((const Type*)CelsStateGet(CelsGetCurrentSession(), (id), sizeof(Type)))

#define _CEL_GET_STATE_KEYED_3(session, id, Type) \
    ((const Type*)CelsStateGet((session), (id), sizeof(Type)))

#define cel_get_state_keyed(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_GET_STATE_KEYED_3, _CEL_GET_STATE_KEYED_2, _CEL_GET_STATE_KEYED_2)(__VA_ARGS__)

/**
 * @def cel_get_state
 * @brief Reads reactive state snapshot without registering a recomposition dependency.
 *
 * What it does:
 * Returns a read-only pointer to the front-buffer of a reactive state cell without
 * recording the calling composable in the observer registry.
 *
 * Overloads:
 * - cel_get_state(Type): Looks up in ambient session by auto-hashed Type name.
 * - cel_get_state(session, Type): Looks up in explicit session by auto-hashed Type name.
 * - cel_get_state(session, id, Type): Looks up by explicit 64-bit ID.
 */
#define cel_get_state(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_GET_STATE_3, _CEL_GET_STATE_2, _CEL_GET_STATE_1)(__VA_ARGS__)

/* ========================================================================= */
/* State Mutation (cel_mutate)                                               */
/* Hoisted syntax: cel_mutate(ptr) { this->field = val; }                    */
/* ========================================================================= */

#if defined(__GNUC__) || defined(__clang__) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
    #define _CEL_MUTATE_TYPE(ptr) __typeof__(*(ptr))
#elif defined(_MSC_VER) && _MSC_VER >= 1930
    #define _CEL_MUTATE_TYPE(ptr) typeof(*(ptr))
#else
    #define _CEL_MUTATE_TYPE(ptr) void
#endif

/**
 * @def cel_mutate
 * @brief Modifies a reactive state instance and invalidates all observing composables.
 *
 * What it does:
 * Accesses the double-buffered back-buffer of the specified state instance pointer
 * (allocated via cel_state or cel_remember_state) and marks all composable groups
 * that observed this cell via cel_watch() as dirty in the owning session's invalidation queue.
 *
 * Architectural Rule (Actor Model):
 * Sessions only mutate their own state via cel_mutate(ptr).
 * To communicate changes from outside the session (e.g. host loop, other sessions,
 * background worker threads), send a discrete signal via cel_signal(&targetSession, SignalType, ...),
 * and let the target session handle the signal and mutate its own state internally via cel_connect().
 *
 * Scoped block mutation (`this` pointer available inside block):
 * @code
 *     cel_mutate(win) {
 *         this->isOpen = false;
 *     }
 * @endcode
 */
#define cel_mutate(ptr) \
    for (_CEL_MUTATE_TYPE(ptr) *this = (_CEL_MUTATE_TYPE(ptr)*)CelsMutateStateInstance((void*)(ptr)); \
         this != NULL; \
         this = NULL)

/* ========================================================================= */
/* Tier 3 Low-Level / Internal Plumbing: Direct Session Mutation             */
/* Used for raw test fixtures and engine internals.                          */
/* ========================================================================= */

#define _CELS_SESSION_MUTATE_2(session, Type) \
    for (Type *this = (Type*)CelsStateMutate((session), CelsHashKey(#Type), sizeof(Type)); \
         this != NULL; \
         this = NULL)

#define _CELS_SESSION_MUTATE_3(session, id, Type) \
    for (Type *this = (Type*)CelsStateMutate((session), (id), sizeof(Type)); \
         this != NULL; \
         this = NULL)

#define _CELS_GET_SESSION_MUTATE_MACRO(_1, _2, _3, NAME, ...) NAME

/**
 * @def cels_session_mutate
 * @brief Tier 3 Low-Level Plumbing: Mutates a state cell directly by session pointer and key.
 *
 * NOTE: For standard application and composable development, use cel_mutate(ptr).
 * Sessions only mutate their own state. For cross-session or external communication,
 * use cel_signal(&session, Type, ...).
 */
#define cels_session_mutate(...) \
    _CELS_GET_SESSION_MUTATE_MACRO(__VA_ARGS__, _CELS_SESSION_MUTATE_3, _CELS_SESSION_MUTATE_2)(__VA_ARGS__)

#define cels_state_mutate_ptr(session, id, Type) \
    ((Type*)CelsStateMutate((session), (id), sizeof(Type)))

/* ========================================================================= */
/* Engine Loop Termination (cel_quit)                                        */
/* ========================================================================= */

/**
 * @def cel_engine_quit
 * @brief Signals the active CelsEngine to cleanly terminate its frame loop.
 *
 * What it does:
 * Invokes CelsEngineQuit(NULL) on the active ambient engine instance.
 *
 * Expected outcome:
 * CelsEngineIsRunning() will return false at the next frame boundary, causing the
 * host loop to exit cleanly and begin orderly shutdown.
 *
 * Where to use:
 * Call from quit buttons, window close callbacks, or exit shortcuts (e.g. Esc).
 */
#define cel_engine_quit() CelsEngineQuit(NULL)

/**
 * @def cel_quit
 * @brief Alias for cel_engine_quit(). Signals the host engine to terminate.
 */
#define cel_quit()        CelsEngineQuit(NULL)

/* ========================================================================= */
/* Session Creation & Discovery (cel_create_session, cel_get_session)        */
/* ========================================================================= */

/**
 * @def cel_create_session
 * @brief Creates and registers a named secondary session managed by the host engine.
 *
 * Example:
 * @code
 *     CelsSession *audio = cel_create_session(&engine, "audio", CELS_PROFILE_256);
 *     cel_attach(audio, AudioDSPComposition);
 * @endcode
 */
#define cel_create_session(engine, name, profile) \
    CelsEngineCreateSession((engine), (name), (profile))

/**
 * @def cel_get_session
 * @brief Retrieves a named session managed by the host engine.
 *
 * Passing "main", "root", or NULL retrieves the engine's primary session.
 *
 * Example:
 * @code
 *     CelsSession *main = cel_get_session(&engine, "main");
 *     cel_signal(main, WindowActionSignal, { .action = WINDOW_ACTION_CLOSE });
 * @endcode
 */
#define cel_get_session(engine, name) \
    CelsEngineGetSession((engine), (name))

/* ========================================================================= */
/* Discrete Events, Signals & Global Broadcasts                             */
/* ========================================================================= */

/**
 * @def cel_event
 * @brief Emits a local event that bubbles up the composable tree to ancestors.
 *
 * What it does:
 * Stages a discrete event record in the active session's event queue and invalidates
 * any ancestor composable groups currently listening for Type via cel_listen().
 *
 * Example:
 * @code
 *     cel_event(ButtonClicked, { .buttonId = BTN_CONFIRM });
 * @endcode
 */
#define cel_event(Type, ...) \
    CelsEventEmit(CelsGetCurrentSession(), CelsHashKey(#Type), (const Type[]){ __VA_ARGS__ }, sizeof(Type))

/**
 * @def cel_listen
 * @brief Binds an inline handler loop for local tree events bubbling from descendants.
 *
 * What it does:
 * Registers the calling composable as a listener for Type and iterates over any
 * unconsumed events of that type emitted in the active tree.
 *
 * Example:
 * @code
 *     cel_listen(ButtonClicked, ev) {
 *         printf("Button %d was clicked!\n", ev->buttonId);
 *     }
 * @endcode
 */
#define _CEL_LISTEN_2(Type, var) \
    for (CELS_UNUSED const Type *var = (const Type*)CelsEventPoll(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_LOCAL); \
         ((void)var, var != NULL); \
         var = (const Type*)CelsEventNext(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_LOCAL))

#define _CEL_LISTEN_1(Type) \
    for (CELS_UNUSED const Type *_cels_ev = (const Type*)CelsEventPoll(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_LOCAL); \
         ((void)_cels_ev, _cels_ev != NULL); \
         _cels_ev = (const Type*)CelsEventNext(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_LOCAL))

#define cel_listen(...) \
    _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_LISTEN_2, _CEL_LISTEN_1)(__VA_ARGS__)

/**
 * @def cel_signal
 * @brief Sends a targeted signal directly into another session's inbox.
 *
 * What it does:
 * Stages a discrete signal payload directly in targetSession's event queue and marks
 * that session for recomposition so its cel_connect() handlers process the signal.
 *
 * Example:
 * @code
 *     cel_signal(&hudSession, PlayerHealed, { .amount = 25 });
 * @endcode
 */
#define cel_signal(targetSession, Type, ...) \
    CelsEventSignal((targetSession), CelsHashKey(#Type), (const Type[]){ __VA_ARGS__ }, sizeof(Type))

#define _CEL_CONNECT_2(Type, var) \
    for (CELS_UNUSED const Type *var = (const Type*)CelsEventPoll(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_SIGNAL); \
         ((void)var, var != NULL); \
         var = (const Type*)CelsEventNext(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_SIGNAL))

#define _CEL_CONNECT_1(Type) \
    for (CELS_UNUSED const Type *_cels_sig = (const Type*)CelsEventPoll(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_SIGNAL); \
         ((void)_cels_sig, _cels_sig != NULL); \
         _cels_sig = (const Type*)CelsEventNext(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_SIGNAL))

/**
 * @def cel_connect
 * @brief Binds an inline handler loop for targeted signals sent directly to this session.
 *
 * What it does:
 * Iterates over any unconsumed targeted signals of Type received by this session.
 * Supports both 1-argument (trigger-only) and 2-argument (payload-binding) forms.
 *
 * Overloads:
 * - cel_connect(Type): Iterates over signals without binding a payload variable.
 * - cel_connect(Type, var): Binds the signal payload to var for inspection.
 *
 * Examples:
 * @code
 *     // 1-argument trigger:
 *     cel_connect(StartWorkflowSignal) {
 *         TriggerWorkflow();
 *     }
 *
 *     // 2-argument payload binding:
 *     cel_connect(PlayerHealed, sig) {
 *         SpawnFloatingNumbers(sig->amount);
 *     }
 * @endcode
 */
#define cel_connect(...) \
    _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_CONNECT_2, _CEL_CONNECT_1)(__VA_ARGS__)

/**
 * @def cel_broadcast
 * @brief Publishes a global broadcast across the engine, worker threads, and sessions.
 *
 * What it does:
 * Enqueues a broadcast payload into the engine's thread-safe global queue. The payload
 * is distributed to all active sessions at frame boundaries and processed by cel_bind().
 *
 * Example:
 * @code
 *     cel_broadcast(AudioTrigger, { .sound = "click.wav", .gain = 1.0f });
 * @endcode
 */
#define cel_broadcast(Type, ...) \
    CelsEventBroadcast(CelsGetCurrentSession(), CelsHashKey(#Type), (const Type[]){ __VA_ARGS__ }, sizeof(Type))

#define _CEL_BIND_2(Type, var) \
    for (CELS_UNUSED const Type *var = (const Type*)CelsEventPoll(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_BROADCAST); \
         ((void)var, var != NULL); \
         var = (const Type*)CelsEventNext(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_BROADCAST))

#define _CEL_BIND_1(Type) \
    for (CELS_UNUSED const Type *_cels_bcast = (const Type*)CelsEventPoll(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_BROADCAST); \
         ((void)_cels_bcast, _cels_bcast != NULL); \
         _cels_bcast = (const Type*)CelsEventNext(CelsGetCurrentSession(), CelsHashKey(#Type), CELS_EVENT_SCOPE_BROADCAST))

/**
 * @def cel_bind
 * @brief Binds an inline handler loop for global engine broadcasts.
 *
 * What it does:
 * Iterates over any unconsumed broadcasts of Type delivered to the calling session.
 * Supports both 1-argument (trigger-only) and 2-argument (payload-binding) forms.
 *
 * Overloads:
 * - cel_bind(Type): Iterates over broadcasts without binding a payload variable.
 * - cel_bind(Type, var): Binds the broadcast payload to var for inspection.
 *
 * Examples:
 * @code
 *     // 1-argument trigger:
 *     cel_bind(SaveBroadcast) {
 *         SaveGame();
 *     }
 *
 *     // 2-argument payload binding:
 *     cel_bind(AudioTrigger, bcast) {
 *         PlaySound(bcast->sound, bcast->gain);
 *     }
 * @endcode
 */
#define cel_bind(...) \
    _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_BIND_2, _CEL_BIND_1)(__VA_ARGS__)

/* ========================================================================= */
/* Procedural Tasks & Coroutines (CEL_Task)                                  */
/* ========================================================================= */

/**
 * @def cancel
 * @brief Defines the cancellation and unmount teardown block of a CEL_Task.
 *
 * What it does:
 * Designates a scoped block containing cleanup logic that executes whenever the task
 * is explicitly cancelled via cel_cancel() or cel_cancel_task(), or when the task is
 * unmounted because its parent composable was pruned from the session hierarchy.
 *
 * Expected outcome:
 * Guaranteed execution of resource cleanup (closing sockets, freeing temporary buffers,
 * resetting hardware/render states) before the task's slot memory is reclaimed.
 *
 * Where to use:
 * Place inside a CEL_Task definition, typically before or after the run block.
 *
 * Example:
 * @code
 *     CEL_Task(DownloadTask, const char *url) {
 *         cancel {
 *             AbortHttpRequest();
 *             CELS_LOG_INFO("Download aborted for %s", url);
 *         }
 *         run {
 *             // download steps
 *         }
 *     }
 * @endcode
 */
#ifndef cancel
#define cancel \
    _cels_task_cancel_entry: \
    if (_cels_task_phase == CEL_TASK_PHASE_CANCEL) \
        for (int _cels_canc_once = 1; _cels_canc_once; _cels_canc_once = 0)
#endif

/**
 * @def run
 * @brief Defines the cooperative coroutine execution body of a CEL_Task.
 *
 * What it does:
 * Encloses the procedural step-by-step logic of the task executing inside a cooperative
 * fiber. Preserves all local stack variables, supports nested helper function calls, and
 * allows native C switch statements across cel_yield() and cel_wait() suspension points.
 *
 * Expected outcome:
 * Executes cooperatively across multiple engine frame ticks without blocking the main thread.
 *
 * Where to use:
 * Place inside a CEL_Task definition.
 */
#ifndef run
#define run \
    if (0) goto _cels_task_cancel_entry; \
    if (_cels_task_phase == CEL_TASK_PHASE_RUN)
#endif

/**
 * @def cel_yield
 * @brief Suspends task execution and yields control to the engine until the next frame.
 *
 * What it does:
 * Suspends the task's cooperative fiber and yields control back to the session loop.
 * Schedules the task's composable group for recomposition on the subsequent frame.
 * Local variables on the C call stack are preserved intact.
 *
 * Where to use:
 * Call exclusively inside the run block of a CEL_Task (or inside any helper subroutine called by it).
 */
#ifndef cel_yield
#define cel_yield() \
    do { \
        CelsTaskYield(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, 0); \
        CelsFiberSwitch((CelsFiber*)_cels_task_state->callerFiber); \
    } while(0)
#endif

/**
 * @def cel_wait
 * @brief Suspends task execution for a specified non-blocking delay in milliseconds.
 *
 * What it does:
 * Calculates a monotonic deadline timestamp (`now + ms`) and suspends the cooperative fiber.
 * On every frame before the deadline expires, the task remains suspended without switching
 * into the fiber. Once elapsed, execution resumes right after cel_wait() with full stack preservation.
 *
 * @param ms Delay duration in milliseconds.
 */
#ifndef cel_wait
#define cel_wait(ms) \
    do { \
        CelsTaskWait(CelsGetCurrentSession(), _cels_task_state, _cels_task_key, 0, (uint32_t)(ms)); \
        CelsFiberSwitch((CelsFiber*)_cels_task_state->callerFiber); \
    } while(0)
#endif

/**
 * @def cel_cancel
 * @brief Cancels task execution from within the task body and executes teardown.
 *
 * What it does:
 * Transitions the task state to cancelled and performs an immediate jump to the
 * task's cancel block.
 */
#ifndef cel_cancel
#define cel_cancel() \
    do { \
        CelsTaskCancel(_cels_task_state); \
        _cels_task_phase = CEL_TASK_PHASE_CANCEL; \
        goto _cels_task_cancel_entry; \
    } while(0)
#endif

/**
 * @def cel_task
 * @brief Invokes and schedules a declared CEL_Task inside an active composition.
 *
 * What it does:
 * Enters the task's composable group in the session slot table, initializes or restores
 * the coroutine state, executes active steps, and exits the group.
 *
 * Expected outcome:
 * Advances the task state machine by one step (or polls its waiting timer).
 *
 * Where to use:
 * Inside any CEL_Composition or CEL_Composable body.
 *
 * Example:
 * @code
 *     CEL_Composition(GameScreen, void *userData) {
 *         cel_task(EnemySpawnTask, 5, 2.5f);
 *     }
 * @endcode
 */
#ifndef cel_task
#define cel_task(Name, ...) Name(__VA_ARGS__)
#endif

/**
 * @def cel_cancel_task
 * @brief Externally cancels the active instance of a declared task.
 *
 * What it does:
 * Marks the specified task's slot state as cancelled and immediately triggers its
 * cancel teardown block.
 *
 * Expected outcome:
 * The task ceases running and executes any registered cleanup logic.
 *
 * Where to use:
 * Call from UI event handlers, buttons, or parent compositions.
 *
 * Example:
 * @code
 *     if (UserClickedAbort()) {
 *         cel_cancel_task(ConnectServerTask);
 *     }
 * @endcode
 */
#ifndef cel_cancel_task
#define cel_cancel_task(Name) _cels_task_cancel_##Name(CelsGetCurrentSession())
#endif

/**
 * @def cel_is_task_running
 * @brief Queries whether a declared task is actively running.
 *
 * What it does:
 * Inspects the persistent task state in the active session.
 *
 * Expected outcome:
 * Returns true if the task has been started and has neither completed nor been cancelled.
 *
 * Where to use:
 * Call in composables to toggle loading spinners, disable buttons, or coordinate tasks.
 */
#ifndef cel_is_task_running
#define cel_is_task_running(Name) _cels_task_is_running_##Name(CelsGetCurrentSession())
#endif

/**
 * @def cel_is_task_done
 * @brief Queries whether a declared task has completed execution.
 *
 * What it does:
 * Inspects the persistent task state in the active session.
 *
 * Expected outcome:
 * Returns true if the task's run block executed to completion.
 *
 * Where to use:
 * Call in composables to detect task completion and trigger subsequent UI flows.
 */
#ifndef cel_is_task_done
#define cel_is_task_done(Name) _cels_task_is_done_##Name(CelsGetCurrentSession())
#endif

/**
 * @def cel_is_task_cancelled
 * @brief Queries whether a declared task was cancelled.
 *
 * What it does:
 * Inspects the persistent task state in the active session.
 *
 * Expected outcome:
 * Returns true if the task was cancelled via cel_cancel() or cel_cancel_task().
 *
 * Where to use:
 * Call in composables to display cancellation banners or retry prompts.
 */
#ifndef cel_is_task_cancelled
#define cel_is_task_cancelled(Name) _cels_task_is_cancelled_##Name(CelsGetCurrentSession())
#endif

/**
 * @def cel_restart_task
 * @brief Resets a completed or cancelled task back to step 0.
 *
 * What it does:
 * Clears the task's done and cancelled flags, resets its execution step to 0, and
 * invalidates the session group so it restarts execution on the next frame.
 *
 * Expected outcome:
 * The task begins executing from the start of its run block on the next frame.
 *
 * Where to use:
 * Call from retry buttons or restart event handlers.
 */
#ifndef cel_restart_task
#define cel_restart_task(Name) _cels_task_restart_##Name(CelsGetCurrentSession())
#endif

/**
 * @def CEL_Task
 * @brief Declares a procedural coroutine task with persistent step tracking and unmount cleanup.
 *
 * What it does:
 * Generates an inline composable function and internal coroutine state machine. The task
 * allocates a `CelsTaskState` struct in the session's slot memory using cel_remember(),
 * pins execution step and non-blocking timers across recomposition passes, runs the `run`
 * block step-by-step cooperatively, and invokes the `cancel` teardown block when aborted
 * or unmounted.
 *
 * Expected outcome:
 * Enables procedural, multi-frame logic (animations, sequential API calls, timers, delays)
 * within declarative CELS code without spawning OS threads or blocking the main frame loop.
 *
 * Where to use:
 * Declare at file/global scope. Invoke inside any CEL_Composition or CEL_Composable using
 * `cel_task(TaskName, ...)`.
 *
 * Supported signatures:
 * - 0 arguments: CEL_Task(TaskName) { ... }
 * - 1 argument:  CEL_Task(TaskName, Type1, Arg1) { ... }
 * - 2 arguments: CEL_Task(TaskName, Type1, Arg1, Type2, Arg2) { ... }
 *
 * Example:
 * @code
 *     CEL_Task(ConnectTask, const char*, host, int, port) {
 *         cancel {
 *             CloseSocket();
 *         }
 *         run {
 *             InitSocket();
 *             cel_wait(100); // 100ms async wait
 *             ConnectTo(host, port);
 *             cel_yield();   // pause until next frame
 *             SendHandshake();
 *         }
 *     }
 *
 *     CEL_Composition(MainView, void *userData) {
 *         cel_task(ConnectTask, "127.0.0.1", 9000);
 *     }
 * @endcode
 */
#ifndef CEL_Task
#define CEL_Task(...) \
    _CEL_GET_TASK_MACRO(__VA_ARGS__, _CEL_TASK_2, _UNUSED, _CEL_TASK_1, _UNUSED, _CEL_TASK_0)(__VA_ARGS__)
#endif

/* ========================================================================= */
/* Transaction Batch Staging (cel_stage_*)                                   */
/* ========================================================================= */

/**
 * @def cel_stage_set
 * @brief Stages a component or payload assignment transaction in the active session.
 *
 * What it does:
 * Appends a CELS_OP_SET operation to the session's active transaction batch. Serializes
 * the provided struct value into the batch's linear byte arena without heap allocations
 * or mutex locks.
 *
 * Expected outcome:
 * The operation is buffered until the frame sync point or commit hook, where it will be
 * processed by external consumers (ECS world, render backend, physics engine).
 *
 * Where to use:
 * Call from within any CEL_Composable, CEL_Composition, or CEL_Task.
 *
 * Example:
 * @code
 *     cel_stage_set(entityId, Position, { .x = 10.0f, .y = 20.0f });
 * @endcode
 */
#ifndef cel_stage_set
#define cel_stage_set(targetId, Type, ...) \
    CelsSessionStageSet(CelsGetCurrentSession(), \
                        (uint64_t)(targetId), \
                        CelsHashKey(#Type), \
                        sizeof(Type), \
                        &(Type)__VA_ARGS__)
#endif

/**
 * @def cel_stage_remove
 * @brief Stages a component removal or tag detachment transaction in the active session.
 *
 * What it does:
 * Appends a CELS_OP_REMOVE operation identifying the targetId and component type key
 * to the session's transaction staging batch.
 *
 * Expected outcome:
 * At the transaction commit point, the handler is notified to remove or unbind the
 * component from the target entity.
 *
 * Where to use:
 * Call inside composables or tasks when conditionally shedding components or tags.
 *
 * Example:
 * @code
 *     cel_stage_remove(entityId, RigidBody);
 * @endcode
 */
#ifndef cel_stage_remove
#define cel_stage_remove(targetId, Type) \
    CelsSessionStageRemove(CelsGetCurrentSession(), \
                           (uint64_t)(targetId), \
                           CelsHashKey(#Type))
#endif

/**
 * @def cel_stage_delete
 * @brief Stages a target deletion or destruction transaction in the active session.
 *
 * What it does:
 * Appends a CELS_OP_DELETE operation targeting targetId to the session's transaction batch.
 *
 * Expected outcome:
 * At the frame sync point, the backend consumer destroys the associated entity or resource.
 *
 * Where to use:
 * Inside composables or tasks when an entity should be despawned or destroyed.
 *
 * Example:
 * @code
 *     cel_stage_delete(deadEnemyId);
 * @endcode
 */
#ifndef cel_stage_delete
#define cel_stage_delete(targetId) \
    CelsSessionStageDelete(CelsGetCurrentSession(), (uint64_t)(targetId))
#endif

/**
 * @def cel_stage_custom
 * @brief Stages a custom user-defined transaction operation with a typed payload.
 *
 * What it does:
 * Enqueues an application-defined opcode with a custom struct payload into the transaction batch.
 *
 * Expected outcome:
 * Allows arbitrary cross-thread or cross-subsystem messages (e.g. audio triggers, networking
 * packets, draw commands) to be staged locklessly during recomposition.
 *
 * Where to use:
 * Inside composables or tasks for domain-specific operations beyond SET/REMOVE/DELETE.
 *
 * Example:
 * @code
 *     cel_stage_custom(OP_PLAY_AUDIO, soundId, AudioParams, { .volume = 0.8f, .pitch = 1.0f });
 * @endcode
 */
#ifndef cel_stage_custom
#define cel_stage_custom(opCode, targetId, Type, ...) \
    CelsSessionStageCustom(CelsGetCurrentSession(), \
                           (uint32_t)(opCode), \
                           (uint64_t)(targetId), \
                           CelsHashKey(#Type), \
                           sizeof(Type), \
                           &(Type)__VA_ARGS__)
#endif

/**
 * @def cel_user_data
 * @brief Retrieves the ambient session's attached user context pointer.
 *
 * What it does:
 * Queries CelsGetCurrentSession() and retrieves its arbitrary userData pointer,
 * casting it to `Type*`.
 *
 * Expected outcome:
 * Returns the application-level context object configured during session initialization
 * or composition attachment.
 *
 * Where to use:
 * Inside composables needing access to application-wide non-reactive context (e.g. host pointers,
 * native window handles, asset managers).
 *
 * Example:
 * @code
 *     AppContext *ctx = cel_user_data(AppContext);
 * @endcode
 */
#ifndef cel_user_data
#define cel_user_data(Type) \
    ((Type*)CelsSessionGetUserData(CelsGetCurrentSession()))
#endif

/**
 * @def cel_session_user_data
 * @brief Retrieves an explicit session's attached user context pointer.
 *
 * What it does:
 * Reads the userData pointer from a specific CelsSession instance and casts it to `Type*`.
 *
 * Expected outcome:
 * Returns the userData pointer associated with the given session.
 *
 * Where to use:
 * In multi-session hosts, background tasks, or callback hooks where the session pointer
 * is already available.
 */
#ifndef cel_session_user_data
#define cel_session_user_data(session, Type) \
    ((Type*)CelsSessionGetUserData(session))
#endif

/* ========================================================================= */
/* Declarative Temporal Transitions (cel_transition)                         */
/* ========================================================================= */

/**
 * @def cel_transition
 * @brief Declaratively transitions a scalar value toward a target over time.
 *
 * 1. What it does exactly:
 * Allocates or retrieves a persistent CelsTransitionState struct inside the
 * calling composable's slot-table memory via cel_remember(). Computes the
 * current interpolated value based on monotonic elapsed time and the selected
 * easing curve. Automatically invalidates the enclosing composable group on
 * subsequent frames while in motion, and automatically ceases invalidation
 * once the target value settles. Supports seamless in-flight retargeting with
 * zero visual pops.
 *
 * 2. Expected outcome:
 * Returns the smoothed float value for the current frame. While converging,
 * schedules the session for continuous next-frame recomposition. Once converged,
 * drops to 0% CPU idle without triggering further recompositions.
 *
 * 3. Where to use it:
 * Inside any active CEL_Composable body during a recomposition pass.
 *
 * 4. Usage example:
 * @code
 *     CEL_Composable(PlayerHealthHUD) {
 *         const PlayerState *p = cel_watch(PlayerState, CEL_Player);
 *         if (!p) return;
 *
 *         // Smoothly ease visual bar toward authoritative health over 400ms
 *         float visualHealth = cel_transition(p->currentHealth, 400, CEL_EASE_OUT_QUAD);
 *         DrawHealthBar(visualHealth, p->maxHealth);
 *     }
 * @endcode
 */
#ifndef cel_transition
#define cel_transition(...) \
    _CEL_GET_TRANS_MACRO(__VA_ARGS__, _CEL_TRANS_3, _CEL_TRANS_2, _UNUSED)(__VA_ARGS__)
#endif

/* ========================================================================= */
/* Child Introspection & Sibling Metrics (cel_child_*)                       */
/* ========================================================================= */

/**
 * Resolves full layout and sibling metadata for the currently executing composable.
 *
 * @param outInfo Destination pointer to receive child info struct.
 * @return CELS_OK on success,
 *         CELS_ERROR_INVALID_ARGUMENT if outInfo is NULL,
 *         CELS_ERROR_INVALID_STATE if called outside a container or outside active session.
 */
static inline CelsResult cel_child_info(CelsChildInfo *outInfo)
{
    return CelsGetChildInfo(CelsGetCurrentSession(), outInfo);
}

/**
 * Resolves the 0-based sequential sibling index of this child within its parent container.
 *
 * @param outIndex Destination pointer to receive 0-based index.
 * @return CELS_OK on success,
 *         CELS_ERROR_INVALID_ARGUMENT if outIndex is NULL,
 *         CELS_ERROR_INVALID_STATE if called outside a container or outside active session.
 */
static inline CelsResult cel_child_index(uint32_t *outIndex)
{
    if (outIndex == NULL) return CELS_ERROR_INVALID_ARGUMENT;
    CelsChildInfo info;
    CelsResult res = CelsGetChildInfo(CelsGetCurrentSession(), &info);
    if (res == CELS_OK) {
        *outIndex = info.index;
    }
    return res;
}

/**
 * Resolves the total sibling count of the parent container (or child count of current container).
 *
 * @param outCount Destination pointer to receive count.
 * @return CELS_OK on success,
 *         CELS_ERROR_INVALID_ARGUMENT if outCount is NULL,
 *         CELS_ERROR_INVALID_STATE if called outside active composition.
 */
static inline CelsResult cel_child_count(uint32_t *outCount)
{
    if (outCount == NULL) return CELS_ERROR_INVALID_ARGUMENT;
    CelsSession *sess = CelsGetCurrentSession();
    if (sess == NULL || sess->currentDepth == 0) return CELS_ERROR_INVALID_STATE;
    uint32_t depth = sess->currentDepth - 1;
    uint32_t groupIdx = sess->groupIndexStack[depth];
    const CelsSlotGroup *g = CelsGetGroup(sess, groupIdx);
    *outCount = (g != NULL && g->nodeCount > 0) ? (uint32_t)g->nodeCount : sess->childCounterStack[depth];
    return CELS_OK;
}

/**
 * Checks whether this child is the first element among siblings in its container (index == 0).
 */
static inline CelsResult cel_is_first_child(bool *outIsFirst)
{
    if (outIsFirst == NULL) return CELS_ERROR_INVALID_ARGUMENT;
    CelsChildInfo info;
    CelsResult res = CelsGetChildInfo(CelsGetCurrentSession(), &info);
    if (res == CELS_OK) {
        *outIsFirst = info.isFirst;
    }
    return res;
}

/**
 * Checks whether this child is the last element among siblings in its container.
 */
static inline CelsResult cel_is_last_child(bool *outIsLast)
{
    if (outIsLast == NULL) return CELS_ERROR_INVALID_ARGUMENT;
    CelsChildInfo info;
    CelsResult res = CelsGetChildInfo(CelsGetCurrentSession(), &info);
    if (res == CELS_OK) {
        *outIsLast = info.isLast;
    }
    return res;
}

/* ========================================================================= */
/* Ambient Context Scope (cel_get_context / cel_set_context)                 */
/* ========================================================================= */

#define _CEL_GET_CONTEXT_1(Type) \
    ((const Type *)CelsScopeFind(CelsGetCurrentSession() ? CelsGetCurrentSession()->ambientScope : NULL, CelsHashKey(#Type)))

#define _CEL_GET_CONTEXT_2(Type, outPtr) \
    CelsContextResolve(CelsGetCurrentSession(), CelsHashKey(#Type), (const void **)(outPtr))

#define _CEL_GET_CONTEXT_3(session, Type, outPtr) \
    CelsContextResolve((session), CelsHashKey(#Type), (const void **)(outPtr))

/**
 * @def cel_get_context
 * @def cel_getContext
 * @brief Reads ambient context up the ancestor call tree with optional CelsResult protection.
 *
 * Supported Signatures:
 * - cel_get_context(Type): Returns const Type* (or NULL if absent).
 * - cel_get_context(Type, &outPtr): Returns CelsResult (CELS_OK, CELS_ERROR_NOT_FOUND, CELS_ERROR_INVALID_STATE).
 * - cel_get_context(session, Type, &outPtr): Explicit session variant returning CelsResult.
 */
#define cel_get_context(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_GET_CONTEXT_3, _CEL_GET_CONTEXT_2, _CEL_GET_CONTEXT_1)(__VA_ARGS__)

#define cel_getContext(...) cel_get_context(__VA_ARGS__)

#define _CEL_SET_CONTEXT_2(Type, ptr) \
    CelsContextPush(CelsGetCurrentSession(), CelsHashKey(#Type), (const void *)(ptr))

#define _CEL_SET_CONTEXT_3(session, Type, ptr) \
    CelsContextPush((session), CelsHashKey(#Type), (const void *)(ptr))

/**
 * @def cel_set_context
 * @def cel_setContext
 * @brief Attaches ambient context to the active composable scope.
 *
 * Automatically restores ancestor scope when the current composable exits. Zero heap allocations.
 *
 * @param Type Struct type name.
 * @param ptr  Pointer to the context struct payload to scope down descendants.
 */
#define cel_set_context(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_SET_CONTEXT_3, _CEL_SET_CONTEXT_2, _UNUSED)(__VA_ARGS__)

#define cel_setContext(...) cel_set_context(__VA_ARGS__)

/**
 * @def cel_context
 * @brief Unified ambient context API alias (resolves read or write depending on parameter count).
 */
#define cel_context(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_SET_CONTEXT_3, _CEL_SET_CONTEXT_2, _CEL_GET_CONTEXT_1)(__VA_ARGS__)

/* ========================================================================= */
/* Core Container Scoping Helper (cel_container)                             */
/* ========================================================================= */

/**
 * @typedef CelsContainerStartHook
 * @brief Lifecycle callback executed immediately after entering a container scope.
 *
 * @param userData Opaque user context or layout parameters passed into cel_container.
 */
typedef void (*CelsContainerStartHook)(void *userData);

/**
 * @typedef CelsContainerEndHook
 * @brief Lifecycle callback executed immediately after child composables settle.
 *
 * Receives the exact count of direct child composables executed within the container scope,
 * allowing layout and container engines to perform instant post-layout index clamping,
 * geometry allocation, or directional focus navigation.
 *
 * @param childCount Number of direct child composables executed inside this container.
 * @param userData   Opaque user context or layout parameters passed into cel_container.
 */
typedef void (*CelsContainerEndHook)(uint32_t childCount, void *userData);

/**
 * @struct CelsContainerScope
 * @brief RAII scope tracking structure for cel_container inline blocks.
 *
 * Sized and ordered from largest alignment to smallest alignment (descending field
 * ordering) to eliminate structure padding and maintain 32-byte cache alignment.
 */
typedef struct CelsContainerScope {
    CelsSession           *session;     /**< Active CelsSession pointer (8 bytes). */
    CelsContainerEndHook   onGroupEnd;  /**< Post-children completion hook (8 bytes). */
    void                  *userData;    /**< Opaque user or layout parameters (8 bytes). */
    uint8_t                entered;     /**< Single-iteration loop guard flag (1 byte). */
    uint8_t                reserved[7]; /**< Alignment padding to 32 bytes (7 bytes). */
} CelsContainerScope;

/**
 * @brief Enters a container group scope in the session slot table.
 *
 * Resolves the ambient session, sets up the scope RAII tracking struct, enters the
 * composable group with the specified 64-bit key, and executes the onGroupStart hook.
 *
 * @param session       Active session pointer (if NULL, resolves via CelsGetCurrentSession()).
 * @param key           64-bit group key hash.
 * @param onGroupStart  Optional callback invoked on container start.
 * @param onGroupEnd    Optional callback invoked on container exit.
 * @param userData      Opaque user context passed to start and end hooks.
 * @param[out] outScope Pointer to caller-allocated CelsContainerScope structure.
 * @return True if successfully entered; false on failure or session unavailable.
 */
static inline bool CelsContainerScopeEnter(
    CelsSession           *session,
    uint64_t               key,
    CelsContainerStartHook onGroupStart,
    CelsContainerEndHook   onGroupEnd,
    void                  *userData,
    CelsContainerScope    *outScope)
{
    if (session == NULL) session = CelsGetCurrentSession();
    if (session == NULL || outScope == NULL) return false;
    outScope->session = session;
    outScope->entered = 1;
    outScope->onGroupEnd = onGroupEnd;
    outScope->userData = userData;

    if (!CelsEnterComposable(session, key)) {
        return false;
    }
    if (onGroupStart != NULL) {
        onGroupStart(userData);
    }
    return true;
}

/**
 * @brief Exits a container group scope, delivering settled child metrics to onGroupEnd.
 *
 * Queries the frame-accurate settled child counter from the session's child counter stack,
 * delivers the count and userData to onGroupEnd if registered, and invokes CelsExitGroup().
 *
 * @param scope Pointer to active CelsContainerScope.
 */
static inline void CelsContainerScopeExit(CelsContainerScope *scope)
{
    if (scope != NULL && scope->session != NULL) {
        if (scope->onGroupEnd != NULL) {
            uint32_t childCount = 0;
            if (scope->session->currentDepth > 0) {
                uint32_t depth = scope->session->currentDepth - 1;
                childCount = scope->session->childCounterStack[depth];
            }
            scope->onGroupEnd(childCount, scope->userData);
        }
        CelsExitGroup(scope->session);
    }
}

#define _CEL_CONTAINER_4(Name, onStart, onEnd, userData) \
    for (CelsContainerScope _cels_cscope = { 0 }; \
         !_cels_cscope.entered && CelsContainerScopeEnter(CelsGetCurrentSession(), CelsHashKey(#Name), \
                                                          (CelsContainerStartHook)(onStart), \
                                                          (CelsContainerEndHook)(onEnd), \
                                                          (void*)(userData), &_cels_cscope); \
         CelsContainerScopeExit(&_cels_cscope))

#define _CEL_CONTAINER_3(Name, onStart, onEnd) \
    _CEL_CONTAINER_4(Name, onStart, onEnd, NULL)

#define _CEL_CONTAINER_2(Name, onStart) \
    _CEL_CONTAINER_4(Name, onStart, NULL, NULL)

#define _CEL_CONTAINER_1(Name) \
    _CEL_CONTAINER_4(Name, NULL, NULL, NULL)

/**
 * @def cel_container
 * @brief Establishes an inline container group in the slot table with optional group hooks.
 *
 * What it does:
 * Enters a composable group in the slot table for the duration of the enclosed block,
 * automatically recording child metrics (cel_child_index, cel_child_count, isFirst, isLast).
 * Supports optional onGroupStart and onGroupEnd lifecycle hooks for custom layout engines.
 * Zero heap allocations.
 *
 * Signatures:
 * - cel_container(Name)
 * - cel_container(Name, onGroupStart)
 * - cel_container(Name, onGroupStart, onGroupEnd)
 * - cel_container(Name, onGroupStart, onGroupEnd, userData)
 *
 * Hook signatures:
 * - void onGroupStart(void *userData);
 * - void onGroupEnd(uint32_t childCount, void *userData);
 *
 * Example:
 * @code
 *     cel_container(Toolbar) {
 *         Button("File");
 *         Button("Edit");
 *     }
 * @endcode
 */
#define cel_container(...) \
    _CEL_GET_MACRO_4(__VA_ARGS__, _CEL_CONTAINER_4, _CEL_CONTAINER_3, _CEL_CONTAINER_2, _CEL_CONTAINER_1)(__VA_ARGS__)

#define _CEL_CONTAINER_KEYED_5(Name, id, onStart, onEnd, userData) \
    for (CelsContainerScope _cels_cscope = { 0 }; \
         !_cels_cscope.entered && CelsContainerScopeEnter(CelsGetCurrentSession(), CelsKeyIndex(CelsHashKey(#Name), (uint64_t)(id)), \
                                                          (CelsContainerStartHook)(onStart), \
                                                          (CelsContainerEndHook)(onEnd), \
                                                          (void*)(userData), &_cels_cscope); \
         CelsContainerScopeExit(&_cels_cscope))

#define _CEL_CONTAINER_KEYED_4(Name, id, onStart, onEnd) \
    _CEL_CONTAINER_KEYED_5(Name, id, onStart, onEnd, NULL)

#define _CEL_CONTAINER_KEYED_3(Name, id, onStart) \
    _CEL_CONTAINER_KEYED_5(Name, id, onStart, NULL, NULL)

#define _CEL_CONTAINER_KEYED_2(Name, id) \
    _CEL_CONTAINER_KEYED_5(Name, id, NULL, NULL, NULL)

/**
 * @def cel_container_keyed
 * @brief Establishes an inline container group in the slot table with an explicit 64-bit ID.
 *
 * Identical to cel_container, but derives its slot table identity by compounding the
 * symbolic Name with an explicit 64-bit identifier (via CelsKeyIndex(CelsHashKey(#Name), id)),
 * enabling safe reconciliation of dynamic container lists or entity-bound containers.
 *
 * Signatures:
 * - cel_container_keyed(Name, id)
 * - cel_container_keyed(Name, id, onGroupStart)
 * - cel_container_keyed(Name, id, onGroupStart, onGroupEnd)
 * - cel_container_keyed(Name, id, onGroupStart, onGroupEnd, userData)
 */
#define cel_container_keyed(...) \
    _CEL_GET_MACRO_5(__VA_ARGS__, _CEL_CONTAINER_KEYED_5, _CEL_CONTAINER_KEYED_4, _CEL_CONTAINER_KEYED_3, _CEL_CONTAINER_KEYED_2, _UNUSED)(__VA_ARGS__)

/* ========================================================================= */
/* Composable Identity, Keys & Components (cel_key / CEL_Entity / cel_has)   */
/* ========================================================================= */

#define _CEL_KEY_1(id) \
    for (CelsEntityScope _cels_key_scope = { 0 }; \
         !_cels_key_scope.isActive && CelsEnterEntityScope(CelsGetCurrentSession(), (uint64_t)(id), NULL, &_cels_key_scope); \
         CelsExitEntityScope(CelsGetCurrentSession(), &_cels_key_scope))

#define _CEL_KEY_2(id, name) \
    for (CelsEntityScope _cels_key_scope = { 0 }; \
         !_cels_key_scope.isActive && CelsEnterEntityScope(CelsGetCurrentSession(), (uint64_t)(id), (name), &_cels_key_scope); \
         CelsExitEntityScope(CelsGetCurrentSession(), &_cels_key_scope))

#define _CEL_KEY_3(world, name, id) \
    for (CelsEntityScope _cels_key_scope = { 0 }; \
         !_cels_key_scope.isActive && CelsEnterEntityScope(CelsGetCurrentSession(), (uint64_t)(id), (name), &_cels_key_scope); \
         CelsExitEntityScope(CelsGetCurrentSession(), &_cels_key_scope))

/**
 * @def cel_key
 * @brief Establishes an explicit identity/key scope for list reconciliation and component attachment.
 *
 * What it does:
 * Enters a composable group in the slot table keyed by 64-bit ID. When used in loops over dynamic
 * lists, CELS reconciles sibling groups by key so local slot memory (cel_remember) and reactive
 * bindings are preserved across item reordering. When items are removed, they are automatically
 * unmounted and their components/entities pruned.
 *
 * Example:
 * @code
 *     for (int i = 0; i < count; i++) {
 *         cel_key(items[i].id) {
 *             TodoItemCard(&items[i]);
 *         }
 *     }
 * @endcode
 */
#define cel_key(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_KEY_3, _CEL_KEY_2, _CEL_KEY_1, _UNUSED)(__VA_ARGS__)

/**
 * @def CEL_Entity
 * @brief Alias to cel_key for backward-compatible declarative entity scopes.
 */
#define CEL_Entity(...) cel_key(__VA_ARGS__)

/**
 * @def cel_id
 * @brief Retrieves the 64-bit entity/node ID of the active composable scope.
 *
 * What it does:
 * Returns the active 64-bit ID for the current composable scope. If the composable was declared
 * with an explicit ID parameter (e.g. CEL_Composable(MyWidget, id, ...)), returns that ID.
 * If unkeyed, returns the auto-generated composable group ID.
 *
 * Note:
 * cel_id() is strictly a getter. Developers pass explicit IDs directly to keyed composables
 * via CEL_Composable(Name, id, ...) rather than setting them imperatively.
 */
#define cel_id() CelsGetActiveEntityId(CelsGetCurrentSession())
#define cel_entity_id() cel_id()

/**
 * @def cel_call
 * @brief Keyed composable invocation helper to invoke any composable with an explicit key on a single line.
 *
 * Example:
 * @code
 *     for (int i = 0; i < count; i++) {
 *         cel_call(TodoItemCard, items[i].id, &items[i]);
 *     }
 * @endcode
 */
#define cel_call(Fn, id, ...) do { \
    cel_key(id) { \
        Fn(__VA_ARGS__); \
    } \
} while(0)

/**
 * @def cel_has
 * @brief Declaratively binds a component to the active composable/entity scope.
 *
 * What it does:
 * Stages a CELS_OP_SET transaction for the component. When omitted on subsequent frames,
 * CELS automatically stages a CELS_OP_REMOVE transaction at the close of the scope.
 * When the composable unmounts from the hierarchy, CELS automatically stages CELS_OP_DELETE.
 *
 * Example:
 * @code
 *     CEL_Composable(EnemyView, id, const Enemy *enemy) {
 *         cel_has(Position, { .x = enemy->x, .y = enemy->y });
 *         if (enemy->isBurning) {
 *             cel_has(BurnDebuff, { .dps = 15.0f });
 *         }
 *     }
 * @endcode
 */
#define cel_has(Type, ...) \
    CelsEntityRegisterComponent(CelsGetCurrentSession(), CelsHashKey(#Type), sizeof(Type), &(Type)__VA_ARGS__)

/**
 * @def cel_has_tag
 * @brief Declaratively binds a zero-sized tag component to the active composable/entity scope.
 */
#define cel_has_tag(Type) \
    CelsEntityRegisterComponent(CelsGetCurrentSession(), CelsHashKey(#Type), 0, NULL)

/**
 * @def cel_get
 * @brief Queries an attached component, checking CELS intra-frame staged batch first,
 * then falling back to registered ECS lookup hook (Flecs).
 *
 * Overloads:
 * - cel_get(Type): Reads component attached to current active composable node (cel_id()).
 * - cel_get(entityId, Type): Reads component attached to explicit entity ID.
 *
 * Example:
 * @code
 *     const Position *pos = cel_get(Position);
 *     const Health *targetHp = cel_get(targetEntity, Health);
 * @endcode
 */
#define cel_get(...) _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_GET_2, _CEL_GET_1, _UNUSED)(__VA_ARGS__)
#define _CEL_GET_1(Type)           ((const Type*)CelsEntityGetComponent(CelsGetCurrentSession(), CelsGetActiveEntityId(CelsGetCurrentSession()), CelsHashKey(#Type)))
#define _CEL_GET_2(entityId, Type) ((const Type*)CelsEntityGetComponent(CelsGetCurrentSession(), (uint64_t)(entityId), CelsHashKey(#Type)))

/**
 * @def cel_is
 * @brief Checks if a component or tag is attached in the active frame or registered ECS.
 *
 * Overloads:
 * - cel_is(Type): Checks component on current active composable node (cel_id()).
 * - cel_is(entityId, Type): Checks component on explicit entity ID.
 */
#define cel_is(...) _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_IS_2, _CEL_IS_1, _UNUSED)(__VA_ARGS__)
#define _CEL_IS_1(Type)           CelsEntityHasComponent(CelsGetCurrentSession(), CelsGetActiveEntityId(CelsGetCurrentSession()), CelsHashKey(#Type))
#define _CEL_IS_2(entityId, Type) CelsEntityHasComponent(CelsGetCurrentSession(), (uint64_t)(entityId), CelsHashKey(#Type))

#ifdef __cplusplus
}
#endif
