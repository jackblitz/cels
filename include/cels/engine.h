#pragma once

/**
 * @file engine.h
 * @brief Host Engine lifecycle, system/engine memory modules, and execution coordinator.
 *
 * CelsEngine represents the host executable (.exe) runtime. It owns:
 * 1. Subsystem memory modules (CEL_Module): SDL windowing, Vulkan device/swapchain,
 *    Flecs ECS entity worlds, and Audio streams. These survive DLL reloads.
 * 2. The reactive composition slot session (CelsSession) with its L1 cache slab.
 * 3. The main loop and dynamic application (.dll) loader / hot-reloader.
 *
 * Typical usage:
 * @code
 *     CelsEngine engine;
 *     CelsResult result = CelsEngineInit(&engine, "my_app");
 *     if (result != CELS_OK) {
 *         fprintf(stderr, "Engine init failed: %s\n", CelsResultToString(result));
 *         return result;
 *     }
 *
 *     // Register host subsystem modules before or during tick
 *     MyRenderer renderer = { ... };
 *     CEL_RegisterModule(&engine, MyRenderer, &renderer);
 *
 *     while (!engine.shouldQuit) {
 *         CelsEnginePollReload(&engine);
 *         CelsEngineRecompose(&engine);
 *     }
 *
 *     CelsEngineDestroy(&engine);
 * @endcode
 *
 * Thread safety: CelsEngine functions operate on the primary engine thread.
 * Subsystem modules registered via CelsEngineRegisterModule must synchronize
 * their own access if accessed concurrently by background worker threads.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cels/runtime/session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Engine Magic & Limits                                                     */
/* ========================================================================= */

#define CELS_ENGINE_MAGIC 0x454E4731u /* 'ENG1' */

#ifndef CELS_MAX_MODULES
#define CELS_MAX_MODULES 16u
#endif

/* Forward declarations */
struct CelsEngine;
struct CelsAppManifest;
struct CelsAppModule;

/* ========================================================================= */
/* Engine Module Binding Record                                              */
/* ========================================================================= */

#ifndef CELS_MODULE_BINDING_DEFINED
#define CELS_MODULE_BINDING_DEFINED
/**
 * Record tracking an active engine subsystem module registered with the engine.
 */
typedef struct CelsModuleBinding {
    uint64_t key;                      /**< 64-bit FNV-1a hash of module type name */
    const char *name;                  /**< Human-readable name for diagnostics */
    void *instance;                    /**< Pointer to developer's module struct */
    void (*onDestroy)(void *instance); /**< Optional cleanup callback */
} CelsModuleBinding;
#endif

/* ========================================================================= */
/* Macro Dispatch Helpers                                                    */
/* ========================================================================= */

#ifndef _CEL_GET_MACRO_2
#define _CEL_GET_MACRO_2(_1, _2, NAME, ...) NAME
#endif

#ifndef _CEL_GET_MACRO_3
#define _CEL_GET_MACRO_3(_1, _2, _3, NAME, ...) NAME
#endif

/* ========================================================================= */
/* Engine Instance (CelsEngine)                                              */
/* ========================================================================= */

/**
 * Engine runtime context residing in the executable (.exe).
 * Owns system memory modules and the primary reactive session.
 */
struct CelsEngine {
    uint32_t                      magic;        /**< CELS_ENGINE_MAGIC validation tag */
    const struct CelsAppManifest *manifest;     /**< Application manifest from DLL or static */
    struct CelsAppModule         *appModule;    /**< Dynamic application handle in Hot-Reload mode */
    CelsModuleBinding             modules[CELS_MAX_MODULES]; /**< Subsystem module bindings */
    uint32_t                      moduleCount;  /**< Number of active module bindings */
    CelsSession                   session;      /**< Primary reactive session */
    bool                          isStarted;    /**< True if engine and app are active */
    bool                          shouldQuit;   /**< True if exit has been requested */
};

/* ========================================================================= */
/* Engine Lifecycle API                                                      */
/* ========================================================================= */

