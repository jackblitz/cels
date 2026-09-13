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
