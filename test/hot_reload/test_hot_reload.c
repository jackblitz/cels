#ifdef NDEBUG
#undef NDEBUG
#endif
#include "cels.h"
#include "cels/runtime/module.h"
#include "cli/test_cli.h"

#include <assert.h>
#include <stdio.h>

/* ========================================================================= */
/* Test State and Composables                                                */
/* ========================================================================= */

static int s_renders = 0;
static int s_rememberedCurrent = 0;

CEL_State(HotState) {
    int value;
    int revision;
};

CEL_Composable(HotWidget, HotState*, state) {
    s_renders++;
    int *localCounter = cel_remember(int, 42);
    (*localCounter)++;
    s_rememberedCurrent = *localCounter;

    cel_watch(state);
    const HotState *val = state;
    (void)val;
}

CEL_Composition(HotApp, void *userData) {
    (void)userData;
    HotState *state = cel_remember_state(HotState, { .value = 10, .revision = 1 });
    HotWidget(state);
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

static void TestHotReloadInvalidation(void)
{
    s_renders = 0;
    s_rememberedCurrent = 0;

    CelsSession session;
    CelsSessionInit(&session, NULL);

    cel_attach(&session, HotApp);

    /* Pass 1: Fresh mount */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_renders == 1);
    assert(s_rememberedCurrent == 43);

    /* Pass 2: Quiet recompose - O(1) instant skip */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_renders == 1);

    /* Pass 3: Trigger hot reload */
    CelsSessionHotReload(&session);
    assert(session.isHotReloadPending == true);

    /* Verify all active groups are marked INVALIDATED and not FRESH_MOUNT */
    const uint32_t groupCount = CelsGetLogicalGroupCount(&session);
    for (uint32_t i = 0; i < groupCount; ++i) {
        const CelsSlotGroup *g = CelsGetGroup(&session, i);
        assert((g->flags & CELS_FLAG_INVALIDATED) != 0);
        assert((g->flags & CELS_FLAG_FRESH_MOUNT) == 0);
    }

    /* Pass 4: Recompose after hot reload - executes with newly loaded code! */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_renders == 2);
    assert(s_rememberedCurrent == 44);
    assert(session.isHotReloadPending == false);

    /* Pass 5: Subsequent quiet check skips again */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_renders == 2);

    CelsSessionDestroy(&session);
}

static void TestHotReloadStateRetention(void)
{
    s_renders = 0;
    s_rememberedCurrent = 0;

    CelsSession session;
    CelsSessionInit(&session, NULL);
    cel_attach(&session, HotApp);

    CelsSessionRecompose(&session);
    assert(s_rememberedCurrent == 43);

    const HotState *state = cel_get_state(&session, HotState);
    assert(state != NULL);
    assert(state->value == 10);

    /* Mutate state before hot reload */
    cels_session_mutate(&session, HotState) {
        this->value = 999;
        this->revision = 5;
    }
    CelsSessionRecompose(&session);
    assert(s_renders == 2);
    assert(state->value == 999);

    /* Trigger hot reload */
    CelsSessionHotReload(&session);
    CelsSessionRecompose(&session);

    /* State must STILL be 999 across hot reload */
    const HotState *retainedState = cel_get_state(&session, HotState);
    assert(retainedState != NULL);
    assert(retainedState->value == 999);
    assert(retainedState->revision == 5);
    assert(s_renders == 3);
    assert(s_rememberedCurrent == 45);

    CelsSessionDestroy(&session);
}

static void TestHotReloadSchemaEvolution(void)
{
    CelsSession session;
    CelsSessionInit(&session, NULL);

    /* Simulate mounting a group with a 4-byte slot */
    assert(CelsEnterComposition(&session, 0x1234));
    int *slotA = (int *)CelsResolveSlot(&session, sizeof(int), &(int){ 10 });
    assert(slotA != NULL && *slotA == 10);
    CelsExitGroup(&session);

    /* Hot reload: composable schema size changes from 4 bytes to 8 bytes */
    CelsSessionHotReload(&session);

    assert(CelsEnterComposition(&session, 0x1234));
    uint64_t *slotB = (uint64_t *)CelsResolveSlot(&session,
                                                  sizeof(uint64_t),
                                                  &(uint64_t){ 999999ULL });
    assert(slotB != NULL);
    assert(*slotB == 999999ULL);
    CelsExitGroup(&session);

    CelsSessionDestroy(&session);
}

