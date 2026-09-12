/*
 * CELS (Composition Entity Lifecycle State)
 * Single-header declarative composition engine for C99.
 *
 * Implements the Jetpack Compose slot table and RememberObserver lifecycle model.
 * Define CELS_IMPLEMENTATION in exactly one .c file before including:
 *   #define CELS_IMPLEMENTATION
 *   #include "cels.h"
 */

#ifndef CELS_H
#define CELS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Configuration Limits                                                      */
/* ========================================================================= */

#ifndef CELS_MAX_DEPTH
#define CELS_MAX_DEPTH 32
#endif

#ifndef CELS_MAX_STATES
#define CELS_MAX_STATES 64
#endif

#ifndef CELS_MAX_WATCHERS
#define CELS_MAX_WATCHERS 8
#endif

#ifndef CELS_MAX_QUEUE
#define CELS_MAX_QUEUE 64
#endif

#ifndef CELS_MAX_GROUPS
#define CELS_MAX_GROUPS 256
#endif

#ifndef CELS_DATA_ARENA_SIZE
#define CELS_DATA_ARENA_SIZE 32768
#endif

#ifndef CELS_MAX_CLEANUPS
#define CELS_MAX_CLEANUPS 128
#endif

#ifndef CELS_MAX_DRAIN_ITERATIONS
#define CELS_MAX_DRAIN_ITERATIONS 8
#endif

#define CELS_SLOT_ALIGNMENT 8
#define CELS_ALIGN_UP(size) (((size) + (CELS_SLOT_ALIGNMENT - 1)) & ~(CELS_SLOT_ALIGNMENT - 1))

/* Every remembered slot occupies at least one aligned arena unit. */
#ifndef CELS_MAX_SLOTS
#define CELS_MAX_SLOTS (CELS_DATA_ARENA_SIZE / CELS_SLOT_ALIGNMENT)
#endif

/* ========================================================================= */
/* Enums & Status Flags                                                      */
/* ========================================================================= */

typedef enum CelsResult {
    CELS_OK                               =  0,
    CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE = -1,
    CELS_ERROR_SLOT_ARENA_OVERFLOW        = -2,
    CELS_ERROR_GROUP_OVERFLOW             = -3,
    CELS_ERROR_QUEUE_OVERFLOW             = -4,
    CELS_ERROR_CLEANUP_OVERFLOW           = -5,
    CELS_ERROR_UNBALANCED_SCOPE           = -6,
    CELS_ERROR_NO_ROOT_COMPOSABLE         = -7,
    CELS_ERROR_STATE_OVERFLOW             = -8
} CelsResult;

#define CELS_FLAG_NONE                 0
#define CELS_FLAG_INVALIDATED          (1 << 0)
#define CELS_FLAG_CONTAINS_INVALIDATED (1 << 1)
#define CELS_FLAG_FRESH_MOUNT          (1 << 2)

/* ========================================================================= */
/* Core Data Types                                                           */
/* ========================================================================= */

typedef struct CelsSession CelsSession;
typedef void (*CelsRootFn)(CelsSession *s);

typedef struct CelsStateHeader {
    uint16_t watcherCount;
    uint64_t watcherKeys[CELS_MAX_WATCHERS];
} CelsStateHeader;

typedef struct CelsStateCell {
    const void      *ptr;
    CelsStateHeader  header;
} CelsStateCell;

#define CEL_State(TypeName) \
    typedef struct TypeName TypeName; \
    struct TypeName

typedef struct CelsObserverDesc {
    size_t size;
    union {
        void (*onRemembered)(void *observer, CelsSession *s);
        void (*onCreate)(void *observer, CelsSession *s);
        void (*OnCreate)(void *observer, CelsSession *s);
    };
    union {
        void (*onForgotten)(void *observer, CelsSession *s);
        void (*onDestroy)(void *observer, CelsSession *s);
        void (*OnDestroy)(void *observer, CelsSession *s);
    };
} CelsObserverDesc;

typedef struct CelsCleanupHook {
    uint64_t groupKey;
    uint32_t groupId;
    void    *instance;
    void   (*onForgotten)(void *instance, CelsSession *s);
} CelsCleanupHook;

typedef struct CelsSlotGroup {
    uint64_t key;
    uint32_t parentIndex;
    uint32_t groupSize;
    uint32_t dataOffset;
    uint16_t dataSize;
    uint16_t flags;
    uint32_t reserved;
} CelsSlotGroup;

/* Stable arena allocations, sorted by arenaOffset. Group IDs survive tree moves. */
typedef struct CelsSlotAllocation {
    uint32_t groupId;
    uint32_t slotOffset;
    uint32_t arenaOffset;
    uint32_t size;
} CelsSlotAllocation;

typedef struct CelsSessionConfig {
    CelsRootFn root;
    uint32_t   maxDrainIterations;
} CelsSessionConfig;

struct CelsSession {
    CelsRootFn root;
    bool       hasComposedOnce;

    uint32_t currentDepth;
    uint32_t currentGroupIndex;
    uint32_t currentSlotOffset;
    uint32_t logicalCursor;

    uint8_t  activeStack[CELS_MAX_DEPTH];
    uint32_t groupIndexStack[CELS_MAX_DEPTH];
    uint32_t oldGroupSizeStack[CELS_MAX_DEPTH];
    uint32_t slotOffsetStack[CELS_MAX_DEPTH];


