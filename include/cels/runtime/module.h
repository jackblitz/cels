#pragma once

/**
 * @file module.h
 * @brief Dynamic library application loader and hot-reloading interface for CELS.
 *
 * Provides cross-platform dynamic library loading, file-change detection,
 * automatic Windows shadow-copying to bypass OS DLL file locking, and
 * seamless recomposition re-evaluation for applications registered with CEL_App.
 *
 * Typical application usage (in your reloadable .so/.dll):
 * @code
 *     CEL_Composable(MyWidget, ...) { ... }
 *     CEL_Composition(AppRoot, key) { MyWidget(); }
 *
 *     static CelsCompositionRef OnStart(CelsSession *session) {
 *         // Register developer's custom system/engine modules:
 *         static MyAudioEngine audio = { ... };
 *         CEL_RegisterModule(session, MyAudioEngine, &audio);
 *
 *         // Return the root composition:
 *         return CEL_COMPOSITION(AppRoot);
 *     }
 *
 *     static void OnEnd(CelsSession *session) {
 *         // Cleanup
 *     }
 *
 *     // Register application:
 *     CEL_App(MyGameApp,
 *         .onStart = OnStart,
 *         .onEnd = OnEnd
 *     );
 * @endcode
 *
 * Typical host runner usage (in your .exe):
 * @code
 *     CelsSession session;
 *     CelsSessionInit(&session, NULL);
 *
 *     CelsAppModule app;
 *     CelsAppModuleLoad(&app, "game_app.dll", &session);
 *
 *     while (running) {
 *         // Checks file timestamp, reloads library, calls onReload, and recomposes if changed:
 *         CelsAppModuleCheckAndReload(&app, &session);
 *     }
 *
 *     CelsAppModuleUnload(&app, &session);
 *     CelsSessionDestroy(&session);
 * @endcode
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cels/runtime/session.h"
#include "cels/app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Symbol Visibility & Export Macro                                          */
/* ========================================================================= */

#if defined(_WIN32) || defined(__CYGWIN__)
    #define CELS_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
    #define CELS_MODULE_EXPORT __attribute__((visibility("default")))
#else
    #define CELS_MODULE_EXPORT
#endif

#define CELS_PATH_MAX 512

/* ========================================================================= */
/* Host Application Dynamic Module Handle & Operations                       */
/* ========================================================================= */

/**
 * Handle representing a loaded dynamic application module and its reload state.
 */
typedef struct CelsAppModule {
    char originalPath[CELS_PATH_MAX];
    char loadedPath[CELS_PATH_MAX];
    char entrySymbol[64];
    void *handle;
    uint64_t lastWriteTime;
    uint32_t instanceId;
    uint32_t reloadCount;
    const struct CelsAppDef *def;
    const struct CelsAppDef *manifest;
    uint64_t attachedKey;
    bool isLoaded;
} CelsAppModule;

struct CelsEngine;

/**
 * Handle representing an application hosted by the host layer.
 * Encapsulates dynamic library loading, session binding, hot-reloading, and lifecycle.
 */
struct CelsApp {
    CelsAppModule        module;
    struct CelsEngine   *engine;
    struct CelsSession  *session;
    bool                 isStarted;
    bool                 isStatic;
};

#ifndef CELS_APP_TYPEDEF_DEFINED
#define CELS_APP_TYPEDEF_DEFINED
typedef struct CelsApp CelsApp;
#endif

/**
 * Loads an application dynamic library and binds it to an engine and target session.
 *
 * @param app      Pointer to uninitialized CelsApp handle. Non-NULL.
 * @param engine   Owning host engine. Non-NULL.
 * @param session  Target session for this application. Non-NULL.
 * @param dllPath  Path or target name of the dynamic library. Non-NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsAppLoad(CelsApp *app, struct CelsEngine *engine, struct CelsSession *session, const char *dllPath);

/**
 * Loads an application dynamic library with a specific entry symbol.
 *
 * Allows multiple apps compiled into the same DLL to be loaded independently.
 *
 * @param app         Pointer to uninitialized CelsApp handle. Non-NULL.
 * @param engine      Owning host engine. Non-NULL.
 * @param session     Target session. Non-NULL.
 * @param dllPath     Path or target name of the dynamic library. Non-NULL.
 * @param entrySymbol Exported entry symbol (defaults to "CelsGetAppDef" if NULL).
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsAppLoadEntry(CelsApp *app, struct CelsEngine *engine, struct CelsSession *session, const char *dllPath, const char *entrySymbol);

/**
 * Binds a static application definition for monolithic / non-reloadable builds.
 *
 * @param app     Pointer to uninitialized CelsApp handle. Non-NULL.
 * @param engine  Owning host engine. Non-NULL.
 * @param session Target session. Non-NULL.
 * @param def     Static application definition. Non-NULL.
 * @return CELS_OK on success.
 */