/* ========================================================================= */
/* CEL_Module Registry Tests                                                 */
/* ========================================================================= */

CEL_Module(SDLTestModule) {
    int windowWidth;
    int windowHeight;
};

CEL_Module(FlecsTestModule) {
    void *world;
    int reloadNotificationCount;
};

static int s_flecsDestroyCount = 0;

static void FlecsTest_OnDestroy(void *instance)
{
    (void)instance;
    s_flecsDestroyCount++;
}

static void TestModuleRegistry(void)
{
    s_flecsDestroyCount = 0;

    CelsEngine engine;
    CelsEngineInit(&engine, NULL);

    SDLTestModule sdl = {
        .windowWidth = 1920,
        .windowHeight = 1080
    };
    FlecsTestModule flecs = {
        .world = (void *)0xDEADBEEF,
        .reloadNotificationCount = 0
    };

    /* Register modules at the host engine level (.exe) */
    CEL_RegisterModule(&engine, SDLTestModule, &sdl);
    CEL_RegisterModuleWithHooks(&engine, FlecsTestModule, &flecs, NULL, FlecsTest_OnDestroy);

    /* Verify retrieval via CEL_GetModule on engine */
    SDLTestModule *retrievedSdl = CEL_GetModule(&engine, SDLTestModule);
    assert(retrievedSdl != NULL);
    assert(retrievedSdl->windowWidth == 1920);
    assert(retrievedSdl->windowHeight == 1080);

    /* Verify retrieval via CEL_GetModule on session (redirects to engine) */
    FlecsTestModule *retrievedFlecs = CEL_GetModule(&engine.session, FlecsTestModule);
    assert(retrievedFlecs != NULL);
    assert(retrievedFlecs->world == (void *)0xDEADBEEF);

    /* KEY ARCHITECTURAL PRINCIPLE: Subsystems in .exe survive session resets */
    CelsSessionDestroy(&engine.session);
    assert(s_flecsDestroyCount == 0); /* Module was NOT destroyed by session teardown! */

    CelsSessionInit(&engine.session, &(CelsSessionConfig){ .engine = &engine });
    SDLTestModule *sdlAfterReset = CEL_GetModule(&engine.session, SDLTestModule);
    assert(sdlAfterReset == &sdl);

    /* Subsystem modules are only destroyed when CelsEngineDestroy is invoked */
    CelsEngineDestroy(&engine);
    assert(s_flecsDestroyCount == 1);
}

CEL_Module(CustomAppTestModule) {
    int value;
};

static int s_testAppStartFired = 0;
static int s_testAppEndFired = 0;

CEL_Composition(TestAppRoot, void *userData) {
    (void)userData;
    CustomAppTestModule *mod = CEL_GetModule(CustomAppTestModule);
    assert(mod != NULL);
    assert(mod->value == 4242);
}

static void TestApp_OnStart(CelsEngine *engine, CelsSession *session)
{
    s_testAppStartFired++;
    static CustomAppTestModule mod = { .value = 4242 };
    CEL_RegisterModule(engine, CustomAppTestModule, &mod);
    cel_attach(session, TestAppRoot);
}

static void TestApp_OnEnd(CelsEngine *engine, CelsSession *session)
{
    (void)engine;
    (void)session;
    s_testAppEndFired++;
}

CEL_App_Manifest(TestAppDefinition,
    .onStart = TestApp_OnStart,
    .onEnd = TestApp_OnEnd
);

static void TestAppLifecycleAndReturnComposition(void)
{
    s_testAppStartFired = 0;
    s_testAppEndFired = 0;

    const CelsAppManifest *manifest = CelsGetAppManifest();
    assert(manifest != NULL);
    assert(strcmp(manifest->name, "TestAppDefinition") == 0);

    /* Test standalone runner with the manifest */
    assert(CelsEngineRunStandalone(manifest, NULL) == CELS_OK);

    assert(s_testAppStartFired == 1);
    assert(s_testAppEndFired == 1);
}

