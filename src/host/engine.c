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
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
    #include <dlfcn.h>
#endif

#include "cels/runtime/thread.h"
#include "cels/app.h"
#include "cels/runtime/module.h"
#include "cels/runtime/session.h"

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
CelsResult _CelsEngineInitInternalWithOptions(CelsEngine *engine, const char *appName, const CelsSessionConfig *config)
{
    if (engine == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    memset(engine, 0, sizeof(*engine));
    engine->magic = CELS_ENGINE_MAGIC;
    engine->moduleCount = 0;
    engine->isStarted = false;
    engine->broadcastCount = 0;
    CelsMutexInit(&engine->broadcastMutex);
    CelsSetCurrentEngine(engine);

    CelsSessionConfig sc = config ? *config : (CelsSessionConfig){0};
    sc.engine = engine;
    CelsSessionInit(&engine->session, &sc);
    engine->session.engine = engine;
    engine->secondarySessionCount = 0;

    if (appName != NULL && appName[0] != '\0') {
        return CelsEngineLoadApp(engine, appName);
    }

    return CELS_OK;
}

/**
 * Internal initializer for CelsEngine without custom session configuration.
 *
 * Forwards to _CelsEngineInitInternalWithOptions with default session options.
 *
 * @param engine  Target host engine. Non-NULL.
 * @param appName Application module target name or library path. Safe if NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult _CelsEngineInitInternal(CelsEngine *engine, const char *appName)
{
    return _CelsEngineInitInternalWithOptions(engine, appName, NULL);
}

/**
 * Tears down a host engine, releasing modules, sessions, and dynamically loaded libraries.
 *
 * Invokes CelsEngineEnd if running, destroys registered subsystem modules in reverse order
 * of registration, frees all secondary sessions, tears down the primary session,
 * destroys the broadcast mutex, and clears ambient thread pointers.
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

    for (uint32_t i = 0; i < CELS_MAX_SECONDARY_SESSIONS; ++i) {
        if (engine->secondarySessions[i].isUsed && engine->secondarySessions[i].session != NULL) {
            CelsSessionDestroy(engine->secondarySessions[i].session);
            free(engine->secondarySessions[i].session);
            engine->secondarySessions[i].session = NULL;
            engine->secondarySessions[i].isUsed = false;
        }
    }
    engine->secondarySessionCount = 0;
    CelsSessionDestroy(&engine->session);
    CelsMutexDestroy(&engine->broadcastMutex);

    if (s_currentEngine == engine) {
        s_currentEngine = NULL;
    }
    engine->magic = 0;
}

/**
 * Starts execution of an engine instance and performs the initial composition mount.
 *
 * Sets ambient engine and session context on the calling thread, synchronizes session
 * across the application binary boundary, invokes manifest->onStart, marks isStarted = true,
 * and performs the initial CelsSessionRecompose pass on the primary session.
 *
 * @param engine Target host engine with valid manifest. Non-NULL.
 * @return CELS_OK on success, or CELS_ERROR_INVALID_STATE if manifest is missing.
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
        engine->manifest->onStart(engine, &engine->session);
    }

    engine->isStarted = true;
    CelsResult res = CelsSessionRecompose(&engine->session);

    s_currentEngine = prevEngine;
    CelsSetCurrentSession(prevSession);
    return res;
}

/**
 * Stops execution of an engine instance and invokes teardown lifecycle hooks.
 *
 * If an app module is loaded, unloads the module and detaches its composition.
 * If running statically, invokes manifest->onEnd. Marks isStarted = false.
 *
 * @param engine Target host engine. Safe if NULL or if not running.
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
 * Enqueues a thread-safe global broadcast event into the engine's staging queue.
 *
 * Acquires engine->broadcastMutex, stages the event record, and releases the mutex.
 * Broadcasts are later dispatched across all engine sessions during CelsEngineDrainBroadcasts.
 *
 * @param engine   Target host engine. Safe if NULL.
 * @param typeHash 64-bit FNV-1a type name hash identifying the event type.
 * @param payload  Pointer to broadcast payload data buffer.
 * @param size     Payload size in bytes (clamped to CELS_EVENT_MAX_PAYLOAD).
 * @return True if broadcast was staged successfully; false if engine is NULL or queue is full.
 */
bool CelsEngineBroadcast(CelsEngine *engine, uint64_t typeHash, const void *payload, size_t size)
{
    if (engine == NULL) {
        return false;
    }
    CelsMutexLock(&engine->broadcastMutex);
    if (engine->broadcastCount >= CELS_EVENT_QUEUE_CAPACITY) {
        CelsMutexUnlock(&engine->broadcastMutex);
        return false;
    }

    CelsEventRecord *const rec = &engine->broadcastQueue[engine->broadcastCount++];
    rec->typeHash = typeHash;
    rec->scope = CELS_EVENT_SCOPE_BROADCAST;
    rec->size = (uint32_t)((size > CELS_EVENT_MAX_PAYLOAD) ? CELS_EVENT_MAX_PAYLOAD : size);
    rec->sourceGroupId = 0;
    rec->readCount = 0;
    if (payload != NULL && rec->size > 0) {
        memcpy(rec->payload, payload, rec->size);
    }

    CelsMutexUnlock(&engine->broadcastMutex);
    return true;
}

/**
 * Drains staged broadcast events across all engine-supervised sessions.
 *
 * Acquires engine->broadcastMutex, distributes each staged broadcast into the primary
 * session and every active secondary session, resolves any task fiber waiters,
 * invalidates matching broadcast listeners, and resets engine->broadcastCount.
 *
 * @param engine Target host engine. Safe if NULL.
 */
void CelsEngineDrainBroadcasts(CelsEngine *engine)
{
    if (engine == NULL) {
        return;
    }
    CelsMutexLock(&engine->broadcastMutex);
    if (engine->broadcastCount == 0) {
        CelsMutexUnlock(&engine->broadcastMutex);
        return;
    }

    /* 1. Deliver staged broadcasts into engine's primary session */
    {
        CelsSession *sess = &engine->session;
        CelsEventQueue *const q = &sess->eventQueue;

        for (uint32_t i = 0; i < engine->broadcastCount; ++i) {
            const CelsEventRecord *src = &engine->broadcastQueue[i];

            for (uint32_t w = 0; w < q->waiterCount; ++w) {
                CelsTaskEventWaiter *const waiter = &q->waiters[w];
                if (waiter->isWaiting && waiter->typeHash == src->typeHash && (waiter->scopeMask & CELS_EVENT_SCOPE_BROADCAST)) {
                    if (waiter->outBuffer != NULL && src->size > 0) {
                        const size_t copySize = (src->size < waiter->outSize) ? src->size : waiter->outSize;
                        memcpy(waiter->outBuffer, src->payload, copySize);
                    }
                    waiter->received = true;
                    waiter->isWaiting = false;
                    CelsSessionInvalidateKey(sess, waiter->groupKey);
                }
            }

            if (q->count < CELS_EVENT_QUEUE_CAPACITY) {
                CelsEventRecord *const dst = &q->records[q->count++];
                *dst = *src;

                for (uint32_t l = 0; l < q->listenerCount; ++l) {
                    if (q->listeners[l].typeHash == src->typeHash && (q->listeners[l].scopeMask & CELS_EVENT_SCOPE_BROADCAST)) {
                        CelsSessionInvalidateKey(sess, q->listeners[l].key);
                    }
                }
            }
        }
    }

    /* 2. Deliver staged broadcasts into all engine-managed secondary sessions */
    for (uint32_t sIdx = 0; sIdx < CELS_MAX_SECONDARY_SESSIONS; ++sIdx) {
        if (!engine->secondarySessions[sIdx].isUsed || engine->secondarySessions[sIdx].session == NULL) continue;
        CelsSession *sess = engine->secondarySessions[sIdx].session;
        CelsEventQueue *const q = &sess->eventQueue;

        for (uint32_t i = 0; i < engine->broadcastCount; ++i) {
            const CelsEventRecord *src = &engine->broadcastQueue[i];

            for (uint32_t w = 0; w < q->waiterCount; ++w) {
                CelsTaskEventWaiter *const waiter = &q->waiters[w];
                if (waiter->isWaiting && waiter->typeHash == src->typeHash && (waiter->scopeMask & CELS_EVENT_SCOPE_BROADCAST)) {
                    if (waiter->outBuffer != NULL && src->size > 0) {
                        const size_t copySize = (src->size < waiter->outSize) ? src->size : waiter->outSize;
                        memcpy(waiter->outBuffer, src->payload, copySize);
                    }
                    waiter->received = true;
                    waiter->isWaiting = false;
                    CelsSessionInvalidateKey(sess, waiter->groupKey);
                }
            }

            if (q->count < CELS_EVENT_QUEUE_CAPACITY) {
                CelsEventRecord *const dst = &q->records[q->count++];
                *dst = *src;

                for (uint32_t l = 0; l < q->listenerCount; ++l) {
                    if (q->listeners[l].typeHash == src->typeHash && (q->listeners[l].scopeMask & CELS_EVENT_SCOPE_BROADCAST)) {
                        CelsSessionInvalidateKey(sess, q->listeners[l].key);
                    }
                }
            }
        }
    }

    engine->broadcastCount = 0;
    CelsMutexUnlock(&engine->broadcastMutex);
}

void CelsEngineSetMode(CelsEngine *engine, CelsEngineMode mode)
{
    if (engine != NULL) {
        engine->mode = mode;
    }
}

CelsEngineMode CelsEngineGetMode(const CelsEngine *engine)
{
    return engine ? engine->mode : CELS_MODE_IMMEDIATE;
}

bool CelsEngineNeedsRecompose(const CelsEngine *engine)
{
    if (engine == NULL) {
        return false;
    }
    if (engine->mode == CELS_MODE_IMMEDIATE) {
        return true;
    }
    if (engine->broadcastCount > 0) {
        return true;
    }
    if (CelsSessionNeedsRecompose(&engine->session)) {
        return true;
    }
    for (uint32_t i = 0; i < CELS_MAX_SECONDARY_SESSIONS; ++i) {
        if (engine->secondarySessions[i].isUsed && engine->secondarySessions[i].session != NULL) {
            if (CelsSessionNeedsRecompose(engine->secondarySessions[i].session)) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Triggers a recomposition pass on the primary session and all active secondary sessions.
 *
 * @param engine Target host engine. Non-NULL.
 * @return CELS_OK on success, or CelsResult error code.
 */
CelsResult CelsEngineRecompose(CelsEngine *engine)
{
    if (engine == NULL) {
        return CELS_ERROR_INVALID_ARGUMENT;
    }
    CelsEngineDrainBroadcasts(engine);
    CelsResult res = CELS_OK;
    if (engine->session.attachedCount > 0) {
        res = CelsSessionRecompose(&engine->session);
    }
    for (uint32_t i = 0; i < CELS_MAX_SECONDARY_SESSIONS; ++i) {
        if (engine->secondarySessions[i].isUsed && engine->secondarySessions[i].session != NULL) {
            if (engine->secondarySessions[i].session->attachedCount > 0) {
                CelsResult r = CelsSessionRecompose(engine->secondarySessions[i].session);
                if (r != CELS_OK && res == CELS_OK) {
                    res = r;
                }
            }
        }
    }
    return res;
}

/**
 * Creates and registers a named secondary reactive session supervised by the engine.
 *
 * If name is "main" or "root", returns the primary session (&engine->session).
 * If a secondary session with name already exists, returns the existing instance.
 * Otherwise, allocates a new heap CelsSession, initializes it with CelsSessionInitWithProfile,
 * binds its engine pointer to engine, and records it in engine->secondarySessions.
 *
 * @param engine  Target host engine. Non-NULL.
 * @param name    Unique alphanumeric session identifier string. Non-NULL.
 * @param profile Memory profile defining the session slab size and capacity limit.
 * @return Pointer to initialized CelsSession, or NULL if capacity exceeded or allocation failed.
 */
CelsSession *CelsEngineCreateSession(CelsEngine *engine, const char *name, CelsSessionProfile profile)
{
    if (engine == NULL || name == NULL || name[0] == '\0') {
        return NULL;
    }

    /* "main" or "root" maps directly to the primary session */
    if (strcmp(name, "main") == 0 || strcmp(name, "root") == 0) {
        return &engine->session;
    }

    /* Check if secondary session with name already exists */
    uint64_t hash = CelsHashKey(name);
    for (uint32_t i = 0; i < CELS_MAX_SECONDARY_SESSIONS; ++i) {
        if (engine->secondarySessions[i].isUsed && engine->secondarySessions[i].nameHash == hash &&
            strcmp(engine->secondarySessions[i].name, name) == 0) {
            return engine->secondarySessions[i].session;
        }
    }

    /* Find next free slot in secondarySessions */
    for (uint32_t i = 0; i < CELS_MAX_SECONDARY_SESSIONS; ++i) {
        if (!engine->secondarySessions[i].isUsed) {
            CelsEngineSessionEntry *entry = &engine->secondarySessions[i];
            memset(entry, 0, sizeof(*entry));
            entry->session = (CelsSession *)calloc(1, sizeof(CelsSession));
            if (entry->session == NULL) {
                return NULL;
            }
            entry->session->isHeapAllocated = true;
            CelsSessionInitWithProfile(entry->session, profile);
            entry->session->engine = engine;
            strncpy(entry->name, name, sizeof(entry->name) - 1);
            entry->nameHash = hash;
            entry->isUsed = true;
            engine->secondarySessionCount++;
            return entry->session;
        }
    }

    fprintf(stderr, "[CELS ERROR] Engine secondary session capacity full (%u / %u). Cannot create session '%s'.\n",
            engine->secondarySessionCount, CELS_MAX_SECONDARY_SESSIONS, name);
    return NULL;
}

/**
 * Retrieves a session supervised by the engine by its unique name string.
 *
 * If name is NULL, empty, "main", or "root", returns the primary session (&engine->session).
 * Otherwise, scans secondary sessions using 64-bit FNV-1a hash matching.
 *
 * @param engine Target host engine. Safe if NULL.
 * @param name   Session identifier name string.
 * @return Pointer to matching CelsSession if found; NULL if not found or engine is NULL.
 */
CelsSession *CelsEngineGetSession(CelsEngine *engine, const char *name)
{
    if (engine == NULL) {
        return NULL;
    }

    /* Default / primary session */
    if (name == NULL || name[0] == '\0' || strcmp(name, "main") == 0 || strcmp(name, "root") == 0) {
        return &engine->session;
    }

    uint64_t hash = CelsHashKey(name);
    for (uint32_t i = 0; i < CELS_MAX_SECONDARY_SESSIONS; ++i) {
        if (engine->secondarySessions[i].isUsed && engine->secondarySessions[i].nameHash == hash &&
            strcmp(engine->secondarySessions[i].name, name) == 0) {
            return engine->secondarySessions[i].session;
        }
    }

    return NULL;
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
CelsResult CelsEngineRunStandalone(const struct CelsAppDef *manifest, const CelsSessionConfig *config)
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
            FARPROC fp = GetProcAddress(hSelf, "CelsGetAppDef");
            if (fp == NULL) {
                fp = GetProcAddress(hSelf, "CelsGetAppManifest");
            }
            if (fp != NULL) {
                typedef const struct CelsAppDef *(*EntryFn)(void);
                EntryFn entry = NULL;
                memcpy(&entry, &fp, sizeof(entry));
                if (entry != NULL) {
                    engine->manifest = entry();
                    return CelsEngineStart(engine);
                }
            }
        }
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
        void *hSelf = dlopen(NULL, RTLD_NOW);
        if (hSelf != NULL) {
            void *fp = dlsym(hSelf, "CelsGetAppDef");
            if (fp == NULL) {
                fp = dlsym(hSelf, "CelsGetAppManifest");
            }
            dlclose(hSelf);
            if (fp != NULL) {
                typedef const struct CelsAppDef *(*EntryFn)(void);
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
