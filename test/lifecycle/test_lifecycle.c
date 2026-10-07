#ifdef NDEBUG
#undef NDEBUG
#endif
#include "cels.h"
#include "cli/test_cli.h"

#include <assert.h>
#include <stdio.h>

/* ========================================================================= */
/* Reactive Entity State & Observer Resources                                */
/* ========================================================================= */

CEL_State(Enemy) {
    int  hp;
    bool isAlive;
};

CEL_State(EnemyTexture) {
    int  gpuHandle;
    bool isLoaded;
};

static int g_texturesRemembered = 0;
static int g_texturesForgotten = 0;
static int g_renderCount = 0;

CEL_Lifecycle(TextureLifecycle, EnemyTexture *self) {
    mount {
        self->gpuHandle = 0x55AA;
        self->isLoaded = true;
        g_texturesRemembered++;
    }
    unmount {
        g_texturesForgotten++;
        self->gpuHandle = 0;
        self->isLoaded = false;
    }
}

CEL_EvaluateFn(EnemyEval, void*, ctx) {
    (void)ctx;
    const Enemy *state = cel_get_state_keyed(CEL_ID("Goblin"), Enemy);
    if (state != NULL && (state->hp <= 0 || !state->isAlive)) {
        return false;
    }
    return true;
}

CEL_Composition(GoblinApp, void *userData) {
    (void)userData;
    g_renderCount++;
    const Enemy *state = cel_watch(Enemy, CEL_ID("Goblin"));
    (void)state;
    EnemyTexture *tex = cel_remember(EnemyTexture, 0);
    cel_lifecycle(TextureLifecycle, tex);
}

static void ResetLifecycleState(CelsSession *s) {
    g_renderCount = 0;
    g_texturesRemembered = 0;
    g_texturesForgotten = 0;
    CelsSessionRememberState(s, CEL_ID("Goblin"), sizeof(Enemy), &((Enemy){ .hp = 100, .isAlive = true }));
    cels_session_mutate(s, CEL_ID("Goblin"), Enemy) {
        this->hp = 100;
        this->isAlive = true;
    }
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

static void TestInitialMount(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);
    ResetLifecycleState(&session);
    cel_attach(&session, GoblinApp, EnemyEval);

    CelsResult res1 = CelsSessionRecompose(&session);
    assert(res1 == CELS_OK);
    assert(g_renderCount == 1);
    assert(g_texturesRemembered == 1);
    assert(g_texturesForgotten == 0);

    CelsSessionDestroy(&session);
}

static void TestNonFatalMutation(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);
    ResetLifecycleState(&session);
    cel_attach(&session, GoblinApp, EnemyEval);

    CelsResult res1 = CelsSessionRecompose(&session);
    assert(res1 == CELS_OK);

    cels_session_mutate(&session, CEL_ID("Goblin"), Enemy) {
        this->hp = 50;
    }

    CelsResult res2 = CelsSessionRecompose(&session);
    assert(res2 == CELS_OK);
    assert(g_renderCount == 2);
    assert(g_texturesRemembered == 1);
    assert(g_texturesForgotten == 0);

    CelsSessionDestroy(&session);
}

static void TestFatalMutationAndDestroy(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);
    ResetLifecycleState(&session);
    cel_attach(&session, GoblinApp, EnemyEval);

    assert(CelsSessionRecompose(&session) == CELS_OK);

    cels_session_mutate(&session, CEL_ID("Goblin"), Enemy) {
        this->hp = 0;
    }

    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(g_renderCount == 1);
    assert(g_texturesForgotten == 1);
    uint32_t activeGroups = session.maxGroups - (session.groupsGapEnd - session.groupsGapStart);
    assert(activeGroups == 0);

    CelsSessionDestroy(&session);
}

