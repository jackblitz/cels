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
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cels/session.h"

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
    CelsModuleBinding             modules[CELS_MAX_MODULES];
    uint32_t                      moduleCount;
    CelsSession                   session;      /**< Primary reactive session */
    bool                          isStarted;
    bool                          shouldQuit;
};

/* Engine lifecycle */
void       CelsEngineInit(CelsEngine *engine, const struct CelsAppManifest *manifest, const CelsSessionConfig *config);
void       CelsEngineDestroy(CelsEngine *engine);

CelsResult CelsEngineStart(CelsEngine *engine);
void       CelsEngineEnd(CelsEngine *engine);

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

void       CelsEngineRegisterModule(CelsEngine *engine,
                                    uint64_t key,
                                    const char *name,
                                    void *instance,
                                    void (*onDestroy)(void *instance));

void      *CelsEngineGetModule(const CelsEngine *engine, uint64_t key);

CelsEngine *CelsGetCurrentEngine(void);
void        CelsSetCurrentEngine(CelsEngine *engine);

CelsResult CelsEngineRunStandalone(const struct CelsAppManifest *manifest, const CelsSessionConfig *config);

/* ========================================================================= */
/* Module Access Dispatch Helpers                                            */
/* ========================================================================= */

void *_cels_resolve_module(const void *ctx, uint64_t key);
void  _cels_dispatch_register_module(void *ctx, uint64_t key, const char *name, void *inst, void (*on_destroy)(void*));

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
