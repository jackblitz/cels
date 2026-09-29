#pragma once

/**
 * @file cels.h
 * @brief Public API and Declarative DSL Macros for CELS.
 *
 * CELS (Composition, Evaluation, Lifecycle, State) is a high-performance,
 * cache-aligned declarative composition engine for C99.
 *
 * Four Core Concepts:
 * - C: Composition (CEL_Composition, CEL_Composable, cel_attach)
 * - E: Evaluation  (Fine-grained recomposition, CEL_Evaluate)
 * - L: Lifecycle   (Pure mount & unmount topology tracking: CEL_Lifecycle, cel_lifecycle)
 * - S: State       (Double-buffered cache-aligned state: cel_remember, cel_remember_state,
 *                   cel_watch, cel_get_state, cel_mutate)
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cels/slot_table.h"
#include "cels/state.h"
#include "cels/session.h"
#include "cels/log.h"
#include "cels/engine.h"
#include "cels/app.h"
#include "cels/module.h"
#include "cels/thread.h"
#include "cels/task.h"
#include "cels/version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Macro Helpers for Arity Dispatch                                          */
/* ========================================================================= */

#define _CEL_GET_MACRO_2(_1, _2, NAME, ...) NAME
#define _CEL_GET_MACRO_3(_1, _2, _3, NAME, ...) NAME
#define _CEL_GET_MACRO_4(_1, _2, _3, _4, NAME, ...) NAME
#define _CEL_GET_MACRO_6(_1, _2, _3, _4, _5, _6, NAME, ...) NAME

/* ========================================================================= */
/* Reactive State Declaration (CEL_State)                                    */
/* ========================================================================= */

#ifndef CEL_State
#define CEL_State(TypeName) \
    typedef struct TypeName TypeName; \
    struct TypeName
#endif

#ifndef CEL_Module
#define CEL_Module(TypeName) \
    typedef struct TypeName TypeName; \
    struct TypeName
#endif

/* ========================================================================= */
/* Root Composition Declaration (CEL_Composition)                            */
/* ========================================================================= */

#define _CEL_COMPOSITION_1(CompName) \
    void CompName(void *userData)

#define _CEL_COMPOSITION_2(CompName, Arg) \
    void CompName(Arg)

#define CEL_Composition(...) \
    _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_COMPOSITION_2, _CEL_COMPOSITION_1)(__VA_ARGS__)

/* ========================================================================= */
/* Composition Evaluation Predicate (CEL_Evaluate)                           */
/* ========================================================================= */

#define CEL_Evaluate(Predicate, ctx) ((Predicate) ? (Predicate)((ctx)) : true)
#define CEL_EvaluateFn(Name, ctxType, ctxName) static bool Name(ctxType ctxName)
#define CEL_Evaluation(Name, ctxType, ctxName) static bool Name(ctxType ctxName)

/* ========================================================================= */
/* Root Composition Attachment (cel_attach)                                  */
/* ========================================================================= */

#define _CEL_ATTACH_3(s, id, comp) \
    CelsSessionAttachComposition((s), (id), (void(*)(void*))(comp), NULL, NULL, NULL)

#define _CEL_ATTACH_4(s, id, comp, userData) \
    CelsSessionAttachComposition((s), (id), (void(*)(void*))(comp), (void*)(userData), NULL, NULL)

#define _CEL_ATTACH_5(s, id, comp, userData, eval) \
    CelsSessionAttachComposition((s), (id), (void(*)(void*))(comp), (void*)(userData), (bool(*)(void*))(eval), NULL)

#define _CEL_ATTACH_6(s, id, comp, userData, eval, evalCtx) \
    CelsSessionAttachComposition((s), (id), (void(*)(void*))(comp), (void*)(userData), (bool(*)(void*))(eval), (void*)(evalCtx))

#define cel_attach(...) \
    _CEL_GET_MACRO_6(__VA_ARGS__, _CEL_ATTACH_6, _CEL_ATTACH_5, _CEL_ATTACH_4, _CEL_ATTACH_3, _UNUSED, _UNUSED)(__VA_ARGS__)

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

#define _CEL_GET_COMPOSABLE_MACRO(_1, _2, _3, _4, _5, _6, _7, _8, _9, NAME, ...) NAME

#define CEL_Composable(...) \
    _CEL_GET_COMPOSABLE_MACRO(__VA_ARGS__, _CEL_COMPOSABLE_4_ARGS, _UNUSED, _CEL_COMPOSABLE_3_ARGS, _UNUSED, _CEL_COMPOSABLE_2_ARGS, _UNUSED, _CEL_COMPOSABLE_1_ARG, _CEL_COMPOSABLE_VOID, _CEL_COMPOSABLE_VOID)(__VA_ARGS__)