static void TestDynamicAppLoader(void)
{
    char resolvedPath[CELS_PATH_MAX] = {0};
    const char *foundPath = NULL;

    if (CelsResolveModulePath("test_fixture_app", resolvedPath, sizeof(resolvedPath))) {
        foundPath = resolvedPath;
    } else {
        const char *const modulePaths[] = {
            "build/debug/windows/libtest_fixture_app.dll",
            "build/debug/linux/libtest_fixture_app.so",
            "build/debug/macos/libtest_fixture_app.dylib",
            "../build/debug/windows/libtest_fixture_app.dll",
            "../build/debug/linux/libtest_fixture_app.so",
            "../build/debug/macos/libtest_fixture_app.dylib",
            "../../build/debug/windows/libtest_fixture_app.dll",
            "../../build/debug/linux/libtest_fixture_app.so",
            "../../build/debug/macos/libtest_fixture_app.dylib",
            "libtest_fixture_app.dll",
            "libtest_fixture_app.so",
            "libtest_fixture_app.dylib"
        };
        for (size_t i = 0; i < sizeof(modulePaths) / sizeof(modulePaths[0]); ++i) {
            FILE *f = fopen(modulePaths[i], "rb");
            if (f != NULL) {
                fclose(f);
                foundPath = modulePaths[i];
                break;
            }
        }
    }

    if (foundPath == NULL) {
        return;
    }

    CelsEngine engine;
    CelsEngineInit(&engine, NULL);

    CelsAppModule mod;
    bool loaded = CelsAppModuleLoad(&mod, foundPath, &engine.session);
    assert(loaded);
    assert(mod.isLoaded);
    assert(mod.manifest != NULL);

    /* Mounting attached composition from the DLL */
    assert(CelsSessionRecompose(&engine.session) == CELS_OK);
    assert(engine.session.hasComposedOnce);

    /* Test shadow copy exists on Windows */
#if defined(_WIN32)
    assert(mod.loadedPath[0] != '\0');
#endif

    CelsAppModuleUnload(&mod, &engine.session);
    assert(!mod.isLoaded);

    CelsEngineDestroy(&engine);
}

static int s_staticAppStartFired = 0;
static int s_staticAppEndFired = 0;

CEL_OnStart(StaticTestApp_OnStart)
{
    (void)engine;
    (void)session;
    s_staticAppStartFired++;
}

CEL_OnEnd(StaticTestApp_OnEnd)
{
    (void)engine;
    (void)session;
    s_staticAppEndFired++;
}

static const CelsAppDef s_staticAppDef = {
    .name = "StaticTestApp",
    .onStart = StaticTestApp_OnStart,
    .onEnd = StaticTestApp_OnEnd
};

static void TestStaticAppBinding(void)
{
    s_staticAppStartFired = 0;
    s_staticAppEndFired = 0;

    CelsEngine engine;
    CelsEngineInit(&engine, NULL);

    CelsApp app;
    assert(CelsAppBindStatic(&app, &engine, &engine.session, &s_staticAppDef) == CELS_OK);
    assert(app.isStatic);
    assert(app.module.isLoaded);
    assert(!app.isStarted);

    assert(CelsAppStart(&app) == CELS_OK);
    assert(app.isStarted);
    assert(s_staticAppStartFired == 1);

    CelsAppDestroy(&app);
    assert(!app.isStarted);
    assert(s_staticAppEndFired == 1);

    CelsEngineDestroy(&engine);
}

