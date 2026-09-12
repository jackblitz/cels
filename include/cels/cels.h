#pragma once

/**
 * @file cels.h
 * @brief Public API and Declarative DSL Macros for the CELS Composition Engine.
 *
 * CELS (Composition Entity Lifecycle State) is a high-performance, cache-aligned
 * declarative composition engine for C99, implementing the Jetpack Compose
 * slot table and lifecycle observer model.
 *
 * Typical usage:
 * @code
 *     CEL_State(WindowState) {
 *         bool isOpen;
 *         int width;
 *     };
 *
 *     static WindowState g_window = { .isOpen = true, .width = 800 };
 *
 *     CEL_Composeable(MyComponent, key) {
 *         int *clickCount = cel_remember(int, 0);
 *         int count = cel_watch(clickCount);
 *         // render widget...
 *     }
 *
 *     void RootApp(CelsSession *s) {
 *         CEL_Composition(s, CEL_KEY("RootWindow")) {
 *             MyComponent(CEL_KEY("MyComp"));
 *         } CEL_Close(s);
 *     }
 * @endcode
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cels/session.h"
#include "cels/slot_table.h"
#include "cels/state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Key Utilities (64-bit FNV-1a Hash)                                        */
/* ========================================================================= */

static inline uint64_t
CelsHashKey(const char *str)
{
    uint64_t hash = 14695981039346656037ULL;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static inline uint64_t
CelsKeyIndex(uint64_t baseKey, uint64_t index)
{
    return baseKey ^ (index * 0x517cc1b727220a95ULL);
}

#define CEL_KEY(str) CelsHashKey(str)
#define CEL_KeyIndex(baseKey, index) CelsKeyIndex((uint64_t)(baseKey), (uint64_t)(index))
#define CEL_AUTO_KEY() (CelsHashKey(__FILE__) ^ ((uint64_t)__LINE__ * 0x517cc1b727220a95ULL))

/* ========================================================================= */
/* Macro Helpers for Overloading & Arity Dispatch                            */
/* ========================================================================= */

#define _CEL_ARG_2(_0, _1, _2, ...) _2
#define _CEL_GET_MACRO_2(_1, _2, NAME, ...) NAME
#define _CEL_GET_MACRO_3(_1, _2, _3, NAME, ...) NAME
#define _CEL_GET_MACRO_4(_1, _2, _3, _4, NAME, ...) NAME
#define _CEL_GET_MACRO_COMP(_1, _2, _3, _4, NAME, ...) NAME
#define _CEL_FIRST(a, ...) a

#define _CEL_IS_s_s ~, 1
#define _CEL_IS_s_session ~, 1
#define _CEL_CAT(a, b) a##b
#define _CEL_CHECK_S(token) _CEL_CAT(_CEL_IS_s_, token)
#define _CEL_SECOND(a, b, ...) b
#define _CEL_TEST(x) _CEL_SECOND(x, 0)
#define _CEL_IS_SESSION_ARG(token) _CEL_TEST(_CEL_CHECK_S(token))

/* ========================================================================= */
/* State Definitions                                                         */
/* ========================================================================= */

#define CEL_State(TypeName) \
    typedef struct TypeName TypeName; \
    struct TypeName

#define CEL_LifecycleState(TypeName) CEL_State(TypeName)
#define CEL_Observer(TypeName)       CEL_State(TypeName)

/* ========================================================================= */
/* Top-Level Lifecycle Definitions                                           */
/* ========================================================================= */

#define CEL_LifeCycle(Name, Type) \
    static void _cels_impl_lifecycle_##Name(Type *it, bool *_cels_alive, int _cels_error_cel_destroy_only_valid_in_CEL_LifeCycle); \
    static inline bool _cels_lifecycle_##Name(void *userData) { \
        bool _cels_alive = true; \
        _cels_impl_lifecycle_##Name((Type*)userData, &_cels_alive, 0); \
        return _cels_alive; \
    } \
    static void _cels_impl_lifecycle_##Name(Type *it, bool *_cels_alive, int _cels_error_cel_destroy_only_valid_in_CEL_LifeCycle)

#define CEL_Lifecycle(Name, Type) CEL_LifeCycle(Name, Type)

#define cel_destroy() \
    do { \
        (void)_cels_error_cel_destroy_only_valid_in_CEL_LifeCycle; \
        *_cels_alive = false; \
        return; \
    } while (0)

#define cel_destory() cel_destroy()

/* ========================================================================= */
/* Composition Lifecycle Attachment (CEL_Attach)                             */
/* ========================================================================= */

