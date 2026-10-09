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
/** @deprecated Legacy entry point symbol preserved for backwards compatibility with pre-v0.5 binaries. */
#define CELS_APP_LEGACY_ENTRY_SYMBOL "CelsGetAppDef"

/* ========================================================================= */
/* Application Manifest & Lifecycle Types (Defined in .dll / module)         */
/* ========================================================================= */

/**
 * Application manifest containing metadata and lifecycle callbacks exported by the application module.
 */
struct CelsAppManifest {
    uint32_t     version;                /**< Manifest ABI version (defaults to 1) */
    const char  *name;                   /**< Unique application target identifier */
    const char  *title;                  /**< Human-readable window title / display name */
    const char  *appVersion;             /**< Semantic version string (e.g. "0.5.0") */
    void       (*setSession)(CelsSession *s); /**< Internal session synchronization across DLL boundary */
    void       (*onStart)(CelsSession *session);  /**< Setup: attaches root compositions */
    void       (*onReload)(CelsSession *session); /**< Hot-reload refresh (defaults to onStart if NULL) */
    void       (*onEnd)(CelsSession *session);    /**< Teardown callback on shutdown / unload */
};

#ifndef CELS_APP_MANIFEST_TYPEDEF_DEFINED
#define CELS_APP_MANIFEST_TYPEDEF_DEFINED
typedef struct CelsAppManifest CelsAppManifest;
/** @deprecated Deprecated legacy alias for CelsAppManifest. */
typedef struct CelsAppManifest CelsAppDef;
#endif
typedef const CelsAppManifest *(*CelsAppEntryFn)(void);

/**
 * Application manifest entry point. Exported from .dll in Debug mode,
 * or linked statically in Release standalone mode.
 *
 * @return Pointer to application manifest structure. Never NULL.
 */
CELS_APP_EXPORT const CelsAppManifest *CelsGetAppManifest(void);

/**
 * @deprecated Legacy manifest entry point synonym. Use CelsGetAppManifest().
 */
CELS_APP_EXPORT const CelsAppManifest *CelsGetAppDef(void);

/* ========================================================================= */
/* Application Manifest Registration Macro (CEL_App_Manifest)                */
/* ========================================================================= */

/**
 * Declares an application manifest exporting lifecycle callbacks and metadata.
 *
 * Example:
 * @code
 *     static void WindowApp_OnStart(CelsSession *session) {
 *         cel_attach(session, WindowComposition, WindowEval);
 *     }
 *
 *     CEL_App_Manifest(WindowApp,
 *         .title      = "My CELS Window",
 *         .appVersion = "1.0.0",
 *         .onStart    = WindowApp_OnStart,
 *         .onReload   = WindowApp_OnStart,
 *         .onEnd      = WindowApp_OnEnd
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
        .setSession = _cels_app_set_session_##AppName, \
        __VA_ARGS__ \
    }; \
    CELS_APP_EXPORT const CelsAppManifest *CelsGetAppManifest(void) { \
        return &_cels_app_manifest_##AppName; \
    } \
    CELS_APP_EXPORT const CelsAppManifest *CelsGetAppDef(void) { \
        return &_cels_app_manifest_##AppName; \
    } \
    typedef int _cels_app_semicolon_swallower_##AppName

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
