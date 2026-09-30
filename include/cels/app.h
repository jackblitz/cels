#pragma once

/**
 * @file app.h
 * @brief Application module declaration (CEL_App) and manifest interface for CELS.
 *
 * App level represents the application module (.dll / .so or static module).
 * Applications declare their composition tree, widgets, and logic using CEL_App.
 * Applications access engine-level subsystem memory (SDL, Flecs, Vulkan, Audio)
 * hosted in the executable (CelsEngine) via CEL_GetModule(ModuleType).
 *
 * Typical usage:
 * @code
 *     // 1. Declare root composition in application module
 *     CEL_Composition(GameRoot) {
 *         // Compose UI, game state, widgets
 *     }
 *
 *     // 2. Direct declarative root attachment (Option 1):
 *     CEL_App(MyGame, GameRoot);
 *
 *     // Or with lifecycle evaluation predicate:
 *     // CEL_App(MyGame, GameRoot, GameEval);
 * @endcode
 *
 * Thread safety: Application lifecycle functions (onStart, onEnd) and root
 * compositions run on the primary session composition thread.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cels/runtime/session.h"
#include "cels/engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CELS_UNUSED
    #if defined(__GNUC__) || defined(__clang__)
        #define CELS_UNUSED __attribute__((unused))
    #else
        #define CELS_UNUSED
    #endif
#endif

/**
 * @def CEL_OnStart
 * @brief Declares an application startup lifecycle callback without unused parameter warnings.
 *
 * Injects `engine` (CelsEngine*) and `session` (CelsSession*) parameters decorated with
 * CELS_UNUSED so that either or both can be used without compiler warnings.
 *
 * Example:
 * @code
 *     CEL_OnStart(App_OnStart) {
 *         cel_attach(session, MainWindow);
 *     }
 * @endcode
 */
#define CEL_OnStart(Name) \
    static void Name(CelsEngine *engine CELS_UNUSED, CelsSession *session CELS_UNUSED)

/**
 * @def CEL_OnEnd
 * @brief Declares an application teardown lifecycle callback without unused parameter warnings.
 *
 * Injects `engine` (CelsEngine*) and `session` (CelsSession*) parameters decorated with
 * CELS_UNUSED so that cleanups can be run without compiler warnings.
 *
 * Example:
 * @code
 *     CEL_OnEnd(App_OnEnd) {
 *         // Teardown application resources
 *     }
 * @endcode
 */
#define CEL_OnEnd(Name) \
    static void Name(CelsEngine *engine CELS_UNUSED, CelsSession *session CELS_UNUSED)

/* ========================================================================= */
/* Symbol Visibility & Export Macro                                          */
/* ========================================================================= */

#if defined(_WIN32) || defined(__CYGWIN__)
    #define CELS_APP_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
    #define CELS_APP_EXPORT __attribute__((visibility("default")))
#else
    #define CELS_APP_EXPORT
#endif

#define CELS_APP_ENTRY_SYMBOL "CelsGetAppManifest"

/* ========================================================================= */
/* Application Manifest & Lifecycle Types (Defined in .dll / module)         */
/* ========================================================================= */

/**
 * Application lifecycle callbacks and metadata defined in the reloadable module.
 */
typedef struct CelsAppManifest {
    uint32_t version;                               /**< Manifest version (defaults to 1) */
    const char *name;                               /**< Application identifier / display name */
    bool continuousCompose;                         /**< If true, recomposes continuously (game mode); if false, event-driven (app mode) */
    size_t slabSize;                                /**< Optional requested slab size (e.g. CELS_SLAB_256K). Defaults to 0 (uses engine default) */
    uint32_t maxGroups;                             /**< Optional requested max groups. If 0, auto-calculated from slabSize */
    void (*setSession)(CelsSession *s);             /**< Internal session synchronization across DLL boundary */
    void (*onStart)(CelsEngine *engine, CelsSession *session); /**< Setup callback: attach compositions via cel_attach */
    void (*onEnd)(CelsEngine *engine, CelsSession *session);   /**< Teardown callback on shutdown */
    void (*onPrintTree)(const CelsSession *session); /**< Optional tree inspection callback */
} CelsAppManifest;

typedef const CelsAppManifest *(*CelsAppEntryFn)(void);

/**
 * Application manifest entry point. Exported from .dll in Debug mode,
 * or linked statically in Release standalone mode.
 *
 * @return Pointer to application manifest structure. Never NULL.
 */
CELS_APP_EXPORT const CelsAppManifest *CelsGetAppManifest(void);

/* ========================================================================= */
/* Declarative App Registration Macro (CEL_App)                              */
/* ========================================================================= */

#ifndef NUCLEUS_DEFAULT_CONTINUOUS_COMPOSE
    #define NUCLEUS_DEFAULT_CONTINUOUS_COMPOSE false
#endif

#define _CEL_APP_2(AppName, Comp) \
    static void _cels_app_set_session_##AppName(CelsSession *s) { \
        CelsSetCurrentSession(s); \
    } \
    static void _cels_app_onstart_##AppName(CelsEngine *_cels_eng CELS_UNUSED, CelsSession *_cels_sess CELS_UNUSED) { \
        CelsSessionAttachComposition((_cels_sess), CelsHashKey(#Comp), (void(*)(void*))(Comp), NULL, NULL, NULL); \
    } \
    static const CelsAppManifest _cels_app_manifest_##AppName = { \
        .version = 1, \
        .name = #AppName, \
        .continuousCompose = NUCLEUS_DEFAULT_CONTINUOUS_COMPOSE, \
        .setSession = _cels_app_set_session_##AppName, \
        .onStart = _cels_app_onstart_##AppName, \
        .onEnd = NULL, \
        .onPrintTree = NULL \
    }; \
    CELS_APP_EXPORT const CelsAppManifest *CelsGetAppManifest(void) { \
        return &_cels_app_manifest_##AppName; \
    } \
    typedef int _cels_app_semicolon_swallower_##AppName