CelsResult CelsAppBindStatic(CelsApp *app, struct CelsEngine *engine, struct CelsSession *session, const struct CelsAppDef *def);

/**
 * Starts execution of a loaded application.
 *
 * Synchronizes ambient session across the binary boundary, invokes the app's onStart hook
 * passing (engine, session), and executes the initial composition pass.
 *
 * @param app Target application handle. Non-NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsAppStart(CelsApp *app);

/**
 * Checks if the application dynamic library was modified on disk and hot-reloads it.
 *
 * @param app Target application handle. Safe if NULL.
 * @return True if a reload occurred; false otherwise.
 */
bool CelsAppCheckReload(CelsApp *app);

/**
 * Forces an immediate reload of the application dynamic library.
 *
 * @param app Target application handle. Safe if NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsAppReload(CelsApp *app);

/**
 * Stops execution, unloads the dynamic library, and cleans up shadow copy files.
 *
 * Invokes the app's onEnd hook passing (engine, session) and detaches its root composition.
 *
 * @param app Target application handle. Safe if NULL.
 */
void CelsAppDestroy(CelsApp *app);

/**
 * Loads a CELS application dynamically from disk.
 *
 * Copies libraryPath to a temporary shadow file on Windows to prevent file locking,
 * loads the library, queries CelsGetAppDef, initializes ambient session,
 * invokes manifest->onStart, and attaches the returned composition.
 *
 * @param app         Target application module struct. Non-NULL.
 * @param libraryPath Path to .dll / .so file. Non-NULL.
 * @param session     Target session. Non-NULL.
 * @return true on success, false on failure.
 */
bool CelsAppModuleLoad(CelsAppModule *app, const char *libraryPath, CelsSession *session);

/**
 * Loads a CELS application dynamically from disk with custom entry symbol and autoStart control.
 *
 * @param app         Target application module struct. Non-NULL.
 * @param libraryPath Path to .dll / .so file. Non-NULL.
 * @param session     Target session. Non-NULL.
 * @param entrySymbol Symbol name (or NULL for default).
 * @param autoStart   If true, invokes onStart immediately.
 * @return true on success, false on failure.
 */
bool CelsAppModuleLoadEntryInternal(CelsAppModule *app, const char *libraryPath, CelsSession *session, const char *entrySymbol, bool autoStart);

/**
 * Checks if libraryPath has been modified on disk and reloads it if newer.
 *
 * If updated:
 * 1. Unloads old dynamic library handle.
 * 2. Copies updated file to new shadow file and loads it.
 * 3. Re-binds ambient session in new dynamic library.
 * 4. Re-attaches root composition with refreshed function pointers.
 * 5. Calls CelsSessionHotReload(session) to flag groups for re-evaluation.
 * 6. Executes CelsSessionRecompose(session) to apply code changes immediately.
 *
 * @param app     Active application module handle. Non-NULL.
 * @param session Active session. Non-NULL.
 * @return true if a reload occurred and recomposed; false if no changes.
 */
bool CelsAppModuleCheckAndReload(CelsAppModule *app, CelsSession *session);

/**
 * Unloads the dynamic application module and releases its resources.
 *
 * @param app     Target application module struct. Non-NULL.
 * @param session Target session. Non-NULL.
 */
void CelsAppModuleUnload(CelsAppModule *app, CelsSession *session);

/**
 * Resolves the filesystem path to an application dynamic library.
 *
 * Searches the directory containing the host executable for <appName>.dll,
 * lib<appName>.dll, or unix shared object variants.
 *
 * @param appName Application target name (e.g. "engine_app"). If NULL, resolves via default target.
 * @param outPath Output buffer for resolved path. Non-NULL.
 * @param maxLen  Capacity of output buffer.
 * @return true if found and readable; false otherwise (outPath still contains expected path).
 */
bool CelsResolveModulePath(const char *appName, char *outPath, size_t maxLen);

/* Backward compatibility aliases */
typedef CelsAppModule CelsModule;
#define CelsModuleLoad CelsAppModuleLoad
#define CelsModuleCheckAndReload CelsAppModuleCheckAndReload
#define CelsModuleUnload CelsAppModuleUnload

#ifdef __cplusplus
}
#endif
