#pragma once

/**
 * @file app.h
 * @brief Application module declaration (CEL_App) and manifest interface for CELS.
 *
 * App level represents the application module (.dll / .so or static module).
 * Applications declare their composition tree, widgets, and logic using CEL_App.
 * Applications access engine-level subsystem memory (SDL, Flecs, Vulkan, Audio)
 * hosted in the executable (CelsEngine) via CEL_GetModule(ModuleType).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cels/session.h"
#include "cels/engine.h"

#ifdef __cplusplus
extern "C" {
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

/* ========================================================================= */
/* Composition Reference Handle (Returned by onStart)                        */
/* ========================================================================= */

/**
 * Represents a reference to a composition root and its optional lifecycle evaluator.
 */
typedef struct CelsCompositionRef {
    uint64_t key;
    void (*body)(CelsSession *session, uint64_t key);
    bool (*lifecycleEval)(void *userData);
} CelsCompositionRef;

#define _CEL_COMPOSITION_REF_2(CompName, Lifecycle) \
    ((CelsCompositionRef){ \
        .key = CelsHashKey(#CompName), \
        .body = _cels_app_body_##CompName, \
        .lifecycleEval = _cels_lifecycle_##Lifecycle \
    })

#define _CEL_COMPOSITION_REF_1(CompName) \
    ((CelsCompositionRef){ \
        .key = CelsHashKey(#CompName), \
        .body = _cels_app_body_##CompName, \
        .lifecycleEval = NULL \
    })

/**
 * Constructs a CelsCompositionRef to attach a composition from onStart.
 *
 * Usage:
 *   return CEL_COMPOSITION(AppRoot);
 *   return CEL_COMPOSITION(AppRoot, AppLifecycle);
 */
#define CEL_COMPOSITION(...) \
    _CEL_GET_MACRO_2(__VA_ARGS__, _CEL_COMPOSITION_REF_2, _CEL_COMPOSITION_REF_1)(__VA_ARGS__)

#define CEL_NO_COMPOSITION ((CelsCompositionRef){ 0, NULL, NULL })

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
    void (*setSession)(CelsSession *s);             /**< Internal session synchronization across DLL boundary */
    CelsCompositionRef (*onStart)(CelsEngine *engine, CelsSession *session); /**< Setup & returns root composition */
    void (*onEnd)(CelsEngine *engine, CelsSession *session);   /**< Teardown callback on shutdown */
    void (*onPrintTree)(const CelsSession *session); /**< Optional tree inspection callback */
} CelsAppManifest;

typedef const CelsAppManifest *(*CelsAppEntryFn)(void);

/**
 * Application manifest entry point. Exported from .dll in Debug mode,
 * or linked statically in Release standalone mode.
 */
CELS_APP_EXPORT const CelsAppManifest *CelsGetAppManifest(void);

/* ========================================================================= */
/* Declarative App Registration Macro (CEL_App)                              */
/* ========================================================================= */

#ifndef NUCLEUS_DEFAULT_CONTINUOUS_COMPOSE
    #define NUCLEUS_DEFAULT_CONTINUOUS_COMPOSE false
#endif

#define CEL_App(AppName, ...) \
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

#define CELS_APP(AppName, ...) CEL_App(AppName, __VA_ARGS__)

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