static void TestSubsequentQuietRecompose(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);
    ResetLifecycleState(&session);
    cel_attach(&session, GoblinApp, EnemyEval);

    assert(CelsSessionRecompose(&session) == CELS_OK);

    cels_session_mutate(&session, CEL_ID("Goblin"), Enemy) {
        this->hp = 0;
    }

    assert(CelsSessionRecompose(&session) == CELS_OK);

    CelsResult resQuiet = CelsSessionRecompose(&session);
    assert(resQuiet == CELS_OK);
    assert(g_renderCount == 1);

    CelsSessionDestroy(&session);
}

static void TestFullLifecycleProgression(void) {
    CelsSession session;
    CelsSessionInit(&session, NULL);
    ResetLifecycleState(&session);
    cel_attach(&session, GoblinApp, EnemyEval);

    /* Step 1: Initial Mount */
    CelsResult res1 = CelsSessionRecompose(&session);
    assert(res1 == CELS_OK);
    assert(g_renderCount == 1);
    assert(g_texturesRemembered == 1);
    assert(g_texturesForgotten == 0);

    /* Step 2: Non-fatal Mutation */
    cels_session_mutate(&session, CEL_ID("Goblin"), Enemy) {
        this->hp = 50;
    }

    CelsResult res2 = CelsSessionRecompose(&session);
    assert(res2 == CELS_OK);
    assert(g_renderCount == 2);
    assert(g_texturesRemembered == 1);
    assert(g_texturesForgotten == 0);

    /* Step 3: Fatal Mutation */
    cels_session_mutate(&session, CEL_ID("Goblin"), Enemy) {
        this->hp = 0;
    }

    CelsResult res3 = CelsSessionRecompose(&session);
    assert(res3 == CELS_OK);
    assert(g_renderCount == 2);
    assert(g_texturesForgotten == 1);
    uint32_t activeGroups = session.maxGroups - (session.groupsGapEnd - session.groupsGapStart);
    assert(activeGroups == 0);

    /* Step 4: Subsequent Quiet Recompose */
    CelsResult res4 = CelsSessionRecompose(&session);
    assert(res4 == CELS_OK);
    assert(g_renderCount == 2);

    CelsSessionDestroy(&session);
}

static void TestEngineEvaluationTeardownTriggerQuit(void) {
    CelsEngine engine;
    CelsEngineInit(&engine, NULL);
    ResetLifecycleState(&engine.session);

    cel_attach(&engine.session, GoblinApp, EnemyEval);

    /* Initial composition pass */
    assert(CelsSessionRecompose(&engine.session) == CELS_OK);
    assert(!engine.shouldQuit);

    /* Mutate state to trigger evaluation failure (teardown) */
    cels_session_mutate(&engine.session, CEL_ID("Goblin"), Enemy) {
        this->hp = 0;
    }

    /* Recomposition evaluates EnemyEval -> false -> prunes subtree and sets engine.shouldQuit = true */
    assert(CelsSessionRecompose(&engine.session) == CELS_OK);
    assert(engine.shouldQuit);

    /* Test imperative cel_quit() */
    engine.shouldQuit = false;
    CelsEngineQuit(&engine);
    assert(engine.shouldQuit);

    CelsEngineDestroy(&engine);
}

CEL_State(AppWindowTestState) {
    bool isOpen;
    bool showBadge;
};

static int s_badgeMountCount = 0;
static int s_badgeUnmountCount = 0;

typedef struct TestBadgeResource {
    int id;
} TestBadgeResource;

CEL_Lifecycle(TestBadgeLifecycle, TestBadgeResource *b) {
    mount {
        b->id = 777;
        s_badgeMountCount++;
    }
    unmount {
        b->id = 0;
        s_badgeUnmountCount++;
    }
}

CEL_Composable(TestBadgeComposable) {
    TestBadgeResource *res = cel_remember(TestBadgeResource, 0);
    cel_lifecycle(TestBadgeLifecycle, res);
}

