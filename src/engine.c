#include "cels/engine.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

#include "cels/app.h"
#include "cels/module.h"
#include "cels/session.h"

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

static CELS_THREAD_LOCAL CelsEngine *s_currentEngine = NULL;

/**
 * Returns the currently active ambient host engine bound to the calling thread.
 *
 * @return Pointer to current CelsEngine, or NULL if none is active.
 */
CelsEngine *CelsGetCurrentEngine(void)
{
    return s_currentEngine;
}

/**
 * Sets the active ambient host engine bound to the calling thread.
 *
 * @param engine Target host engine. Safe if NULL.
 */
void CelsSetCurrentEngine(CelsEngine *engine)
{
    s_currentEngine = engine;
}

/**
 * Internal initializer for CelsEngine.
 *
 * Initializes internal fields, establishes ambient thread binding, configures
 * the primary reactive session, and optionally loads the application module.
 *
 * @param engine  Target host engine. Non-NULL.
 * @param appName Application module target name, or NULL.
 * @return CELS_OK on success, or error code on failure.
 */
CelsResult _CelsEngineInitInternal(CelsEngine *engine, const char *appName)
{
    if (engine == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    memset(engine, 0, sizeof(*engine));
    engine->magic = CELS_ENGINE_MAGIC;
    engine->moduleCount = 0;
    engine->isStarted = false;
    engine->shouldQuit = false;
    CelsSetCurrentEngine(engine);

    CelsSessionConfig sc = { .engine = engine };
    CelsSessionInit(&engine->session, &sc);
    engine->session.engine = engine;

    if (appName != NULL && appName[0] != '\0') {
        return CelsEngineLoadApp(engine, appName);
    }

    return CELS_OK;
}

/**
 * Tears down a host engine, releasing modules, sessions, and dynamically loaded libraries.
 *
 * @param engine Target host engine. Safe if NULL.
 */
void CelsEngineDestroy(CelsEngine *engine)
{
    if (engine == NULL) {
        return;
    }

    if (engine->isStarted || engine->appModule != NULL) {
        CelsEngineEnd(engine);
    }

    /* Teardown engine subsystem modules in reverse registration order */
    for (uint32_t i = engine->moduleCount; i > 0; --i) {
        if (engine->modules[i - 1].onDestroy != NULL) {
            engine->modules[i - 1].onDestroy(engine->modules[i - 1].instance);
        }
    }
    engine->moduleCount = 0;

    CelsSessionDestroy(&engine->session);

    if (s_currentEngine == engine) {
        s_currentEngine = NULL;
    }
    engine->magic = 0;
}

/**
 * Starts execution of an engine instance and performs initial composition mount.
 *
 * @param engine Target host engine. Non-NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsEngineStart(CelsEngine *engine)
{
    if (engine == NULL || engine->manifest == NULL) {
        return CELS_ERROR_INVALID_STATE;
    }
    if (engine->isStarted) {
        return CELS_OK;
    }

    CelsEngine *const prevEngine = s_currentEngine;
    CelsSession *const prevSession = CelsGetCurrentSession();
    s_currentEngine = engine;
    CelsSetCurrentSession(&engine->session);

    if (engine->manifest->setSession != NULL) {
        engine->manifest->setSession(&engine->session);
    }

    if (engine->manifest->onStart != NULL) {
        CelsCompositionRef ref = engine->manifest->onStart(engine, &engine->session);
        if (ref.body != NULL && ref.key != 0) {
            CelsSessionAttachComposition(&engine->session, ref.key, ref.body, ref.userData, ref.lifecycleEval, ref.evalCtx);
        }
    }

    engine->isStarted = true;
    CelsResult res = CelsSessionRecompose(&engine->session);

    s_currentEngine = prevEngine;
    CelsSetCurrentSession(prevSession);
    return res;
}

/**
 * Stops execution of an engine instance and invokes teardown hooks.
 *
 * @param engine Target host engine. Safe if NULL.
 */
void CelsEngineEnd(CelsEngine *engine)
{
    if (engine == NULL || !engine->isStarted) {
        return;
    }

    if (engine->appModule != NULL) {
        CelsAppModuleUnload(engine->appModule, &engine->session);
        free(engine->appModule);
        engine->appModule = NULL;
    } else if (engine->manifest != NULL && engine->manifest->onEnd != NULL) {
        CelsEngine *const prevEngine = s_currentEngine;
        CelsSession *const prevSession = CelsGetCurrentSession();
        s_currentEngine = engine;
        CelsSetCurrentSession(&engine->session);

        engine->manifest->onEnd(engine, &engine->session);

        s_currentEngine = prevEngine;
        CelsSetCurrentSession(prevSession);
    }

    engine->isStarted = false;
}

/**
 * Triggers a recomposition pass on the engine's primary session and all active sessions.
 *
 * @param engine Target host engine. Non-NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsEngineRecompose(CelsEngine *engine)
{
    if (engine == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    CelsResult res = CelsSessionRecompose(&engine->session);
    CelsResult allRes = CelsRecomposeAllSessions();
    return (res != CELS_OK) ? res : allRes;
}

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
                              void (*onDestroy)(void *instance))
{
    assert(engine != NULL);
    assert(instance != NULL);

    for (uint32_t i = 0; i < engine->moduleCount; ++i) {
        if (engine->modules[i].key == key) {
            engine->modules[i].name = name;
            engine->modules[i].instance = instance;
            engine->modules[i].onDestroy = onDestroy;
            return;
        }
    }

    if (engine->moduleCount >= CELS_MAX_MODULES) {
        fprintf(stderr, "[CELS ERROR] Engine module registry full (%u / %u). Cannot register module '%s'.\n",
                engine->moduleCount, CELS_MAX_MODULES, name);
        return;
    }

    engine->modules[engine->moduleCount++] = (CelsModuleBinding){
        .key = key,
        .name = name,
        .instance = instance,
        .onDestroy = onDestroy
    };
}

/**
 * Retrieves a registered subsystem module pointer by its 64-bit key from the engine.
 *
 * @param engine Target host engine. Safe if NULL.
 * @param key    Unique 64-bit module type key.
 * @return Pointer to module struct instance, or NULL if not found.
 */
void *CelsEngineGetModule(const CelsEngine *engine, uint64_t key)
{
    if (engine == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < engine->moduleCount; ++i) {
        if (engine->modules[i].key == key) {
            return engine->modules[i].instance;
        }
    }
    return NULL;
}

/**
 * Dispatches module registration to the target engine or session context.
 *
 * @param ctx        Target context (CelsEngine* or CelsSession*), or NULL for ambient.
 * @param key        Unique 64-bit module type key.
 * @param name       Human-readable module identifier for diagnostics. Non-NULL.
 * @param inst       Pointer to developer-allocated module struct. Non-NULL.
 * @param on_destroy Optional cleanup callback. Safe if NULL.
 */
void _cels_dispatch_register_module(void *ctx, uint64_t key, const char *name, void *inst, void (*on_destroy)(void*))
{
    if (ctx != NULL) {
        uint32_t magic = *(const uint32_t *)ctx;
        if (magic == CELS_ENGINE_MAGIC) {
            CelsEngineRegisterModule((CelsEngine *)ctx, key, name, inst, on_destroy);
            return;
        } else if (magic == CELS_SESSION_MAGIC) {
            CelsSession *s = (CelsSession *)ctx;
            if (s->engine != NULL) {
                CelsEngineRegisterModule(s->engine, key, name, inst, on_destroy);
            } else {
                CelsSessionRegisterModule(s, key, name, inst, NULL, on_destroy);
            }
            return;
        }
    }

    /* Ambient resolution */
    CelsEngine *engine = CelsGetCurrentEngine();
    if (engine != NULL) {
        CelsEngineRegisterModule(engine, key, name, inst, on_destroy);
        return;
    }

    CelsSession *s = CelsGetCurrentSession();
    if (s != NULL) {
        if (s->engine != NULL) {
            CelsEngineRegisterModule(s->engine, key, name, inst, on_destroy);
        } else {
            CelsSessionRegisterModule(s, key, name, inst, NULL, on_destroy);
        }
        return;
    }

    fprintf(stderr, "[CELS ERROR] CEL_RegisterModule: No active CelsEngine or CelsSession to register module '%s'.\n", name);
}

/**
 * Resolves a subsystem module pointer from context or ambient ambient engine/session.
 *
 * @param ctx Context pointer (CelsEngine* or CelsSession*), or NULL for ambient.
 * @param key Unique 64-bit module type key.
 * @return Resolved module pointer, or NULL if not found.
 */
void *_cels_resolve_module(const void *ctx, uint64_t key)
{
    if (ctx != NULL) {
        uint32_t magic = *(const uint32_t *)ctx;
        if (magic == CELS_ENGINE_MAGIC) {
            return CelsEngineGetModule((const CelsEngine *)ctx, key);
        } else if (magic == CELS_SESSION_MAGIC) {
            const CelsSession *s = (const CelsSession *)ctx;
            if (s->engine != NULL) {
                return CelsEngineGetModule(s->engine, key);
            }
            return CelsSessionGetModule(s, key);
        }
    }

    /* Ambient resolution */
    CelsEngine *engine = CelsGetCurrentEngine();
    if (engine != NULL) {
        void *m = CelsEngineGetModule(engine, key);
        if (m != NULL) {
            return m;
        }
    }

    CelsSession *s = CelsGetCurrentSession();
    if (s != NULL) {
        if (s->engine != NULL) {
            void *m = CelsEngineGetModule(s->engine, key);
            if (m != NULL) {
                return m;
            }
        }
        return CelsSessionGetModule(s, key);
    }

    return NULL;
}

/**
 * Executes a standalone monolithic application lifecycle using the provided manifest.
 *
 * @param manifest Application manifest descriptor. Non-NULL.
 * @param config   Optional session configuration, or NULL for defaults.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsEngineRunStandalone(const struct CelsAppManifest *manifest, const CelsSessionConfig *config)
{
    (void)config;
    if (manifest == NULL) {
        return CELS_ERROR_INVALID_STATE;
    }

    CelsEngine engine;
    CelsEngineInit(&engine, NULL);
    engine.manifest = manifest;

    CelsResult res = CelsEngineStart(&engine);

    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);
    return res;
}

/**
 * Resolves, loads, and starts an application dynamic module into the host engine.
 *
 * @param engine  Target host engine. Non-NULL.
 * @param appName Application target name. If NULL, uses default target.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsEngineLoadApp(CelsEngine *engine, const char *appName)
{
    if (engine == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

#if defined(CELS_APP_TARGET)
    if (appName == NULL || appName[0] == '\0') {
        appName = CELS_APP_TARGET;
    }
#endif

    char resolvedPath[CELS_PATH_MAX];
    if (!CelsResolveModulePath(appName, resolvedPath, sizeof(resolvedPath))) {
#if defined(_WIN32)
        HMODULE hSelf = GetModuleHandleA(NULL);
        if (hSelf != NULL) {
            FARPROC fp = GetProcAddress(hSelf, "CelsGetAppManifest");
            if (fp != NULL) {
                typedef const struct CelsAppManifest *(*EntryFn)(void);
                EntryFn entry = NULL;
                memcpy(&entry, &fp, sizeof(entry));
                if (entry != NULL) {
                    engine->manifest = entry();
                    return CelsEngineStart(engine);
                }
            }
        }
#endif
        fprintf(stderr, "[CELS Engine] Could not locate application binary for '%s' (searched: %s)\n",
                appName ? appName : "(default)", resolvedPath);
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    if (engine->appModule == NULL) {
        engine->appModule = (CelsAppModule *)calloc(1, sizeof(CelsAppModule));
        if (engine->appModule == NULL) {
            return CELS_ERROR_OUT_OF_MEMORY;
        }
    }

    if (!CelsAppModuleLoad(engine->appModule, resolvedPath, &engine->session)) {
        free(engine->appModule);
        engine->appModule = NULL;
        return CELS_ERROR_INVALID_STATE;
    }

    engine->manifest = engine->appModule->manifest;
    engine->isStarted = true;
    return CelsSessionRecompose(&engine->session);
}

/**
 * Polls for dynamic library modifications and reloads the application if updated.
 *
 * @param engine Target host engine. Safe if NULL.
 * @return True if a reload occurred and the application was recomposed; false otherwise.
 */
bool CelsAppRuntimeCheck(CelsEngine *engine)
{
    if (engine == NULL || engine->appModule == NULL) {
        return false;
    }
    bool reloaded = CelsAppModuleCheckAndReload(engine->appModule, &engine->session);
    if (reloaded) {
        engine->manifest = engine->appModule->manifest;
    }
    return reloaded;
}