/* ========================================================================= */
/* Non-inline Composable Export & Declaration for multi-file translation units*/
/* ========================================================================= */

#define CEL_ComposableDecl(FnName, ...) void FnName(__VA_ARGS__)

#define _CEL_COMPOSABLE_EXPORT_VOID(FnName, ...) \
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

#define _CEL_COMPOSABLE_EXPORT_1_ARG(FnName, Type1, Arg1) \
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

#define _CEL_COMPOSABLE_EXPORT_2_ARGS(FnName, Type1, Arg1, Type2, Arg2) \
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

#define _CEL_COMPOSABLE_EXPORT_3_ARGS(FnName, Type1, Arg1, Type2, Arg2, Type3, Arg3) \
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

#define _CEL_COMPOSABLE_EXPORT_4_ARGS(FnName, Type1, Arg1, Type2, Arg2, Type3, Arg3, Type4, Arg4) \
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

#define CEL_ComposableExport(...) \
    _CEL_GET_COMPOSABLE_MACRO(__VA_ARGS__, _CEL_COMPOSABLE_EXPORT_4_ARGS, _UNUSED, _CEL_COMPOSABLE_EXPORT_3_ARGS, _UNUSED, _CEL_COMPOSABLE_EXPORT_2_ARGS, _UNUSED, _CEL_COMPOSABLE_EXPORT_1_ARG, _CEL_COMPOSABLE_EXPORT_VOID, _CEL_COMPOSABLE_EXPORT_VOID)(__VA_ARGS__)

#define CEL_ComposableDef CEL_ComposableExport

/* ========================================================================= */
/* Pure Lifecycles (CEL_Lifecycle & cel_lifecycle)                           */
/* ========================================================================= */

#define CEL_LIFECYCLE_PHASE_MOUNT   1
#define CEL_LIFECYCLE_PHASE_UNMOUNT 2

#define mount   if (_cels_lifecycle_phase == CEL_LIFECYCLE_PHASE_MOUNT)
#define unmount if (_cels_lifecycle_phase == CEL_LIFECYCLE_PHASE_UNMOUNT)

/**
 * Declares a reusable lifecycle with typed parameter storage.
 *
 * Example:
 * @code
 *     CEL_Lifecycle(NativeResourceLifecycle, ResourceHandle *handle)
 *     {
 *         mount {
 *             NativeAcquire(handle);
 *         }
 *         unmount {
 *             NativeRelease(handle);
 *         }
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
    _cels_lifecycle_attach_##Name(CelsGetCurrentSession(), (void*)(handle))

#define _CEL_LIFECYCLE_INVOKE_1(Name) \
    _cels_lifecycle_attach_##Name(CelsGetCurrentSession(), NULL)

#define cel_lifecycle(...) \
    _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_LIFECYCLE_INVOKE_2, _CEL_LIFECYCLE_INVOKE_1)(__VA_ARGS__)

#define CEL_Attach cel_attach
#define CEL_GetState cel_get_state
#define CEL_None NULL

/* ========================================================================= */
/* Addressable Persistent State (cel_remember_state)                         */
/* ========================================================================= */

#define cel_remember_state(id, Type, defaultVal) \
    ((Type*)CelsSessionRememberState(CelsGetCurrentSession(), (id), sizeof(Type), &(defaultVal)))

#define cel_session_remember_state(session, id, Type, defaultVal) \
    ((Type*)CelsSessionRememberState((session), (id), sizeof(Type), &(defaultVal)))

/* ========================================================================= */
/* Private Local Memory (cel_remember)                                       */
/* ========================================================================= */

#define cel_remember(Type, ...) \
    ((Type*)CelsResolveSlot(CelsGetCurrentSession(), sizeof(Type), &(Type){ __VA_ARGS__ }))

/* ========================================================================= */
/* Reactive Observation & State Reading (cel_watch & cel_get_state)          */
/* ========================================================================= */