static void TestMultiDllAppHosting(void)
{
    char resolvedPath[CELS_PATH_MAX] = {0};
    const char *foundPath = NULL;

    if (CelsResolveModulePath("test_fixture_app", resolvedPath, sizeof(resolvedPath))) {
        foundPath = resolvedPath;
    } else {
        const char *const modulePaths[] = {
            "build/debug/windows/libtest_fixture_app.dll",
            "build/debug/linux/libtest_fixture_app.so",
            "build/debug/macos/libtest_fixture_app.dylib",
            "../build/debug/windows/libtest_fixture_app.dll",
            "../build/debug/linux/libtest_fixture_app.so",
            "../build/debug/macos/libtest_fixture_app.dylib",
            "libtest_fixture_app.dll"
        };
        for (size_t i = 0; i < sizeof(modulePaths) / sizeof(modulePaths[0]); ++i) {
            FILE *f = fopen(modulePaths[i], "rb");
            if (f != NULL) {
                fclose(f);
                foundPath = modulePaths[i];
                break;
            }
        }
    }

    if (foundPath == NULL) {
        return;
    }

    CelsEngine engine;
    CelsEngineInit(&engine, NULL);

    /* Test mode query and switching */
    assert(CelsEngineGetMode(&engine) == CELS_MODE_IMMEDIATE);
    CelsEngineSetMode(&engine, CELS_MODE_RETAINED);
    assert(CelsEngineGetMode(&engine) == CELS_MODE_RETAINED);
    CelsEngineSetMode(&engine, CELS_MODE_IMMEDIATE);
    assert(CelsEngineGetMode(&engine) == CELS_MODE_IMMEDIATE);

    /* Create a secondary named session on the engine */
    CelsSession *toolSession = CelsEngineCreateSession(&engine, "tool_window", CELS_PROFILE_DEFAULT);
    assert(toolSession != NULL);

    /* Host App 1 in primary session */
    CelsApp app1;
    assert(CelsAppLoad(&app1, &engine, &engine.session, foundPath) == CELS_OK);
    assert(app1.module.isLoaded);
    assert(!app1.isStarted);
    assert(CelsAppStart(&app1) == CELS_OK);
    assert(app1.isStarted);
    assert(engine.session.hasComposedOnce);

    /* Host App 2 in secondary session (multi-DLL hosting on same engine) */
    CelsApp app2;
    assert(CelsAppLoad(&app2, &engine, toolSession, foundPath) == CELS_OK);
    assert(app2.module.isLoaded);
    assert(!app2.isStarted);
    assert(CelsAppStart(&app2) == CELS_OK);
    assert(app2.isStarted);
    assert(toolSession->hasComposedOnce);

    /* Test CelsEngineNeedsRecompose in both modes */
    CelsEngineSetMode(&engine, CELS_MODE_IMMEDIATE);
    assert(CelsEngineNeedsRecompose(&engine) == true);
    CelsEngineSetMode(&engine, CELS_MODE_RETAINED);
    assert(CelsEngineNeedsRecompose(&engine) == false);

    /* Test check reload on both apps */
    assert(!CelsAppCheckReload(&app1));
    assert(!CelsAppCheckReload(&app2));

    /* Destroy hosted apps */
    CelsAppDestroy(&app1);
    assert(!app1.isStarted);
    assert(!app1.module.isLoaded);

    CelsAppDestroy(&app2);
    assert(!app2.isStarted);
    assert(!app2.module.isLoaded);

    CelsEngineDestroy(&engine);
}

static const TestCase s_hotReloadTests[] = {
    { "TestHotReloadInvalidation", "Verify hot reload invalidation flags and recomposition re-run", TestHotReloadInvalidation },
    { "TestHotReloadStateRetention", "Verify cel_remember and reactive state retention across reload", TestHotReloadStateRetention },
    { "TestHotReloadSchemaEvolution", "Verify slot schema size change recovery during reload", TestHotReloadSchemaEvolution },
    { "TestModuleRegistry", "Verify CEL_Module registration, retrieval, and survival across session restarts", TestModuleRegistry },
    { "TestAppLifecycleAndReturnComposition", "Verify CEL_App onStart/onEnd hooks, returning CEL_COMPOSITION, and custom module", TestAppLifecycleAndReturnComposition },
    { "TestDynamicAppLoader", "Verify loading, shadow copying, and unloading a dynamic CELS application module", TestDynamicAppLoader },
    { "TestStaticAppBinding", "Verify CelsAppBindStatic with monolithic CelsAppDef lifecycle hooks", TestStaticAppBinding },
    { "TestMultiDllAppHosting", "Verify multi-DLL hosting across primary and secondary sessions, mode switching, and recompose predicate", TestMultiDllAppHosting }
};

static const TestSuite s_hotReloadSuite = {
    .name = "hot_reload",
    .description = "CELS Hot-Reloading and dynamic state persistence tests",
    .tests = s_hotReloadTests,
    .testCount = sizeof(s_hotReloadTests) / sizeof(s_hotReloadTests[0])
};

const TestSuite *
GetHotReloadTestSuite(void)
{
    return &s_hotReloadSuite;
}

