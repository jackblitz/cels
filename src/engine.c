#include "cels/engine.h"
#include "cels/app.h"
#include "cels/session.h"
#include "cels/module.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

CelsEngine *CelsGetCurrentEngine(void)
{
    return s_currentEngine;
}

void CelsSetCurrentEngine(CelsEngine *engine)
{
    s_currentEngine = engine;
}

void CelsEngineInit(CelsEngine *engine, const struct CelsAppManifest *manifest,
                    const CelsSessionConfig *config)
{
    assert(engine != NULL);
    memset(engine, 0, sizeof(*engine));
    engine->magic = CELS_ENGINE_MAGIC;
    engine->manifest = manifest;
    engine->moduleCount = 0;
    engine->isStarted = false;
    engine->shouldQuit = false;

    CelsSessionConfig sc = config ? *config : (CelsSessionConfig){0};
    if (sc.slabSize == 0 && manifest != NULL && manifest->slabSize > 0) {
        sc.slabSize = manifest->slabSize;
        if (sc.maxGroups == 0 && manifest->maxGroups > 0) {
            sc.maxGroups = manifest->maxGroups;
        }
    }
    sc.engine = engine;
    CelsSessionInit(&engine->session, &sc);
    engine->session.engine = engine;
}

void CelsEngineDestroy(CelsEngine *engine)
{
    if (engine == NULL) {
        return;
    }

    if (engine->appModule != NULL) {
        CelsAppModuleUnload(engine->appModule, &engine->session);
        free(engine->appModule);
        engine->appModule = NULL;
        engine->isStarted = false;
    } else if (engine->isStarted) {
        CelsEngineEnd(engine);
    }

    /* Slot cleanup can release entities, GPU resources, and other objects
     * belonging to registered modules. Keep those modules alive and resolvable
     * until every session lifecycle callback has finished. */
    CelsSessionDestroy(&engine->session);

    /* Teardown engine subsystem modules after their session-owned resources. */
    for (uint32_t i = engine->moduleCount; i > 0; --i) {
        if (engine->modules[i - 1].onDestroy != NULL) {
            engine->modules[i - 1].onDestroy(engine->modules[i - 1].instance);
        }
    }
    engine->moduleCount = 0;

    if (s_currentEngine == engine) {
        s_currentEngine = NULL;
    }
    engine->magic = 0;
}

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
            CelsSessionAttachComposition(&engine->session, ref.key, ref.body, ref.lifecycleEval, NULL);
        }
    }

    engine->isStarted = true;
    CelsResult res = CelsSessionRecompose(&engine->session);

    s_currentEngine = prevEngine;
    CelsSetCurrentSession(prevSession);
    return res;
}

void CelsEngineEnd(CelsEngine *engine)
{
    if (engine == NULL || !engine->isStarted) {
        return;
    }

    if (engine->manifest != NULL && engine->manifest->onEnd != NULL) {
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

void CelsEngineRegisterModule(CelsEngine *engine, uint64_t key,
                              const char *name, void *instance,
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

    assert(engine->moduleCount < CELS_MAX_MODULES && "Exceeded CELS_MAX_MODULES in CelsEngine");
    engine->modules[engine->moduleCount++] = (CelsModuleBinding){
        .key = key,
        .name = name,
        .instance = instance,
        .onDestroy = onDestroy
    };
}

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

void _cels_dispatch_register_module(void *ctx, uint64_t key, const char *name,
                                    void *inst, void (*on_destroy)(void *))
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

CelsResult CelsEngineRunStandalone(const struct CelsAppManifest *manifest,
                                   const CelsSessionConfig *config)
{
    if (manifest == NULL) {
        return CELS_ERROR_INVALID_STATE;
    }

    CelsEngine engine;
    CelsEngineInit(&engine, manifest, config);

    CelsResult res = CelsEngineStart(&engine);

    CelsEngineEnd(&engine);
    CelsEngineDestroy(&engine);
    return res;
}

CelsResult CelsEngineLoadApp(CelsEngine *engine, const char *appName)
{
    if (engine == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }

    char resolvedPath[CELS_PATH_MAX] = {0};
    bool found = CelsResolveModulePath(appName, resolvedPath, sizeof(resolvedPath));

    if (!found) {
        fprintf(stderr,
                "[CELS Engine] Application library '%s' not found on disk (expected: '%s').\n",
                appName ? appName : "(default)", resolvedPath);
        return CELS_ERROR_INVALID_STATE;
    }

    if (engine->appModule != NULL) {
        CelsAppModuleUnload(engine->appModule, &engine->session);
        free(engine->appModule);
        engine->appModule = NULL;
    }

    engine->appModule = (CelsAppModule *)calloc(1, sizeof(CelsAppModule));
    if (engine->appModule == NULL) {
        return CELS_ERROR_OUT_OF_MEMORY;
    }

    if (!CelsAppModuleLoad(engine->appModule, resolvedPath, &engine->session)) {
        fprintf(stderr, "[CELS Engine] Failed to load dynamic library '%s'.\n", resolvedPath);
        free(engine->appModule);
        engine->appModule = NULL;
        return CELS_ERROR_INVALID_STATE;
    }

    engine->manifest = engine->appModule->manifest;
    engine->isStarted = true;

    /* Initial mount pass */
    CelsSessionRecompose(&engine->session);

    return CELS_OK;
}

bool CelsAppRuntimeCheck(CelsEngine *engine)
{
    if (engine == NULL || engine->appModule == NULL) {
        return false;
    }

    return CelsAppModuleCheckAndReload(engine->appModule, &engine->session);
}