#define _CEL_WATCH_1(Type) \
    ((const Type*)CelsStateWatch(CelsGetCurrentSession(), CelsHashKey(#Type), sizeof(Type)))

#define _CEL_WATCH_2(Type, id) \
    ((const Type*)CelsStateWatch(CelsGetCurrentSession(), (id), sizeof(Type)))

#define _CEL_WATCH_3(session, Type, id) \
    ((const Type*)CelsStateWatch((session), (id), sizeof(Type)))

#define cel_watch(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_WATCH_3, _CEL_WATCH_2, _CEL_WATCH_1)(__VA_ARGS__)

#define _CEL_GET_STATE_2(id, Type) \
    ((const Type*)CelsStateGet(CelsGetCurrentSession(), (id), sizeof(Type)))

#define _CEL_GET_STATE_3(session, id, Type) \
    ((const Type*)CelsStateGet((session), (id), sizeof(Type)))

#define cel_get_state(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_GET_STATE_3, _CEL_GET_STATE_2)(__VA_ARGS__)

/* ========================================================================= */
/* State Mutation (cel_mutate)                                               */
/* Block syntax: cel_mutate(session, id, Type) { this->field = val; }        */
/* Value syntax: cel_mutate(session, id, Type, modifiedVal)                 */
/* ========================================================================= */

#define _CEL_MUTATE_2(id, Type) \
    for (Type *this = (Type*)CelsStateMutate(CelsGetCurrentSession(), (id), sizeof(Type)); \
         this != NULL; \
         this = NULL)

#define _CEL_MUTATE_3(session, id, Type) \
    for (Type *this = (Type*)CelsStateMutate((session), (id), sizeof(Type)); \
         this != NULL; \
         this = NULL)

#define _CEL_MUTATE_4(session, id, Type, val) \
    do { \
        Type *_cels_dst = (Type*)CelsStateMutate((session), (id), sizeof(Type)); \
        if (_cels_dst != NULL) { \
            *_cels_dst = (val); \
        } \
    } while (0)

#define cel_mutate(...) \
    _CEL_GET_MACRO_4(__VA_ARGS__, _CEL_MUTATE_4, _CEL_MUTATE_3, _CEL_MUTATE_2)(__VA_ARGS__)

#define cel_mutate_ptr(session, id, Type) \
    ((Type*)CelsStateMutate((session), (id), sizeof(Type)))

/* ========================================================================= */
/* Engine Loop Termination (cel_quit)                                        */
/* ========================================================================= */

#define cel_engine_quit() CelsEngineQuit(NULL)
#define cel_quit()        CelsEngineQuit(NULL)

/* ========================================================================= */
/* Flecs ECS Integration (Optional)                                          */
/* ========================================================================= */

#if defined(flecs_STATIC) || defined(FLECS_H) || defined(flecs_EXPORTS) || defined(CELS_ENABLE_FLECS)
#ifndef _CELS_FLECS_INTEGRATION_DEFINED
#define _CELS_FLECS_INTEGRATION_DEFINED

typedef struct CelsEntitySlot {
    ecs_world_t  *world;
    ecs_entity_t  entity;
} CelsEntitySlot;

static inline void _cels_entity_cleanup(void *instance, CelsSession *s) {
    (void)s;
    CelsEntitySlot *slot = (CelsEntitySlot*)instance;
    if (slot && slot->world && ecs_is_valid(slot->world, slot->entity)) {
        ecs_delete(slot->world, slot->entity);
    }
    if (slot) {
        slot->entity = 0;
        slot->world = NULL;
    }
}

static inline ecs_entity_t _cels_resolve_entity(
    CelsSession *s, 
    ecs_world_t *world, 
    const char *name, 
    uint64_t key
) {
    (void)key;
    bool isMount = CelsIsFreshMount(s);

    CelsEntitySlot *slot = (CelsEntitySlot*)CelsResolveSlot(
        s, 
        sizeof(CelsEntitySlot), 
        NULL
    );

    if (isMount && slot) {
        slot->world = world;
        slot->entity = ecs_new(world);
        if (name && name[0] != '\0') {
            ecs_set_name(world, slot->entity, name);
        }
        CelsSessionRegisterLifecycle(s, slot, NULL, _cels_entity_cleanup);
    }
    return slot ? slot->entity : 0;
}

#define CEL_Entity(world, name, key) \
    for (int _cels_ent_run = (CelsEnterComposable(CelsGetCurrentSession(), (uint64_t)(key)) ? 1 : 0), _cels_ent_done = 0; \
         !_cels_ent_done; \
         _cels_ent_done = 1, CelsExitGroup(CelsGetCurrentSession())) \
        for ( ; _cels_ent_run; _cels_ent_run = 0) \
            for (ecs_entity_t it = _cels_resolve_entity(CelsGetCurrentSession(), (world), (name), (uint64_t)(key)); \
                 it != 0; \
                 it = 0)

#endif /* _CELS_FLECS_INTEGRATION_DEFINED */
#endif /* Flecs ECS Integration */

#ifdef __cplusplus
}
#endif