    /* Dual Gap Buffer: Structural Groups */
    CelsSlotGroup groups[CELS_MAX_GROUPS];
    uint32_t      groupsGapStart;
    uint32_t      groupsGapEnd;

    /* Nonmoving slot arena: remembered pointers stay valid until their group leaves. */
    uint8_t       dataArena[CELS_DATA_ARENA_SIZE];
    /* Used byte count and arena capacity, retained for inspection. */
    uint32_t      dataGapStart;
    uint32_t      dataGapEnd;
    CelsSlotAllocation slots[CELS_MAX_SLOTS];
    uint32_t slotCount;
    uint32_t nextGroupId;

    /* State Registry */
    CelsStateCell states[CELS_MAX_STATES];
    uint32_t      stateCount;

    /* RememberObserver cleanup tracking */
    CelsCleanupHook cleanups[CELS_MAX_CLEANUPS];
    uint32_t        cleanupCount;

    /* Invalidation Queue */
    uint64_t invalidationQueue[CELS_MAX_QUEUE];
    uint32_t queueCount;

    uint32_t maxDrainIterations;
    bool     isRecomposing;
};

/* ========================================================================= */
/* Key Utilities (64-bit FNV-1a Hash)                                        */
/* ========================================================================= */

static inline uint64_t CelsHashKey(const char *str) {
    uint64_t hash = 14695981039346656037ULL;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static inline uint64_t CelsKeyIndex(uint64_t baseKey, uint64_t index) {
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
#define _CEL_GET_MACRO_COMP(_1, _2, _3, _4, NAME, ...) NAME

/* ========================================================================= */
/* Top-Level Lifecycle Definitions                                            */
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

/* ========================================================================= */
/* Syntactically Locked Declarative DSL Macros                               */
/* ========================================================================= */

#define _CEL_COMPOSITION_1(rootKey) \
    do { \
        if (CelsEnterComposition(CelsGetCurrentSession(), (rootKey)))

#define _CEL_COMPOSITION_2(session, rootKey) \
    do { \
        if (CelsEnterComposition((session), (rootKey)))

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

#define _CEL_IS_s_s ~, 1
#define _CEL_IS_s_session ~, 1
#define _CEL_CAT(a, b) a##b
#define _CEL_CHECK_S(token) _CEL_CAT(_CEL_IS_s_, token)
#define _CEL_SECOND(a, b, ...) b
#define _CEL_TEST(x) _CEL_SECOND(x, 0)
#define _CEL_IS_SESSION_ARG(token) _CEL_TEST(_CEL_CHECK_S(token))

/* Legacy 2-arg block with explicit session & CEL_Close pairing */
#define _CEL_COMPOSABLE_LEGACY_BLOCK(session, key) \
    do { \
        if (CelsEnterComposable((session), (key)))

/* Modern Compose container invocation: runs component setup, then allows children in the middle */
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

/* 1-arg container block without component logic, e.g. CEL_Composable(key) { ... } */
#define _CEL_COMPOSABLE_1(key) \
    for (int _cels_run = (CelsEnterComposable(CelsGetCurrentSession(), (key)) ? 1 : 0), _cels_done = 0; \
         !_cels_done; \
         _cels_done = 1, CelsExitGroup(CelsGetCurrentSession())) \
        for ( ; _cels_run; _cels_run = 0)

#define CEL_Composable(...) _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_COMPOSABLE_2, _CEL_COMPOSABLE_1)(__VA_ARGS__)

/* Generic box container for children */
#define CEL_BOX(key) _CEL_COMPOSABLE_1(key)

/* Compose leaf invocation (no body) */
#define CEL_Compose(Component, key) Component(key)

#define _CEL_CLOSE_0() CelsExitGroup(CelsGetCurrentSession())
#define _CEL_CLOSE_1(session) CelsExitGroup((session))
#define _CEL_CLOSE_CHOOSER(...) _CEL_ARG_2(__VA_ARGS__, _CEL_CLOSE_1, _CEL_CLOSE_0)

#define CEL_Close(...) \
        _CEL_CLOSE_CHOOSER(dummy, ##__VA_ARGS__, _CEL_CLOSE_1, _CEL_CLOSE_0)(__VA_ARGS__); \
    } while (0)

#define cel_close(...) CEL_Close(__VA_ARGS__)

#define CEL_Observer(Type) \
    typedef struct Type Type; \
    struct Type

#define _cel_rem_obs_sess(session, Type, on_rem, on_forg) \
    ((Type*)CelsResolveSlot((session), sizeof(Type), NULL, &(CelsObserverDesc){ \
        .size         = sizeof(Type), \
        .OnCreate     = (void(*)(void*, CelsSession*))(on_rem), \
        .OnDestroy    = (void(*)(void*, CelsSession*))(on_forg) \
    }))

#define _cel_rem_obs_curr(Type, on_rem, on_forg) \
    _cel_rem_obs_sess(CelsGetCurrentSession(), Type, on_rem, on_forg)

#define _CEL_REM_OBS_DISPATCH_1(a, Type, on_rem, on_forg) _cel_rem_obs_sess(a, Type, on_rem, on_forg)
#define _CEL_REM_OBS_DISPATCH_0(Type, on_rem, on_forg, ...) _cel_rem_obs_curr(Type, on_rem, on_forg)
#define _CEL_REM_OBS_DISPATCH_2(is_s, ...) _CEL_REM_OBS_DISPATCH_##is_s(__VA_ARGS__)
#define _CEL_REM_OBS_DISPATCH(is_s, ...)   _CEL_REM_OBS_DISPATCH_2(is_s, __VA_ARGS__)

#define cel_remember_observer(...) \
    _CEL_REM_OBS_DISPATCH(_CEL_IS_SESSION_ARG(_CEL_FIRST(__VA_ARGS__)), __VA_ARGS__)

#define cel_observer(...) cel_remember_observer(__VA_ARGS__)

/* ========================================================================= */
/* Persistent Component-Local Memory (cel_remember)                          */
/* ========================================================================= */
/*
 * cel_remember(Type, initialValue...)
 * or with explicit session: cel_remember(session, Type, initialValue...)
 *
 * HOW IT WORKS:
 * 1. Persistent Local State:
 *    cel_remember provides persistent, self-contained local memory for a
 *    composable component across recompositions.
 *
 * 2. Sequential Slot Allocation:
 *    Each call to cel_remember allocates the next slot within the current
 *    component's group in the session data arena. A single component can
 *    declare multiple cel_remember variables in sequential order.
 *
 * 3. Lifecycle Behavior:
 *    - Fresh Mount: Allocates slot memory in the arena and initializes it
 *      with (Type){ initialValue... }.
 *    - Recomposition: Returns the existing persistent memory pointer in the
 *      exact same call order, preserving previous mutations.
 *    - Structural Edits: Slots use a nonmoving arena; inserting, removing, or
 *      reordering other groups never changes a surviving slot's address.
 *    - Pruning / Teardown: When the component leaves the composition tree,
 *      all its remembered slots are automatically reclaimed together.
 *      Pointers must not be used after that group's cleanup completes.
 *      Freed arena ranges are reused without compacting live allocations.
 *
 * 4. Reactive Subscription & Mutation:
 *    - Read & Subscribe: Type val = cel_watch(ptr);
 *    - Mutate: cel_mutate(session, ptr) { (*this)++; }
 *    - Wire into Callbacks: Pass the returned pointer to component callbacks
 *      or event listeners (e.g. button onClick userData).
 */
#define _cel_remember_sess(session, Type, ...) \
    ((Type*)CelsResolveSlot((session), sizeof(Type), &(Type){ __VA_ARGS__ }, NULL))

#define _cel_remember_curr(Type, ...) \
    ((Type*)CelsResolveSlot(CelsGetCurrentSession(), sizeof(Type), &(Type){ __VA_ARGS__ }, NULL))

#define _CEL_REMEMBER_DISPATCH_1(a, Type, ...) _cel_remember_sess(a, Type, __VA_ARGS__)
#define _CEL_REMEMBER_DISPATCH_0(Type, ...)    _cel_remember_curr(Type, __VA_ARGS__)
#define _CEL_REMEMBER_DISPATCH_2(is_s, ...)    _CEL_REMEMBER_DISPATCH_##is_s(__VA_ARGS__)
#define _CEL_REMEMBER_DISPATCH(is_s, ...)      _CEL_REMEMBER_DISPATCH_2(is_s, __VA_ARGS__)

#define _CEL_FIRST(a, ...) a
#define cel_remember(...) _CEL_REMEMBER_DISPATCH(_CEL_IS_SESSION_ARG(_CEL_FIRST(__VA_ARGS__)), __VA_ARGS__)

#define _CEL_WATCH_1(state_ptr) (CelsStateRead(CelsGetCurrentSession(), (state_ptr)), *(state_ptr))
#define _CEL_WATCH_2(session, state_ptr) (CelsStateRead((session), (state_ptr)), *(state_ptr))
#define cel_watch(...) _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_WATCH_2, _CEL_WATCH_1)(__VA_ARGS__)

/* Scoped mutation: infers struct type and exposes this-> */
#define _cel_mutate_2(session, state_ptr) \
    for (__typeof__(*(state_ptr)) _cel_old_ = *(state_ptr), *this = (state_ptr); \
         this != NULL; \
         CelsStateCommitMutation((session), this, &_cel_old_, sizeof(*this)), this = NULL)

#define _cel_mutate_1(state_ptr) \
    _cel_mutate_2(CelsGetCurrentSession(), (state_ptr))

#define cel_mutate(...) _CEL_GET_MACRO_2(__VA_ARGS__, _cel_mutate_2, _cel_mutate_1)(__VA_ARGS__)

/* Query an active observer resource (native handles/observers only) by key */
#define CEL_FindObserver(session, key, Type) \
    ((Type*)CelsFindObserver((session), (key)))

/* ========================================================================= */
/* Compose-Style Component Functions                                         */
/* ========================================================================= */

#define _CEL_COMPOSABLE_DEF(FnName, keyName) \
    static void _cels_body_##FnName(CelsSession *s, uint64_t keyName); \
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
    static void _cels_body_##FnName(CelsSession *s, uint64_t keyName)

#define CEL_Composeable(FnName, keyName)        _CEL_COMPOSABLE_DEF(FnName, keyName)
#define CEL_DefineComposable(FnName, keyName)   _CEL_COMPOSABLE_DEF(FnName, keyName)
#define CEL_ComposableFn(FnName, keyName)       _CEL_COMPOSABLE_DEF(FnName, keyName)
#define CEL_Composable_Def(FnName, keyName)     _CEL_COMPOSABLE_DEF(FnName, keyName)

#define CEL_Composable_Props(FnName, keyName, PropsType, propsName) \
    static void _cels_body_##FnName(CelsSession *s, uint64_t keyName, PropsType propsName); \
    static inline void FnName(uint64_t keyName, PropsType propsName) { \
        CelsSession *s = CelsGetCurrentSession(); \
        assert(s != NULL && #FnName " called outside of an active CelsSession"); \
        if (CelsEnterComposable(s, keyName)) { \
            _cels_body_##FnName(s, keyName, propsName); \
        } \
        CelsExitGroup(s); \
    } \
    static inline void FnName##_s(CelsSession *s, uint64_t keyName, PropsType propsName) { \
        assert(s != NULL && #FnName "_s called with NULL session"); \
        if (CelsEnterComposable(s, keyName)) { \
            _cels_body_##FnName(s, keyName, propsName); \
        } \
        CelsExitGroup(s); \
    } \
    static void _cels_body_##FnName(CelsSession *s, uint64_t keyName, PropsType propsName)

#define CEL_Composeable_Props(FnName, keyName, PropsType, propsName) \
    CEL_Composable_Props(FnName, keyName, PropsType, propsName)
#define CEL_DefineComposable_Props(FnName, keyName, PropsType, propsName) \
    CEL_Composable_Props(FnName, keyName, PropsType, propsName)

/* ========================================================================= */
/* Engine Function Declarations                                              */
/* ========================================================================= */

#ifndef CELS_THREAD_LOCAL
    #if defined(_MSC_VER)
        #define CELS_THREAD_LOCAL __declspec(thread)
    #elif defined(__GNUC__) && !defined(_WIN32)
        #define CELS_THREAD_LOCAL __thread
    #elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__) && !defined(_WIN32)
        #define CELS_THREAD_LOCAL _Thread_local
    #else
        #define CELS_THREAD_LOCAL
    #endif
#endif

CelsSession* CelsGetCurrentSession(void);
void         CelsSetCurrentSession(CelsSession *s);

void        CelsSessionInit(CelsSession *s, const CelsSessionConfig *cfg);
void        CelsSessionSetRoot(CelsSession *s, CelsRootFn rootFn);
void        CelsSessionDestroy(CelsSession *s);
CelsResult  CelsSessionRecompose(CelsSession *s);

bool        CelsEnterComposition(CelsSession *s, uint64_t rootKey);
bool        CelsEnterComposable(CelsSession *s, uint64_t key);
void        CelsExitGroup(CelsSession *s);
void        CelsPruneSubtree(CelsSession *s, uint32_t rootLogicalIndex);
void        CelsPruneSubtreeByKey(CelsSession *s, uint64_t key);

void*       CelsResolveSlot(CelsSession *s, size_t size, const void *initVal, const CelsObserverDesc *desc);
void        CelsStateRead(CelsSession *s, const void *statePtr);
void        CelsStateCommitMutation(CelsSession *s, const void *statePtr, const void *oldVal, size_t size);
void*       CelsFindObserver(CelsSession *s, uint64_t key);

static inline uint32_t CelsGetLogicalGroupCount(const CelsSession *s) {
    return CELS_MAX_GROUPS - (s->groupsGapEnd - s->groupsGapStart);
}

static inline uint32_t CelsGroupLogicalToPhysical(const CelsSession *s, uint32_t logical) {
    return (logical < s->groupsGapStart) ? logical : logical + (s->groupsGapEnd - s->groupsGapStart);
}

static inline uint32_t CelsDataLogicalToPhysical(const CelsSession *s, uint32_t logical) {
    (void)s;
    return logical;
}

static inline CelsSlotGroup* CelsGetGroup(CelsSession *s, uint32_t logical) {
    return &s->groups[CelsGroupLogicalToPhysical(s, logical)];
}

static inline uint8_t* CelsGetData(CelsSession *s, uint32_t logicalOffset) {
    return &s->dataArena[CelsDataLogicalToPhysical(s, logicalOffset)];
}

static inline bool CelsIsFreshMount(CelsSession *s) {
    if (!s || s->currentDepth == 0) return false;
    return (CelsGetGroup(s, s->currentGroupIndex)->flags & CELS_FLAG_FRESH_MOUNT) != 0;
}

#define cel_init if (CelsIsFreshMount(CelsGetCurrentSession()))
#define cel_spawn cel_init
#define cel_once  cel_init

/* ========================================================================= */
/* Flecs ECS Integration (CEL_Entity)                                        */
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
        &(CelsObserverDesc){
            .size = sizeof(CelsEntitySlot),
            .onForgotten = _cels_entity_cleanup
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

#endif /* CELS_H */

/* ========================================================================= */
/* ENGINE IMPLEMENTATION                                                     */
/* ========================================================================= */

#if defined(CELS_IMPLEMENTATION) && !defined(CELS_IMPLEMENTATION_INCLUDED)
#define CELS_IMPLEMENTATION_INCLUDED

static CELS_THREAD_LOCAL CelsSession *cels_current_session = NULL;

CelsSession* CelsGetCurrentSession(void) {
    return cels_current_session;
}

void CelsSetCurrentSession(CelsSession *s) {
    cels_current_session = s;
}

static void CelsMoveGroupGap(CelsSession *s, uint32_t targetLogical) {
    if (targetLogical == s->groupsGapStart) return;

    if (targetLogical < s->groupsGapStart) {
        uint32_t delta = s->groupsGapStart - targetLogical;
        memmove(&s->groups[s->groupsGapEnd - delta],
                &s->groups[targetLogical],
                delta * sizeof(CelsSlotGroup));
        s->groupsGapStart -= delta;
        s->groupsGapEnd   -= delta;
    } else {
        uint32_t delta = targetLogical - s->groupsGapStart;
        memmove(&s->groups[s->groupsGapStart],
                &s->groups[s->groupsGapEnd],
                delta * sizeof(CelsSlotGroup));
        s->groupsGapStart += delta;
        s->groupsGapEnd   += delta;
    }
}

static void CelsUnsubscribeGroupWatchers(CelsSession *s, uint64_t groupKey) {
    for (uint32_t i = 0; i < s->stateCount;) {
        CelsStateHeader *header = &s->states[i].header;
        for (uint16_t w = 0; w < header->watcherCount; ++w) {
            if (header->watcherKeys[w] == groupKey) {
                header->watcherKeys[w] = header->watcherKeys[--header->watcherCount];
                break;
            }
        }
        if (header->watcherCount == 0) {
            s->states[i] = s->states[--s->stateCount];
        } else {
            ++i;
        }
    }
}

static void CelsFireCleanupsForGroup(CelsSession *s, uint32_t groupId) {
    for (uint32_t i = s->cleanupCount; i > 0; --i) {
        uint32_t idx = i - 1;
        if (s->cleanups[idx].groupId == groupId) {
            if (s->cleanups[idx].onForgotten) {
                s->cleanups[idx].onForgotten(s->cleanups[idx].instance, s);
            }
            --s->cleanupCount;
            memmove(&s->cleanups[idx], &s->cleanups[idx + 1],
                    (s->cleanupCount - idx) * sizeof(s->cleanups[0]));
        }
    }
}

static void CelsReleaseSlotsForGroup(CelsSession *s, uint32_t groupId) {
    for (uint32_t i = 0; i < s->slotCount;) {
        CelsSlotAllocation *slot = &s->slots[i];
        if (slot->groupId != groupId) { ++i; continue; }
        uintptr_t first = (uintptr_t)&s->dataArena[slot->arenaOffset];
        uintptr_t end = first + slot->size;
        for (uint32_t state = 0; state < s->stateCount;) {
            uintptr_t ptr = (uintptr_t)s->states[state].ptr;
            if (ptr >= first && ptr < end) {
                s->states[state] = s->states[--s->stateCount];
            } else {
                ++state;
            }
        }
        s->dataGapStart -= slot->size;
        --s->slotCount;
        memmove(slot, slot + 1, (s->slotCount - i) * sizeof(*slot));
    }
}

void CelsPruneSubtree(CelsSession *s, uint32_t rootLogicalIndex) {
    CelsSlotGroup *root = CelsGetGroup(s, rootLogicalIndex);
    uint32_t groupsToRemove = 1 + root->groupSize;
    uint32_t parentIdx = root->parentIndex;

    for (uint32_t i = groupsToRemove; i > 0; --i) {
        uint32_t targetLogical = rootLogicalIndex + (i - 1);
        CelsSlotGroup *g = CelsGetGroup(s, targetLogical);

        CelsFireCleanupsForGroup(s, g->reserved);
        CelsUnsubscribeGroupWatchers(s, g->key);
        CelsReleaseSlotsForGroup(s, g->reserved);
    }
    if (rootLogicalIndex > 0) {
        uint32_t curr = parentIdx;
        while (true) {
            CelsSlotGroup *p = CelsGetGroup(s, curr);
            assert(p->groupSize >= groupsToRemove);
            p->groupSize -= groupsToRemove;

            if (curr == 0) break;
            curr = p->parentIndex;
        }
    }
    uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    CelsMoveGroupGap(s, totalGroups);
    memmove(&s->groups[rootLogicalIndex], &s->groups[rootLogicalIndex + groupsToRemove],
            (totalGroups - rootLogicalIndex - groupsToRemove) * sizeof(s->groups[0]));
    s->groupsGapStart -= groupsToRemove;
    for (uint32_t i = rootLogicalIndex; i < s->groupsGapStart; ++i) {
        if (s->groups[i].parentIndex >= rootLogicalIndex + groupsToRemove) {
            s->groups[i].parentIndex -= groupsToRemove;
        }
    }
}

void CelsPruneSubtreeByKey(CelsSession *s, uint64_t key) {
    if (!s) return;
    uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    for (uint32_t i = 0; i < totalGroups; ++i) {
        if (CelsGetGroup(s, i)->key == key) {
            CelsPruneSubtree(s, i);
            return;
        }
    }
}

static void CelsDrainInvalidationQueue(CelsSession *s) {
    uint32_t totalGroups = CelsGetLogicalGroupCount(s);

    while (s->queueCount > 0) {
        uint64_t targetKey = s->invalidationQueue[--s->queueCount];

        for (uint32_t i = 0; i < totalGroups; ++i) {
            CelsSlotGroup *g = CelsGetGroup(s, i);
            if (g->key == targetKey) {
                g->flags |= CELS_FLAG_INVALIDATED;

                for (uint32_t c = i + 1; c <= i + g->groupSize && c < totalGroups; ++c) {
                    CelsGetGroup(s, c)->flags |= CELS_FLAG_INVALIDATED;
                }

                if (i > 0) {
                    uint32_t curr = g->parentIndex;
                    while (true) {
                        CelsSlotGroup *p = CelsGetGroup(s, curr);
                        p->flags |= CELS_FLAG_CONTAINS_INVALIDATED;
                        if (curr == 0) break;
                        curr = p->parentIndex;
                    }
                }
                break;
            }
        }
    }
}

void CelsSessionInit(CelsSession *s, const CelsSessionConfig *cfg) {
    assert(s != NULL);
    memset(s, 0, sizeof(CelsSession));

    s->root = cfg ? cfg->root : NULL;
    s->hasComposedOnce = false;

    s->groupsGapStart = 0;
    s->groupsGapEnd   = CELS_MAX_GROUPS;

    s->dataGapStart   = 0;
    s->dataGapEnd     = CELS_DATA_ARENA_SIZE;
    s->nextGroupId = 1;

    s->maxDrainIterations = (cfg && cfg->maxDrainIterations > 0)
        ? cfg->maxDrainIterations
        : CELS_MAX_DRAIN_ITERATIONS;
}

void CelsSessionSetRoot(CelsSession *s, CelsRootFn rootFn) {
    assert(s != NULL);
    s->root = rootFn;
}

void CelsSessionDestroy(CelsSession *s) {
    if (!s) return;
    CelsSession *previous = cels_current_session;
    cels_current_session = s;
    if (CelsGetLogicalGroupCount(s) > 0) CelsPruneSubtree(s, 0);
    cels_current_session = previous == s ? NULL : previous;
    memset(s, 0, sizeof(CelsSession));
}

CelsResult CelsSessionRecompose(CelsSession *s) {
    assert(s != NULL);
    if (!s->root) {
        return CELS_ERROR_NO_ROOT_COMPOSABLE;
    }

    if (s->hasComposedOnce && s->queueCount == 0) {
        return CELS_OK;
    }

    CelsSession *prevSession = cels_current_session;
    cels_current_session = s;

    uint32_t iterations = 0;
    s->isRecomposing = true;

    do {
        if (++iterations > s->maxDrainIterations) {
            s->isRecomposing = false;
            cels_current_session = prevSession;
            return CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE;
        }

        CelsDrainInvalidationQueue(s);

        s->currentDepth = 0;
        s->currentSlotOffset = 0;
        s->logicalCursor = 0;

        s->root(s);

        if (s->currentDepth != 0) {
            s->isRecomposing = false;
            if (prevSession) cels_current_session = prevSession;
            return CELS_ERROR_UNBALANCED_SCOPE;
        }

    } while (s->queueCount > 0);

    s->hasComposedOnce = true;
    s->isRecomposing = false;
    if (prevSession) cels_current_session = prevSession;
    return CELS_OK;
}

bool CelsEnterComposition(CelsSession *s, uint64_t rootKey) {
    assert(s->currentDepth == 0 && "CEL_Composition cannot be nested");
    cels_current_session = s;
    uint32_t depth = s->currentDepth++;
    s->activeStack[depth] = 1;

    uint32_t totalGroups = CelsGetLogicalGroupCount(s);

    if (totalGroups == 0) {
        CelsMoveGroupGap(s, 0);
        s->groups[0] = (CelsSlotGroup){
            .key = rootKey,
            .parentIndex = 0,
            .groupSize = 0,
            .dataOffset = 0,
            .dataSize = 0,
            .flags = CELS_FLAG_FRESH_MOUNT,
            .reserved = s->nextGroupId++
        };
        s->groupsGapStart = 1;
    } else {
        CelsSlotGroup *root = CelsGetGroup(s, 0);
        if (root->key != rootKey) {
            CelsPruneSubtree(s, 0);
            --s->currentDepth;
            return CelsEnterComposition(s, rootKey);
        }
    }

    s->currentGroupIndex = 0;
    s->groupIndexStack[depth] = 0;
    s->oldGroupSizeStack[depth] = CelsGetGroup(s, 0)->groupSize;
    s->currentSlotOffset = 0;
    s->logicalCursor = 1;

    return true;
}

bool CelsEnterComposable(CelsSession *s, uint64_t key) {
    assert(s->currentDepth > 0 && "CEL_Composable must be nested within CEL_Composition");
    assert(s->currentDepth < CELS_MAX_DEPTH && "Exceeded CELS_MAX_DEPTH");

    uint32_t depth = s->currentDepth++;
    s->slotOffsetStack[depth - 1] = s->currentSlotOffset;
    uint32_t totalGroups = CelsGetLogicalGroupCount(s);
    uint32_t cursor = s->logicalCursor;
    uint32_t parentIdx = s->groupIndexStack[depth - 1];
    uint32_t parentEnd = parentIdx + 1 + CelsGetGroup(s, parentIdx)->groupSize;
    uint32_t matchIdx = UINT32_MAX;

    /* Match direct siblings only. A keyed subtree can move without being remounted. */
    for (uint32_t i = cursor; i < parentEnd;) {
        CelsSlotGroup *candidate = CelsGetGroup(s, i);
        if (candidate->key == key) { matchIdx = i; break; }
        i += 1 + candidate->groupSize;
    }
    if (matchIdx != UINT32_MAX && matchIdx != cursor) {
        uint32_t movedCount = 1 + CelsGetGroup(s, matchIdx)->groupSize;
        CelsSlotGroup moved[CELS_MAX_GROUPS];
        CelsMoveGroupGap(s, totalGroups);
        memcpy(moved, &s->groups[matchIdx], movedCount * sizeof(moved[0]));
        memmove(&s->groups[cursor + movedCount], &s->groups[cursor],
                (matchIdx - cursor) * sizeof(moved[0]));
        memcpy(&s->groups[cursor], moved, movedCount * sizeof(moved[0]));
        for (uint32_t i = 1; i < totalGroups; ++i) {
            uint32_t parent = s->groups[i].parentIndex;
            if (parent >= matchIdx && parent < matchIdx + movedCount) {
                s->groups[i].parentIndex = cursor + (parent - matchIdx);
            } else if (parent >= cursor && parent < matchIdx) {
                s->groups[i].parentIndex = parent + movedCount;
            }
        }
    }

    if (matchIdx != UINT32_MAX) {
        CelsSlotGroup *cached = CelsGetGroup(s, cursor);

        if (!(cached->flags & (CELS_FLAG_INVALIDATED | CELS_FLAG_CONTAINS_INVALIDATED))) {
            s->activeStack[depth] = 0;
            s->groupIndexStack[depth] = cursor;
            s->logicalCursor += (1 + cached->groupSize);
            return false;
        }

        cached->flags &= ~(CELS_FLAG_INVALIDATED | CELS_FLAG_CONTAINS_INVALIDATED);
        s->activeStack[depth] = 1;
        s->groupIndexStack[depth] = cursor;
        s->oldGroupSizeStack[depth] = cached->groupSize;
        s->currentGroupIndex = cursor;
        s->currentSlotOffset = 0;
        s->logicalCursor++;
        return true;
    }

    assert(totalGroups < CELS_MAX_GROUPS && "CELS_ERROR_GROUP_OVERFLOW");
    assert(s->nextGroupId != 0 && "CELS group identity overflow");
    CelsMoveGroupGap(s, cursor);

    s->groups[s->groupsGapStart] = (CelsSlotGroup){
        .key = key,
        .parentIndex = parentIdx,
        .groupSize = 0,
        .dataOffset = 0,
        .dataSize = 0,
        .flags = CELS_FLAG_FRESH_MOUNT,
        .reserved = s->nextGroupId++
    };
    s->groupsGapStart++;

    for (uint32_t i = cursor + 1; i <= totalGroups; ++i) {
        CelsSlotGroup *group = CelsGetGroup(s, i);
        if (group->parentIndex >= cursor) ++group->parentIndex;
    }
    for (uint32_t ancestor = parentIdx;;) {
        CelsSlotGroup *group = CelsGetGroup(s, ancestor);
        ++group->groupSize;
        if (ancestor == 0) break;
        ancestor = group->parentIndex;
    }

    s->activeStack[depth] = 1;
    s->groupIndexStack[depth] = cursor;
    s->oldGroupSizeStack[depth] = 0;
    s->currentGroupIndex = cursor;
    s->currentSlotOffset = 0;
    s->logicalCursor = cursor + 1;

    return true;
}

void CelsExitGroup(CelsSession *s) {
    assert(s->currentDepth > 0 && "Unmatched CEL_Close call");
    uint32_t depth = --s->currentDepth;
    uint32_t groupIdx = s->groupIndexStack[depth];

    if (s->activeStack[depth]) {
        uint32_t expectedEnd = groupIdx + 1 + CelsGetGroup(s, groupIdx)->groupSize;
        while (s->logicalCursor < expectedEnd && s->logicalCursor < CelsGetLogicalGroupCount(s)) {
            CelsSlotGroup *dead = CelsGetGroup(s, s->logicalCursor);
            uint32_t removed = 1 + dead->groupSize;
            CelsPruneSubtree(s, s->logicalCursor);
            expectedEnd -= removed;
        }

        CelsSlotGroup *g = CelsGetGroup(s, groupIdx);
        if (g->flags & CELS_FLAG_FRESH_MOUNT) {
            g->dataSize = (uint16_t)s->currentSlotOffset;
            g->flags &= ~CELS_FLAG_FRESH_MOUNT;
        }
        assert(g->groupSize == (s->logicalCursor - 1) - groupIdx);
    }

    if (depth > 0) {
        s->currentGroupIndex = s->groupIndexStack[depth - 1];
        s->currentSlotOffset = s->slotOffsetStack[depth - 1];
    }

}

void* CelsResolveSlot(CelsSession *s, size_t size, const void *initVal, const CelsObserverDesc *desc) {
    assert(s->currentDepth > 0);
    CelsSlotGroup *group = CelsGetGroup(s, s->currentGroupIndex);
    size_t alignedSize = CELS_ALIGN_UP(size);
    assert(alignedSize > 0 && alignedSize <= UINT16_MAX);

    if (group->flags & CELS_FLAG_FRESH_MOUNT) {
        assert(s->slotCount < CELS_MAX_SLOTS && "CELS_ERROR_SLOT_ARENA_OVERFLOW");
        uint32_t offset = 0;
        uint32_t insertion = 0;
        while (insertion < s->slotCount) {
            CelsSlotAllocation *next = &s->slots[insertion];
            if (offset + alignedSize <= next->arenaOffset) break;
            offset = next->arenaOffset + next->size;
            ++insertion;
        }
        assert(offset + alignedSize <= CELS_DATA_ARENA_SIZE && "CELS_ERROR_SLOT_ARENA_OVERFLOW");
        memmove(&s->slots[insertion + 1], &s->slots[insertion],
                (s->slotCount - insertion) * sizeof(s->slots[0]));
        s->slots[insertion] = (CelsSlotAllocation){
            .groupId = group->reserved,
            .slotOffset = s->currentSlotOffset,
            .arenaOffset = offset,
            .size = (uint32_t)alignedSize
        };
        ++s->slotCount;
        if (s->currentSlotOffset == 0) group->dataOffset = offset;
        uint8_t *slotPtr = &s->dataArena[offset];
        s->dataGapStart += (uint32_t)alignedSize;

        if (initVal) {
            memcpy(slotPtr, initVal, size);
        } else {
            memset(slotPtr, 0, size);
        }

        if (desc) {
            assert(s->cleanupCount < CELS_MAX_CLEANUPS && "CELS_ERROR_CLEANUP_OVERFLOW");
            s->cleanups[s->cleanupCount++] = (CelsCleanupHook){
                .groupKey = group->key,
                .groupId = group->reserved,
                .instance = slotPtr,
                .onForgotten = desc->onForgotten
            };
            if (desc->onRemembered) {
                desc->onRemembered(slotPtr, s);
            }
        }

        s->currentSlotOffset += (uint32_t)alignedSize;
        return (void*)slotPtr;
    }

    for (uint32_t i = 0; i < s->slotCount; ++i) {
        CelsSlotAllocation *slot = &s->slots[i];
        if (slot->groupId == group->reserved && slot->slotOffset == s->currentSlotOffset) {
            assert(slot->size == alignedSize && "Remembered slot type/order changed");
            s->currentSlotOffset += (uint32_t)alignedSize;
            return &s->dataArena[slot->arenaOffset];
        }
    }
    assert(false && "Remembered slot count/order changed");
    return NULL;
}

static CelsStateHeader* CelsGetOrCreateStateHeader(CelsSession *s, const void *statePtr) {
    for (uint32_t i = 0; i < s->stateCount; ++i) {
        if (s->states[i].ptr == statePtr) {
            return &s->states[i].header;
        }
    }
    assert(s->stateCount < CELS_MAX_STATES && "CELS_ERROR_STATE_OVERFLOW");
    uint32_t idx = s->stateCount++;
    s->states[idx].ptr = statePtr;
    s->states[idx].header.watcherCount = 0;
    return &s->states[idx].header;
}

void CelsStateRead(CelsSession *s, const void *statePtr) {
    if (!s) s = CelsGetCurrentSession();
    if (!s) return;
    assert(statePtr != NULL);

    if (s->currentDepth > 0 && s->activeStack[s->currentDepth - 1]) {
        uint32_t activeGroupIdx = s->groupIndexStack[s->currentDepth - 1];
        uint64_t currentKey = CelsGetGroup(s, activeGroupIdx)->key;

        CelsStateHeader *header = CelsGetOrCreateStateHeader(s, statePtr);

        for (uint16_t i = 0; i < header->watcherCount; ++i) {
            if (header->watcherKeys[i] == currentKey) {
                return;
            }
        }

        if (header->watcherCount < CELS_MAX_WATCHERS) {
            header->watcherKeys[header->watcherCount++] = currentKey;
        }
    } else if (s->currentDepth == 0 && CelsGetLogicalGroupCount(s) > 0) {
        uint64_t currentKey = CelsGetGroup(s, 0)->key;
        CelsStateHeader *header = CelsGetOrCreateStateHeader(s, statePtr);

        for (uint16_t i = 0; i < header->watcherCount; ++i) {
            if (header->watcherKeys[i] == currentKey) {
                return;
            }
        }

        if (header->watcherCount < CELS_MAX_WATCHERS) {
            header->watcherKeys[header->watcherCount++] = currentKey;
        }
    }
}

void CelsStateCommitMutation(CelsSession *s, const void *statePtr, const void *oldVal, size_t size) {
    if (!s) s = CelsGetCurrentSession();
    if (!s) return;
    assert(statePtr != NULL);
    assert(oldVal != NULL);

    if (memcmp(statePtr, oldVal, size) == 0) {
        return;
    }

    CelsStateHeader *header = NULL;
    for (uint32_t i = 0; i < s->stateCount; ++i) {
        if (s->states[i].ptr == statePtr) { header = &s->states[i].header; break; }
    }
    if (!header) return;
    for (uint16_t i = 0; i < header->watcherCount; ++i) {
        if (s->queueCount < CELS_MAX_QUEUE) {
            s->invalidationQueue[s->queueCount++] = header->watcherKeys[i];
        }
    }
}

void* CelsFindObserver(CelsSession *s, uint64_t key) {
    if (!s) return NULL;
    for (uint32_t i = 0; i < s->cleanupCount; ++i) {
        if (s->cleanups[i].groupKey == key) {
            return s->cleanups[i].instance;
        }
    }
    return NULL;
}

#endif /* CELS_IMPLEMENTATION */
