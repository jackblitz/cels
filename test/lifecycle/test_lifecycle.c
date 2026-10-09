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

typedef struct FrameMetrics {
    int mountCount;
    int beforeChildrenCount;
    int afterChildrenCount;
    int unmountCount;
    uint32_t lastSettledChildren;
} FrameMetrics;

CEL_Lifecycle(MetricsLifecycle, FrameMetrics*, m) {
    mount {
        m->mountCount++;
    }
    before_children {
        m->beforeChildrenCount++;
    }
    after_children(childCount) {
        m->afterChildrenCount++;
        m->lastSettledChildren = childCount;
    }
    unmount {
        m->unmountCount++;
    }
}

CEL_Composable(LeafChild, int, id) {
    (void)id;
}

CEL_State(MetricsRootState) {
    int childCount;
    bool isVisible;
};

CEL_Composable(MetricsParentComposable, FrameMetrics*, m) {
    cel_lifecycle(MetricsLifecycle, m);
    const MetricsRootState *st = cel_watch(MetricsRootState, CEL_ID("MetricsState"));
    if (st != NULL) {
        for (int i = 0; i < st->childCount; ++i) {
            LeafChild(i);
        }
    }
}

CEL_Composition(MetricsRootApp, void*, userData) {
    FrameMetrics *m = (FrameMetrics*)userData;
    const MetricsRootState *st = cel_watch(MetricsRootState, CEL_ID("MetricsState"));
    if (st != NULL && st->isVisible) {
        MetricsParentComposable(m);
    }
}

static void TestBeforeAndAfterChildrenPhases(void)
{
    FrameMetrics m = {0};

    CelsSession session;
    CelsSessionInit(&session, NULL);

    MetricsRootState initState = { .childCount = 4, .isVisible = true };
    CelsSessionRememberState(&session, CEL_ID("MetricsState"), sizeof(MetricsRootState), &initState);

    cel_attach(&session, MetricsRootApp, NULL, &m);

    /* Frame 0: First mount pass with 4 children */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(m.mountCount == 1);
    assert(m.beforeChildrenCount == 1);
    assert(m.afterChildrenCount == 1);
    assert(m.lastSettledChildren == 4);
    assert(m.unmountCount == 0);

    /* Frame 1: Recompose with 7 children */
    cels_session_mutate(&session, CEL_ID("MetricsState"), MetricsRootState) {
        this->childCount = 7;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(m.mountCount == 1);          /* Mount only runs once on fresh mount */
    assert(m.beforeChildrenCount == 2); /* before_children runs every frame before children */
    assert(m.afterChildrenCount == 2);  /* after_children runs every frame after children */
    assert(m.lastSettledChildren == 7); /* after_children receives the exact settled child count */
    assert(m.unmountCount == 0);

    /* Frame 2: Hide parent -> unmounts */
    cels_session_mutate(&session, CEL_ID("MetricsState"), MetricsRootState) {
        this->isVisible = false;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(m.mountCount == 1);
    assert(m.beforeChildrenCount == 2);
    assert(m.afterChildrenCount == 2);
    assert(m.unmountCount == 1); /* Unmount runs when node is pruned */

    CelsSessionDestroy(&session);
}

typedef struct InlineMetrics {
    int attachCount;
    int enterCount;
    int exitCount;
    int lastChildCount;
    int detachCount;
} InlineMetrics;

static InlineMetrics s_inlineMetrics = {0};

CEL_Composable(TestInlineLeaf, int, val) {
    (void)val;
}

CEL_Composable(TestInlineContainer, int, multiplier) {
    on_attach {
        s_inlineMetrics.attachCount++;
    }

    enter {
        s_inlineMetrics.enterCount++;
    }

    on_exit(childCount) {
        s_inlineMetrics.exitCount++;
        s_inlineMetrics.lastChildCount = (int)childCount * multiplier;
    }

    on_detach {
        s_inlineMetrics.detachCount++;
    }
}

CEL_State(InlineRootState) {
    int count;
    bool visible;
};

CEL_Composition(InlineTestApp, void *userData) {
    (void)userData;
    const InlineRootState *st = cel_watch_state(InlineRootState);
    if (st && st->visible) {
        cel_scope(TestInlineContainer, 2) {
            for (int i = 0; i < st->count; i++) {
                TestInlineLeaf(i);
            }
        }
    }
}

static void TestInlineComposableLifecycle(void) {
    memset(&s_inlineMetrics, 0, sizeof(s_inlineMetrics));

    CelsSession session;
    CelsSessionInit(&session, NULL);

    InlineRootState initState = { .count = 3, .visible = true };
    CelsSessionRememberState(&session, CelsHashKey("InlineRootState"), sizeof(InlineRootState), &initState);

    cel_attach(&session, InlineTestApp, NULL, NULL);

    /* Pass 1: Fresh mount with 3 children */
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_inlineMetrics.attachCount == 1);
    assert(s_inlineMetrics.enterCount == 1);
    assert(s_inlineMetrics.exitCount == 1);
    assert(s_inlineMetrics.lastChildCount == 6); /* 3 * 2 */
    assert(s_inlineMetrics.detachCount == 0);

    /* Pass 2: Recompose with 5 children */
    cels_session_mutate(&session, InlineRootState) {
        this->count = 5;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_inlineMetrics.attachCount == 1);    /* on_attach runs only on fresh mount */
    assert(s_inlineMetrics.enterCount == 2);     /* scope entry runs every execution */
    assert(s_inlineMetrics.exitCount == 2);      /* on_exit runs at scope exit */
    assert(s_inlineMetrics.lastChildCount == 10); /* 5 * 2 */
    assert(s_inlineMetrics.detachCount == 0);

    /* Pass 3: Hide container -> pruned from tree */
    cels_session_mutate(&session, InlineRootState) {
        this->visible = false;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(s_inlineMetrics.attachCount == 1);
    assert(s_inlineMetrics.enterCount == 2);
    assert(s_inlineMetrics.exitCount == 2);
    assert(s_inlineMetrics.detachCount == 1);    /* on_detach runs when omitted */

    CelsSessionDestroy(&session);
}

static const TestCase s_lifecycleTests[] = {
    { "TestInitialMount", "Initial mount and observer attachment", TestInitialMount },
    { "TestNonFatalMutation", "Non-fatal state mutation preserving observers", TestNonFatalMutation },
    { "TestFatalMutationAndDestroy", "Fatal mutation triggering cel_destroy and observer cleanup", TestFatalMutationAndDestroy },
    { "TestSubsequentQuietRecompose", "Quiet recomposition check on pruned empty tree", TestSubsequentQuietRecompose },
    { "TestFullLifecycleProgression", "Complete multi-step entity lifecycle progression flow", TestFullLifecycleProgression },
    { "TestEngineEvaluationTeardownTriggerQuit", "Root composition evaluation destruction triggers engine shouldQuit", TestEngineEvaluationTeardownTriggerQuit },
    { "TestChildLifecycleDecoupledFromRootEvaluation", "Child component mounts/unmounts independently of root window evaluation teardown", TestChildLifecycleDecoupledFromRootEvaluation },
    { "TestBeforeAndAfterChildrenPhases", "CEL_Lifecycle before_children frame hook and after_children(childCount) child settlement reflection", TestBeforeAndAfterChildrenPhases },
    { "TestInlineComposableLifecycle", "Inline CEL_Composable on_attach, on_exit(childCount), and on_detach hooks", TestInlineComposableLifecycle }
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