CEL_Composable(TestWindowBody) {
    const AppWindowTestState *st = cel_watch_state(AppWindowTestState);
    if (st && st->showBadge) {
        TestBadgeComposable();
    }
}

CEL_EvaluateFn(TestWindowEval, void*, ctx) {
    (void)ctx;
    const AppWindowTestState *st = cel_get_state(AppWindowTestState);
    return (st == NULL || st->isOpen);
}

CEL_Composition(TestWindowComposition, void *userData) {
    (void)userData;
    cel_remember_state(AppWindowTestState, {
        .isOpen = true,
        .showBadge = true
    });
    TestWindowBody();
}

static void TestChildLifecycleDecoupledFromRootEvaluation(void) {
    s_badgeMountCount = 0;
    s_badgeUnmountCount = 0;

    CelsEngine engine;
    CelsEngineInit(&engine, NULL);

    cel_attach(&engine.session, TestWindowComposition, TestWindowEval);

    /* 1. Initial mount: window is open, badge is shown */
    assert(CelsSessionRecompose(&engine.session) == CELS_OK);
    assert(s_badgeMountCount == 1);
    assert(s_badgeUnmountCount == 0);
    assert(!engine.shouldQuit);

    /* 2. Toggle showBadge = false. Badge unmounts via lifecycle, but window stays open! */
    cels_session_mutate(&engine.session, AppWindowTestState) {
        this->showBadge = false;
    }

    assert(CelsSessionRecompose(&engine.session) == CELS_OK);
    assert(s_badgeMountCount == 1);
    assert(s_badgeUnmountCount == 1);
    assert(!engine.shouldQuit);

    /* 3. Re-enable showBadge = true. Badge mounts again */
    cels_session_mutate(&engine.session, AppWindowTestState) {
        this->showBadge = true;
    }

    assert(CelsSessionRecompose(&engine.session) == CELS_OK);
    assert(s_badgeMountCount == 2);
    assert(s_badgeUnmountCount == 1);
    assert(!engine.shouldQuit);

    /* 4. Close window: isOpen = false. WindowEval returns false -> root window destroyed -> engine.shouldQuit = true */
    cels_session_mutate(&engine.session, AppWindowTestState) {
        this->isOpen = false;
    }

    assert(CelsSessionRecompose(&engine.session) == CELS_OK);
    assert(s_badgeUnmountCount == 2);
    assert(engine.shouldQuit);

    CelsEngineDestroy(&engine);
}

static const TestCase s_lifecycleTests[] = {
    { "TestInitialMount", "Initial mount and observer attachment", TestInitialMount },
    { "TestNonFatalMutation", "Non-fatal state mutation preserving observers", TestNonFatalMutation },
    { "TestFatalMutationAndDestroy", "Fatal mutation triggering cel_destroy and observer cleanup", TestFatalMutationAndDestroy },
    { "TestSubsequentQuietRecompose", "Quiet recomposition check on pruned empty tree", TestSubsequentQuietRecompose },
    { "TestFullLifecycleProgression", "Complete multi-step entity lifecycle progression flow", TestFullLifecycleProgression },
    { "TestEngineEvaluationTeardownTriggerQuit", "Root composition evaluation destruction triggers engine shouldQuit", TestEngineEvaluationTeardownTriggerQuit },
    { "TestChildLifecycleDecoupledFromRootEvaluation", "Child component mounts/unmounts independently of root window evaluation teardown", TestChildLifecycleDecoupledFromRootEvaluation }
};

static const TestSuite s_lifecycleSuite = {
    .name = "lifecycle",
    .description = "CEL_Lifecycle observers, reactive mutation, and CEL_Evaluate teardown",
    .tests = s_lifecycleTests,
    .testCount = sizeof(s_lifecycleTests) / sizeof(s_lifecycleTests[0])
};

const TestSuite *GetLifecycleTestSuite(void) {
    return &s_lifecycleSuite;
}