/**
 * Internal initializer for CelsEngine.
 *
 * @param engine  Target host engine. Non-NULL.
 * @param appName Application target name, or NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult _CelsEngineInitInternal(CelsEngine *engine, const char *appName);

/**
 * Internal initializer for CelsEngine with explicit session configuration.
 *
 * @param engine  Target host engine. Non-NULL.
 * @param appName Application target name, or NULL.
 * @param config  Optional session configuration, or NULL for defaults.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult _CelsEngineInitInternalWithOptions(CelsEngine *engine, const char *appName, const CelsSessionConfig *config);

/**
 * Tears down a host engine, releasing modules, sessions, and dynamically loaded libraries.
 *
 * @param engine Target host engine. Safe if NULL.
 */
void CelsEngineDestroy(CelsEngine *engine);

/**
 * Starts execution of an engine instance and performs initial composition mount.
 *
 * @param engine Target host engine. Non-NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsEngineStart(CelsEngine *engine);

/**
 * Stops execution of an engine instance and invokes teardown hooks.
 *
 * @param engine Target host engine. Safe if NULL.
 */
void CelsEngineEnd(CelsEngine *engine);

/**
 * Triggers a recomposition pass on the engine's primary session and all active sessions.
 *
 * @param engine Target host engine. Non-NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsEngineRecompose(CelsEngine *engine);

/**
 * Initializes the engine with explicit session configuration and loads the application module.
 *
 * What it does:
 * - Initializes engine context, thread-local binding, and memory modules.
 * - Initializes the primary reactive session using the specified CelsSessionConfig
 *   (e.g., custom slab size, workload profile, or composable count).
 * - Binds and mounts the application module.
 *
 * Expected outcome:
 * - Engine initialized with CELS_OK, ready for the main tick loop.
 *
 * Where to use:
 * - Host executables customizing session slab memory, capacity profiles, or host engines.
 *
 * @param engine  Target host engine. Non-NULL.
 * @param appName Application module name. If NULL, defaults to CELS_APP_TARGET.
 * @param config  Session configuration (slab, profile, maxComposables), or NULL for defaults.
 * @return CELS_OK on success, or CelsResult error code.
 */
static inline CelsResult CelsEngineInitWithOptions(CelsEngine *engine, const char *appName, const CelsSessionConfig *config)
{
#if defined(CELS_HOT_RELOAD) && !CELS_HOT_RELOAD
    (void)appName;
    CelsResult res = _CelsEngineInitInternalWithOptions(engine, NULL, config);
    if (res != CELS_OK) {
        return res;
    }
    extern const struct CelsAppManifest *CelsGetAppManifest(void);
    const struct CelsAppManifest *manifest = CelsGetAppManifest();
    if (manifest != NULL) {
        engine->manifest = manifest;
        return CelsEngineStart(engine);
    }
    return CELS_OK;
#else
#if defined(CELS_APP_TARGET)
    if (appName == NULL || appName[0] == '\0') {
        appName = CELS_APP_TARGET;
    }
#endif
    return _CelsEngineInitInternalWithOptions(engine, appName, config);
#endif
}

/**
 * Initializes the engine and loads the application module with a named capacity profile.
 *
 * What it does:
 * - Dimensions the primary reactive session memory slab using the target CelsSessionProfile.
 * - Automatically configures optimal group/slot capacities and cache line alignment.
 *
 * Expected outcome:
 * - Engine initialized with the chosen profile slab (e.g. 128 KiB for CELS_PROFILE_1K).
 *
 * Where to use:
 * - Host applications selecting an intent-driven workload capacity profile.
 *
 * @param engine  Target host engine. Non-NULL.
 * @param appName Application module name. If NULL, defaults to CELS_APP_TARGET.
 * @param profile Workload capacity profile (e.g. CELS_PROFILE_1K, CELS_PROFILE_512).
 * @return CELS_OK on success, or CelsResult error code.
 */