#define _CEL_ATTACH_2(Comp, Lifecycle) \
    CelsSessionAttachComposition(CelsGetCurrentSession(), CelsHashKey(#Comp), _cels_body_##Comp, _cels_lifecycle_##Lifecycle, NULL)

#define _CEL_ATTACH_3(sess, Comp, Lifecycle) \
    CelsSessionAttachComposition((CelsSession*)(sess), CelsHashKey(#Comp), _cels_body_##Comp, _cels_lifecycle_##Lifecycle, NULL)

#define _CEL_ATTACH_4(sess, Comp, state_ptr, Lifecycle) \
    CelsSessionAttachComposition((CelsSession*)(sess), CelsHashKey(#Comp), _cels_body_##Comp, _cels_lifecycle_##Lifecycle, (void*)(state_ptr))

#define CEL_Attach(...) \
    _CEL_GET_MACRO_4(__VA_ARGS__, _CEL_ATTACH_4, _CEL_ATTACH_3, _CEL_ATTACH_2)(__VA_ARGS__)

/* ========================================================================= */
/* Declarative Composition Root & Scopes                                     */
/* ========================================================================= */

#define _CEL_COMPOSITION_1(rootKey) \
    do { \
        if (CelsEnterComposition(CelsGetCurrentSession(), (rootKey)))

#define _CEL_COMPOSITION_LEGACY_BLOCK(session, rootKey) \
    do { \
        if (CelsEnterComposition((session), (rootKey)))

#define _CEL_COMPOSITION_DEF(CompName, keyName) \
    static void _cels_body_##CompName(CELS_UNUSED CelsSession *s, CELS_UNUSED uint64_t keyName); \
    static inline void CompName(uint64_t keyName) { \
        CelsSession *s = CelsGetCurrentSession(); \
        assert(s != NULL && #CompName " called outside of an active CelsSession"); \
        if (CelsEnterComposition(s, keyName)) { \
            _cels_body_##CompName(s, keyName); \
        } \
        CelsExitGroup(s); \
    } \
    static inline void CompName##_s(CelsSession *s, uint64_t keyName) { \
        assert(s != NULL && #CompName "_s called with NULL session"); \
        if (CelsEnterComposition(s, keyName)) { \
            _cels_body_##CompName(s, keyName); \
        } \
        CelsExitGroup(s); \
    } \
    static void _cels_body_##CompName(CELS_UNUSED CelsSession *s, CELS_UNUSED uint64_t keyName)

#define _CEL_DISPATCH_COMPOSITION_2(is_sess, a, b) _CEL_DISPATCH_COMPOSITION_IMPL_##is_sess(a, b)
#define _CEL_DISPATCH_COMPOSITION(is_sess, a, b) _CEL_DISPATCH_COMPOSITION_2(is_sess, a, b)
#define _CEL_DISPATCH_COMPOSITION_IMPL_1(a, b) _CEL_COMPOSITION_LEGACY_BLOCK(a, b)
#define _CEL_DISPATCH_COMPOSITION_IMPL_0(a, b) _CEL_COMPOSITION_DEF(a, b)

#define _CEL_COMPOSITION_2(a, b) _CEL_DISPATCH_COMPOSITION(_CEL_IS_SESSION_ARG(a), a, b)

#define _CEL_COMPOSITION_4(Type, key, var, Lifecycle) \
    for (Type *it = (var), *_cels_outer = (Type*)0; \
         !_cels_outer; \
         _cels_outer = (Type*)1) \
        for (uint64_t _cels_k = (key); _cels_k != 0; _cels_k = 0) \
            for (int _cels_ent = CelsEnterComposition(CelsGetCurrentSession(), _cels_k), \
                     _cels_alive = (_cels_ent ? _cels_lifecycle_##Lifecycle(it) : 0), \
                     _cels_run = _cels_alive, \
                     _cels_done = 0; \
                 !_cels_done; \
                 _cels_done = 1, (CelsExitGroup(CelsGetCurrentSession()), \
                                  (!_cels_alive ? CelsPruneSubtreeByKey(CelsGetCurrentSession(), _cels_k) : (void)0))) \
                for ( ; _cels_run; _cels_run = 0)

#define _CEL_COMPOSITION_3(Type, var, Lifecycle) \
    _CEL_COMPOSITION_4(Type, CelsKeyIndex(CelsHashKey(#Type), (uint64_t)(uintptr_t)(var)), var, Lifecycle)

#define CEL_Composition(...) \
    _CEL_GET_MACRO_COMP(__VA_ARGS__, _CEL_COMPOSITION_4, _CEL_COMPOSITION_3, _CEL_COMPOSITION_2, _CEL_COMPOSITION_1)(__VA_ARGS__)

/* ========================================================================= */
/* Composable Nodes & Invocations                                            */
/* ========================================================================= */

#define _CEL_COMPOSABLE_LEGACY_BLOCK(session, key) \
    do { \
        if (CelsEnterComposable((session), (key)))

#define _CEL_COMPOSABLE_COMPONENT_BLOCK(Component, key) \
    for (int _cels_run = (CelsEnterComposable(CelsGetCurrentSession(), (key)) \
                          ? (_cels_body_##Component(CelsGetCurrentSession(), (key)), 1) \
                          : 0), _cels_done = 0; \
         !_cels_done; \
         _cels_done = 1, CelsExitGroup(CelsGetCurrentSession())) \
        for ( ; _cels_run; _cels_run = 0)

#define _CEL_DISPATCH_COMPOSABLE_2(is_sess, a, b) _CEL_DISPATCH_COMPOSABLE_IMPL_##is_sess(a, b)
#define _CEL_DISPATCH_COMPOSABLE(is_sess, a, b) _CEL_DISPATCH_COMPOSABLE_2(is_sess, a, b)
#define _CEL_DISPATCH_COMPOSABLE_IMPL_1(a, b) _CEL_COMPOSABLE_LEGACY_BLOCK(a, b)
#define _CEL_DISPATCH_COMPOSABLE_IMPL_0(a, b) _CEL_COMPOSABLE_COMPONENT_BLOCK(a, b)

#define _CEL_COMPOSABLE_2(a, b) _CEL_DISPATCH_COMPOSABLE(_CEL_IS_SESSION_ARG(a), a, b)

#define _CEL_COMPOSABLE_1(key) \
    for (int _cels_run = (CelsEnterComposable(CelsGetCurrentSession(), (key)) ? 1 : 0), _cels_done = 0; \
         !_cels_done; \
         _cels_done = 1, CelsExitGroup(CelsGetCurrentSession())) \
        for ( ; _cels_run; _cels_run = 0)

#define CEL_Composable(...) _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_COMPOSABLE_2, _CEL_COMPOSABLE_1)(__VA_ARGS__)

#define CEL_BOX(key) _CEL_COMPOSABLE_1(key)
#define CEL_Compose(Component, key) Component(key)

#define _CEL_CLOSE_0() CelsExitGroup(CelsGetCurrentSession())
#define _CEL_CLOSE_1(session) CelsExitGroup((session))
#define _CEL_CLOSE_CHOOSER(...) _CEL_ARG_2(__VA_ARGS__, _CEL_CLOSE_1, _CEL_CLOSE_0)

#define CEL_Close(...) \
        _CEL_CLOSE_CHOOSER(dummy, ##__VA_ARGS__, _CEL_CLOSE_1, _CEL_CLOSE_0)(__VA_ARGS__); \
    } while (0)

#define cel_close(...) CEL_Close(__VA_ARGS__)

/* ========================================================================= */
/* Composable Function Definitions                                           */
/* ========================================================================= */

#ifndef CELS_UNUSED
    #if defined(__GNUC__) || defined(__clang__)
        #define CELS_UNUSED __attribute__((unused))
    #else
        #define CELS_UNUSED
    #endif
#endif

#define _CEL_COMPOSABLE_DEF(FnName, keyName) \
    static void _cels_body_##FnName(CELS_UNUSED CelsSession *s, CELS_UNUSED uint64_t keyName); \
    static inline void FnName(uint64_t keyName) { \
        CelsSession *s = CelsGetCurrentSession(); \
        assert(s != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(s, keyName)) { \
            _cels_body_##FnName(s, keyName); \
        } \
        CelsExitGroup(s); \
    } \
    static inline void FnName##_s(CelsSession *s, uint64_t keyName) { \
        assert(s != NULL && #FnName "_s called with NULL session"); \
        if (CelsEnterComposable(s, keyName)) { \
            _cels_body_##FnName(s, keyName); \
        } \
        CelsExitGroup(s); \
    } \
    static void _cels_body_##FnName(CELS_UNUSED CelsSession *s, CELS_UNUSED uint64_t keyName)

#define CEL_Composeable(FnName, keyName)      _CEL_COMPOSABLE_DEF(FnName, keyName)
#define CEL_DefineComposable(FnName, keyName) _CEL_COMPOSABLE_DEF(FnName, keyName)
#define CEL_ComposableFn(FnName, keyName)     _CEL_COMPOSABLE_DEF(FnName, keyName)
#define CEL_Composable_Def(FnName, keyName)   _CEL_COMPOSABLE_DEF(FnName, keyName)

/* ========================================================================= */
/* Lifecycle State (cel_lifecycle_state) & Observers                         */
/* ========================================================================= */

#define _cel_lifecycle_state_sess(session, Type, on_create, on_destroy) \
    ((Type*)CelsResolveSlot((session), sizeof(Type), NULL, &(CelsLifecycleDesc){ \
        .size      = sizeof(Type), \
        .onCreate  = (void(*)(void*, CelsSession*))(on_create), \
        .onDestroy = (void(*)(void*, CelsSession*))(on_destroy) \
    }))

#define _cel_lifecycle_state_curr(Type, on_create, on_destroy) \
    _cel_lifecycle_state_sess(CelsGetCurrentSession(), Type, on_create, on_destroy)

#define _CEL_LIFECYCLE_STATE_DISPATCH_1(a, Type, on_c, on_d) _cel_lifecycle_state_sess(a, Type, on_c, on_d)
#define _CEL_LIFECYCLE_STATE_DISPATCH_0(Type, on_c, on_d, ...) _cel_lifecycle_state_curr(Type, on_c, on_d)
#define _CEL_LIFECYCLE_STATE_DISPATCH_2(is_s, ...) _CEL_LIFECYCLE_STATE_DISPATCH_##is_s(__VA_ARGS__)
#define _CEL_LIFECYCLE_STATE_DISPATCH(is_s, ...)   _CEL_LIFECYCLE_STATE_DISPATCH_2(is_s, __VA_ARGS__)

#define cel_lifecycle_state(...) \
    _CEL_LIFECYCLE_STATE_DISPATCH(_CEL_IS_SESSION_ARG(_CEL_FIRST(__VA_ARGS__)), __VA_ARGS__)

/* Backwards compatibility aliases */
#define cel_remember_observer(...) cel_lifecycle_state(__VA_ARGS__)
#define cel_observer(...)          cel_lifecycle_state(__VA_ARGS__)

#define CEL_FindLifecycleState(session, key, Type) \
    ((Type*)CelsFindLifecycleState((session), (key)))

#define CEL_FindObserver(session, key, Type) \
    ((Type*)CelsFindObserver((session), (key)))

/* ========================================================================= */
/* Persistent Component Memory (cel_remember)                                */
/* ========================================================================= */

#define _cel_remember_sess(session, Type, ...) \
    ((Type*)CelsResolveSlot((session), sizeof(Type), &(Type){ __VA_ARGS__ }, NULL))

#define _cel_remember_curr(Type, ...) \
    ((Type*)CelsResolveSlot(CelsGetCurrentSession(), sizeof(Type), &(Type){ __VA_ARGS__ }, NULL))

#define _CEL_REMEMBER_DISPATCH_1(a, Type, ...) _cel_remember_sess(a, Type, __VA_ARGS__)
#define _CEL_REMEMBER_DISPATCH_0(Type, ...)    _cel_remember_curr(Type, __VA_ARGS__)
#define _CEL_REMEMBER_DISPATCH_2(is_s, ...)    _CEL_REMEMBER_DISPATCH_##is_s(__VA_ARGS__)
#define _CEL_REMEMBER_DISPATCH(is_s, ...)      _CEL_REMEMBER_DISPATCH_2(is_s, __VA_ARGS__)

#define cel_remember(...) \
    _CEL_REMEMBER_DISPATCH(_CEL_IS_SESSION_ARG(_CEL_FIRST(__VA_ARGS__)), __VA_ARGS__)

/* ========================================================================= */
/* Reactive State Operations: cel_watch & cel_mutate                         */
/* ========================================================================= */

#define _CEL_WATCH_1(state_ptr) (CelsStateRead(CelsGetCurrentSession(), (state_ptr)), *(state_ptr))
#define _CEL_WATCH_2(session, state_ptr) (CelsStateRead((session), (state_ptr)), *(state_ptr))
#define cel_watch(...) _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_WATCH_2, _CEL_WATCH_1)(__VA_ARGS__)

#define _cel_mutate_2(session, state_ptr) \
    for (__typeof__(*(state_ptr)) _cel_old_ = *(state_ptr), *this = (state_ptr); \
         this != NULL; \
         CelsStateCommitMutation((session), this, &_cel_old_, sizeof(*this)), this = NULL)

#define _cel_mutate_1(state_ptr) \
    _cel_mutate_2(CelsGetCurrentSession(), (state_ptr))

#define cel_mutate(...) _CEL_GET_MACRO_2(__VA_ARGS__, _cel_mutate_2, _cel_mutate_1)(__VA_ARGS__)

#define cel_init if (CelsIsFreshMount(CelsGetCurrentSession()))
#define cel_spawn cel_init
#define cel_once  cel_init

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
        NULL, 
        &(CelsLifecycleDesc){
            .size = sizeof(CelsEntitySlot),
            .onDestroy = _cels_entity_cleanup
        }
    );

    if (isMount && slot) {
        slot->world = world;
        slot->entity = ecs_new(world);
        if (name && name[0] != '\0') {
            ecs_set_name(world, slot->entity, name);
        }
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