#define _CEL_APP_3(AppName, Comp, Eval) \
    static void _cels_app_set_session_##AppName(CelsSession *s) { \
        CelsSetCurrentSession(s); \
    } \
    static void _cels_app_onstart_##AppName(CelsEngine *_cels_eng CELS_UNUSED, CelsSession *_cels_sess CELS_UNUSED) { \
        CelsSessionAttachComposition((_cels_sess), CelsHashKey(#Comp), (void(*)(void*))(Comp), NULL, (bool(*)(void*))(Eval), NULL); \
    } \
    static const CelsAppManifest _cels_app_manifest_##AppName = { \
        .version = 1, \
        .name = #AppName, \
        .continuousCompose = NUCLEUS_DEFAULT_CONTINUOUS_COMPOSE, \
        .setSession = _cels_app_set_session_##AppName, \
        .onStart = _cels_app_onstart_##AppName, \
        .onEnd = NULL, \
        .onPrintTree = NULL \
    }; \
    CELS_APP_EXPORT const CelsAppManifest *CelsGetAppManifest(void) { \
        return &_cels_app_manifest_##AppName; \
    } \
    typedef int _cels_app_semicolon_swallower_##AppName

#define _CEL_APP_4(AppName, Comp, Eval, UserData) \
    static void _cels_app_set_session_##AppName(CelsSession *s) { \
        CelsSetCurrentSession(s); \
    } \
    static void _cels_app_onstart_##AppName(CelsEngine *_cels_eng CELS_UNUSED, CelsSession *_cels_sess CELS_UNUSED) { \
        CelsSessionAttachComposition((_cels_sess), CelsHashKey(#Comp), (void(*)(void*))(Comp), (void*)(UserData), (bool(*)(void*))(Eval), NULL); \
    } \
    static const CelsAppManifest _cels_app_manifest_##AppName = { \
        .version = 1, \
        .name = #AppName, \
        .continuousCompose = NUCLEUS_DEFAULT_CONTINUOUS_COMPOSE, \
        .setSession = _cels_app_set_session_##AppName, \
        .onStart = _cels_app_onstart_##AppName, \
        .onEnd = NULL, \
        .onPrintTree = NULL \
    }; \
    CELS_APP_EXPORT const CelsAppManifest *CelsGetAppManifest(void) { \
        return &_cels_app_manifest_##AppName; \
    } \
    typedef int _cels_app_semicolon_swallower_##AppName

/**
 * Declares an application manifest with raw struct initialization / hooks.
 * Use when custom lifecycle callbacks (onEnd, continuousCompose) are required.
 *
 * Example:
 * @code
 *     CEL_App_Manifest(MyGame,
 *         .onStart = OnStart,
 *         .onEnd = OnEnd,
 *         .continuousCompose = true
 *     );
 * @endcode
 */
#define CEL_App_Manifest(AppName, ...) \
    static void _cels_app_set_session_##AppName(CelsSession *s) { \
        CelsSetCurrentSession(s); \
    } \
    static const CelsAppManifest _cels_app_manifest_##AppName = { \
        .version = 1, \
        .name = #AppName, \
        .continuousCompose = NUCLEUS_DEFAULT_CONTINUOUS_COMPOSE, \
        .setSession = _cels_app_set_session_##AppName, \
        __VA_ARGS__ \
    }; \
    CELS_APP_EXPORT const CelsAppManifest *CelsGetAppManifest(void) { \
        return &_cels_app_manifest_##AppName; \
    } \
    typedef int _cels_app_semicolon_swallower_##AppName

#define _CEL_APP_GET_MACRO(_1, _2, _3, _4, NAME, ...) NAME

/**
 * Declarative Application Module Definition (Option 1).
 *
 * Attaches the root composition directly into the engine session upon loading,
 * eliminating the need for out-of-line onStart boilerplate.
 *
 * Overloads:
 * - CEL_App(AppName, RootComp)
 * - CEL_App(AppName, RootComp, EvalPred)
 * - CEL_App(AppName, RootComp, EvalPred, UserData)
 *
 * Example:
 * @code
 *     CEL_App(WindowApp, WindowComposition);
 *     // or with evaluation:
 *     CEL_App(WindowApp, WindowComposition, WindowEval);
 * @endcode
 */
#define CEL_App(...) \
    _CEL_APP_GET_MACRO(__VA_ARGS__, _CEL_APP_4, _CEL_APP_3, _CEL_APP_2)(__VA_ARGS__)

#define CELS_APP(...) CEL_App(__VA_ARGS__)
#define CELS_APP_MANIFEST(AppName, ...) CEL_App_Manifest(AppName, __VA_ARGS__)

/* ========================================================================= */
/* Standalone Execution Helpers                                              */
/* ========================================================================= */

/**
 * Generates an int main() entry point for turnkey standalone execution.
 */
#define CEL_APP_MAIN(AppName) \
    int main(int argc, char **argv) { \
        (void)argc; (void)argv; \
        return (CelsEngineRunStandalone(&_cels_app_manifest_##AppName, NULL) == CELS_OK) ? 0 : 1; \
    }

#ifdef __cplusplus
}
#endif