static inline CelsResult CelsEngineInitWithProfile(CelsEngine *engine, const char *appName, CelsSessionProfile profile)
{
    CelsSessionConfig cfg = CelsSessionProfileConfig(profile);
    return CelsEngineInitWithOptions(engine, appName, &cfg);
}

/**
 * Initializes the engine and loads the application module.
 *
 * Discovers and binds the application module (<appName>.dll in Debug, static in Release),
 * initializes the session memory slab, and mounts the root composition.
 *
 * @param engine  Target host engine. Non-NULL.
 * @param appName Application module name. If NULL, defaults to CELS_APP_TARGET.
 * @return CELS_OK on success, or CelsResult error code.
 */
static inline CelsResult CelsEngineInit(CelsEngine *engine, const char *appName)
{
    return CelsEngineInitWithOptions(engine, appName, NULL);
}

/**
 * Resolves and loads an application module into the engine.
 *
 * In dynamic hot-reload mode (Debug), automatically discovers the application
 * library (<appName>.dll / .so / .dylib) co-located with the running host executable,
 * creates a shadow copy to bypass file locks, loads symbols, initializes session,
 * and performs the initial composition mount.
 *
 * In monolithic mode (Release), statically binds the compiled-in application manifest
 * and starts the engine.
 *
 * @param engine  Target host engine. Non-NULL.
 * @param appName Application module target name (e.g. "engine_app"). If NULL, resolves via default target.
 * @return CELS_OK on success, or CelsResult error code.
 */
