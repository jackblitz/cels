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

CEL_Observer(EnemyTexture) {
    int  gpuHandle;
    bool isLoaded;
};

static int g_texturesRemembered = 0;
static int g_texturesForgotten = 0;
static int g_renderCount = 0;
static Enemy g_goblin = { .hp = 100, .isAlive = true };

static void Texture_OnRemembered(EnemyTexture *self, CelsSession *s) {
    (void)s;
    self->gpuHandle = 0x55AA;
    self->isLoaded = true;
    g_texturesRemembered++;
}

static void Texture_OnForgotten(EnemyTexture *self, CelsSession *s) {
    (void)s;
    g_texturesForgotten++;
    self->gpuHandle = 0;
    self->isLoaded = false;
}

/* ========================================================================= */
/* Top-Level Lifecycle Definition                                            */
/* ========================================================================= */

CEL_LifeCycle(EnemyLifeCycle, Enemy) {
    Enemy state = cel_watch(it);
    if (state.hp <= 0 || !state.isAlive) {
        cel_destroy();
    }
}

static void GoblinApp(CelsSession *s) {
    CEL_Composition(Enemy, &g_goblin, EnemyLifeCycle) {
        g_renderCount++;
        cel_remember_observer(s, EnemyTexture, Texture_OnRemembered, Texture_OnForgotten);

        CEL_Composable(CEL_AUTO_KEY()) {
        }
    }
}

static void ResetLifecycleState(void) {
    g_goblin.hp = 100;
    g_goblin.isAlive = true;
    g_renderCount = 0;
    g_texturesRemembered = 0;
    g_texturesForgotten = 0;
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

static void TestInitialMount(void) {
    ResetLifecycleState();
    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = GoblinApp
    });

    CelsResult res1 = CelsSessionRecompose(&session);
    assert(res1 == CELS_OK);
    assert(g_renderCount == 1);
    assert(g_texturesRemembered == 1);
    assert(g_texturesForgotten == 0);

    CelsSessionDestroy(&session);
}

static void TestNonFatalMutation(void) {
    ResetLifecycleState();
    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = GoblinApp
    });

    CelsResult res1 = CelsSessionRecompose(&session);
    assert(res1 == CELS_OK);

    cel_mutate(&session, &g_goblin) {
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
    ResetLifecycleState();
    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = GoblinApp
    });

    assert(CelsSessionRecompose(&session) == CELS_OK);

    cel_mutate(&session, &g_goblin) {
        this->hp = 0;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);
    assert(g_renderCount == 1);
    assert(g_texturesForgotten == 1);
    uint32_t activeGroups = CELS_MAX_GROUPS - (session.groupsGapEnd - session.groupsGapStart);
    assert(activeGroups == 0);

    CelsSessionDestroy(&session);
}

static void TestSubsequentQuietRecompose(void) {
    ResetLifecycleState();
    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = GoblinApp
    });

    assert(CelsSessionRecompose(&session) == CELS_OK);
    cel_mutate(&session, &g_goblin) {
        this->hp = 0;
    }
    assert(CelsSessionRecompose(&session) == CELS_OK);

    CelsResult resQuiet = CelsSessionRecompose(&session);
    assert(resQuiet == CELS_OK);
    assert(g_renderCount == 1);

    CelsSessionDestroy(&session);
}

static void TestFullLifecycleProgression(void) {
    ResetLifecycleState();
    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = GoblinApp
    });

    /* Step 1: Initial Mount */
    CelsResult res1 = CelsSessionRecompose(&session);
    assert(res1 == CELS_OK);
    assert(g_renderCount == 1);
    assert(g_texturesRemembered == 1);
    assert(g_texturesForgotten == 0);

    /* Step 2: Non-fatal Mutation */
    cel_mutate(&session, &g_goblin) {
        this->hp = 50;
    }
    CelsResult res2 = CelsSessionRecompose(&session);
    assert(res2 == CELS_OK);
    assert(g_renderCount == 2);
    assert(g_texturesRemembered == 1);
    assert(g_texturesForgotten == 0);

    /* Step 3: Fatal Mutation */
    cel_mutate(&session, &g_goblin) {
        this->hp = 0;
    }
    CelsResult res3 = CelsSessionRecompose(&session);
    assert(res3 == CELS_OK);
    assert(g_renderCount == 2);
    assert(g_texturesForgotten == 1);
    uint32_t activeGroups = CELS_MAX_GROUPS - (session.groupsGapEnd - session.groupsGapStart);
    assert(activeGroups == 0);

    /* Step 4: Subsequent Quiet Recompose */
    CelsResult res4 = CelsSessionRecompose(&session);
    assert(res4 == CELS_OK);
    assert(g_renderCount == 2);

    CelsSessionDestroy(&session);
}

static const TestCase s_lifecycleTests[] = {
    { "TestInitialMount", "Initial mount and observer attachment", TestInitialMount },
    { "TestNonFatalMutation", "Non-fatal state mutation preserving observers", TestNonFatalMutation },
    { "TestFatalMutationAndDestroy", "Fatal mutation triggering cel_destroy and observer cleanup", TestFatalMutationAndDestroy },
    { "TestSubsequentQuietRecompose", "Quiet recomposition check on pruned empty tree", TestSubsequentQuietRecompose },
    { "TestFullLifecycleProgression", "Complete multi-step entity lifecycle progression flow", TestFullLifecycleProgression }
};

static const TestSuite s_lifecycleSuite = {
    .name = "lifecycle",
    .description = "CEL_LifeCycle observers, reactive mutation, and cel_destroy teardown",
    .tests = s_lifecycleTests,
    .testCount = sizeof(s_lifecycleTests) / sizeof(s_lifecycleTests[0])
};

const TestSuite *GetLifecycleTestSuite(void) {
    return &s_lifecycleSuite;
}