#if defined(CELS_HOT_RELOAD) && (CELS_HOT_RELOAD == 0)
static inline CelsResult CelsEngineLoadApp(CelsEngine *engine, const char *appName)
{
    (void)appName;
    if (engine == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    extern const struct CelsAppManifest *CelsGetAppManifest(void);
    engine->manifest = CelsGetAppManifest();
    return CelsEngineStart(engine);
}

/**
 * Checks for runtime code reload (no-op in monolithic Release mode).
 *
 * @param engine Target host engine. Safe if NULL.
 * @return Always false in monolithic release mode.
 */
static inline bool CelsAppRuntimeCheck(CelsEngine *engine)
{
    (void)engine;
    return false;
}
#else
CelsResult CelsEngineLoadApp(CelsEngine *engine, const char *appName);
bool       CelsAppRuntimeCheck(CelsEngine *engine);
#endif

#define cels_app_runtime_check CelsAppRuntimeCheck
#define CelsEnginePollReload CelsAppRuntimeCheck
#define cels_engine_poll_reload CelsAppRuntimeCheck

/**
 * Registers an engine subsystem module binding with the host engine.
 *
 * @param engine    Target host engine. Non-NULL.
 * @param key       Unique 64-bit module type key.
 * @param name      Human-readable module identifier for diagnostics. Non-NULL.
 * @param instance  Pointer to developer-allocated module struct. Non-NULL.
 * @param onDestroy Cleanup callback invoked when engine shuts down. Safe if NULL.
 */
void CelsEngineRegisterModule(CelsEngine *engine,
                              uint64_t key,
                              const char *name,
                              void *instance,
                              void (*onDestroy)(void *instance));

/**
 * Retrieves a registered subsystem module pointer by its 64-bit key from the engine.
 *
 * @param engine Target host engine. Safe if NULL.
 * @param key    Unique 64-bit module type key.
 * @return Pointer to module struct instance, or NULL if not found.
 */
void *CelsEngineGetModule(const CelsEngine *engine, uint64_t key);

/**
 * Returns the currently active ambient host engine bound to the calling thread.
 *
 * @return Pointer to current CelsEngine, or NULL if none is active.
 */
CelsEngine *CelsGetCurrentEngine(void);

/**
 * Sets the active ambient host engine bound to the calling thread.
 *
 * @param engine Target host engine. Safe if NULL.
 */
void CelsSetCurrentEngine(CelsEngine *engine);

/**
 * Requests that the host engine terminate its tick loop.
 *
 * @param engine Target engine. If NULL, targets CelsGetCurrentEngine().
 */
static inline void CelsEngineQuit(CelsEngine *engine)
{
    if (engine == NULL) {
        engine = CelsGetCurrentEngine();
    }
    if (engine != NULL) {
        engine->shouldQuit = true;
    }
}

/**
 * Executes a standalone monolithic application lifecycle using the provided manifest.
 *
 * @param manifest Application manifest descriptor. Non-NULL.
 * @param config   Optional session configuration, or NULL for defaults.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsEngineRunStandalone(const struct CelsAppManifest *manifest, const CelsSessionConfig *config);

/* ========================================================================= */
/* Module Access Dispatch Helpers                                            */
/* ========================================================================= */

/**
 * Resolves a subsystem module pointer from context or ambient engine/session.
 *
 * @param ctx Context pointer (CelsEngine* or CelsSession*), or NULL for ambient.
 * @param key Unique 64-bit module type key.
 * @return Resolved module pointer, or NULL if not found.
 */
void *_cels_resolve_module(const void *ctx, uint64_t key);

/**
 * Dispatches module registration to the target engine or session context.
 *
 * @param ctx        Target context (CelsEngine* or CelsSession*), or NULL for ambient.
 * @param key        Unique 64-bit module type key.
 * @param name       Human-readable module identifier for diagnostics. Non-NULL.
 * @param inst       Pointer to developer-allocated module struct. Non-NULL.
 * @param on_destroy Optional cleanup callback. Safe if NULL.
 */
void _cels_dispatch_register_module(void *ctx, uint64_t key, const char *name, void *inst, void (*on_destroy)(void*));

#define _CEL_REG_MODULE_3(target, Type, ptr) \
    _cels_dispatch_register_module((void*)(target), CelsHashKey(#Type), #Type, (void*)(ptr), NULL)

#define _CEL_REG_MODULE_2(Type, ptr) \
    _cels_dispatch_register_module(NULL, CelsHashKey(#Type), #Type, (void*)(ptr), NULL)

#define _CEL_REG_MODULE_4(target, Type, ptr, on_destroy) \
    _cels_dispatch_register_module((void*)(target), CelsHashKey(#Type), #Type, (void*)(ptr), (on_destroy))

#define CEL_RegisterModule(...) \
    _CEL_GET_MACRO_3(__VA_ARGS__, _CEL_REG_MODULE_3, _CEL_REG_MODULE_2)(__VA_ARGS__)

#define CEL_RegisterModuleWithHooks(target, Type, ptr, on_reload, on_destroy) \
    _cels_dispatch_register_module((void*)(target), CelsHashKey(#Type), #Type, (void*)(ptr), (on_destroy))

#define _CEL_GET_MOD_2(ctx, Type) \
    ((Type*)_cels_resolve_module((const void*)(ctx), CelsHashKey(#Type)))

#define _CEL_GET_MOD_1(Type) \
    ((Type*)_cels_resolve_module(NULL, CelsHashKey(#Type)))

#define CEL_GetModule(...) \
    _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_GET_MOD_2, _CEL_GET_MOD_1)(__VA_ARGS__)

#define cel_get_module(Type) CEL_GetModule(Type)
#define cel_engine_get(Type) CEL_GetModule(Type)

/* ========================================================================= */
/* Compatibility Aliases                                                     */
/* ========================================================================= */

#define CELS_APP_MAGIC            CELS_ENGINE_MAGIC
#define CelsAppInit               CelsEngineInit
#define CelsAppDestroy            CelsEngineDestroy
#define CelsAppStart              CelsEngineStart
#define CelsAppEnd                CelsEngineEnd
#define CelsAppRegisterModule     CelsEngineRegisterModule
#define CelsAppGetModule          CelsEngineGetModule
#define CelsGetCurrentApp         CelsGetCurrentEngine
#define CelsSetCurrentApp         CelsSetCurrentEngine
#define CelsAppRunStandalone      CelsEngineRunStandalone

#ifdef __cplusplus
}
#endif
