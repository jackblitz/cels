#define CELS_IMPLEMENTATION
#include "cels.h"
#include <stdio.h>
#include <assert.h>

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

static void Texture_OnRemembered(EnemyTexture *self, CelsSession *s) {
    (void)s;
    self->gpuHandle = 0x55AA;
    self->isLoaded = true;
    g_texturesRemembered++;
    printf("  [Lifecycle Observer] Texture remembered (gpuHandle = 0x%X)\n", self->gpuHandle);
}

static void Texture_OnForgotten(EnemyTexture *self, CelsSession *s) {
    (void)s;
    g_texturesForgotten++;
    printf("  [Lifecycle Observer] Texture forgotten! Released 0x%X\n", self->gpuHandle);
    self->gpuHandle = 0;
    self->isLoaded = false;
}

/* ========================================================================= */
/* Top-Level Lifecycle Definition                                            */
/* ========================================================================= */

CEL_LifeCycle(EnemyLifeCycle, Enemy) {
    // cel_watch subscribes this composition key to changes in 'it'
    Enemy state = cel_watch(it);

    printf("  [EnemyLifeCycle] Evaluating Enemy -> HP: %d, isAlive: %d\n", state.hp, state.isAlive);

    if (state.hp <= 0 || !state.isAlive) {
        printf("  [EnemyLifeCycle] Fatal condition met -> triggering cel_destroy()\n");
        cel_destroy();
    }
}

/* ========================================================================= */
/* Composable Application Tree                                               */
/* ========================================================================= */

static Enemy g_goblin = { .hp = 100, .isAlive = true };
static int   g_renderCount = 0;

void GoblinApp(CelsSession *s) {
    CEL_Composition(Enemy, CEL_KEY("GoblinHost"), &g_goblin, EnemyLifeCycle) {
        g_renderCount++;
        printf("  [GoblinApp] Rendering Goblin! HP = %d (Render Pass #%d)\n", it->hp, g_renderCount);

        cel_remember_observer(s, EnemyTexture, Texture_OnRemembered, Texture_OnForgotten);

        CEL_Composable(CEL_KEY("HealthBar")) {
            printf("    [Child] Rendering HealthBar widget\n");
        }
    }
}

/* ========================================================================= */
/* Main Verification Test                                                    */
/* ========================================================================= */

int main(void) {
    printf("=====================================================\n");
    printf("CELS Top-Level Lifecycle & Composition Test Suite\n");
    printf("=====================================================\n\n");

    CelsSession session;
    CelsSessionInit(&session, &(CelsSessionConfig){
        .root = GoblinApp
    });

    /* --- Step 1: Initial Mount --- */
    printf("=== STEP 1: Initial Mount (Goblin HP = 100) ===\n");
    CelsResult res1 = CelsSessionRecompose(&session);
    assert(res1 == CELS_OK);
    assert(g_renderCount == 1);
    assert(g_texturesRemembered == 1);
    assert(g_texturesForgotten == 0);
    printf("PASS: Step 1 succeeded. Active groups in table: %u\n\n",
           CELS_MAX_GROUPS - (session.groupsGapEnd - session.groupsGapStart));

    /* --- Step 2: Non-fatal Mutation (HP: 100 -> 50) --- */
    printf("=== STEP 2: Non-fatal Mutation (Goblin HP: 100 -> 50) ===\n");
    cel_mutate(&session, &g_goblin) {
        this->hp = 50;
    }
    CelsResult res2 = CelsSessionRecompose(&session);
    assert(res2 == CELS_OK);
    assert(g_renderCount == 2);
    assert(g_texturesRemembered == 1); // Texture observer was preserved!
    assert(g_texturesForgotten == 0);
    printf("PASS: Step 2 succeeded. Recomposed surviving entity.\n\n");

    /* --- Step 3: Fatal Mutation (HP: 50 -> 0) -> cel_destroy() --- */
    printf("=== STEP 3: Fatal Mutation (Goblin HP: 50 -> 0) ===\n");
    cel_mutate(&session, &g_goblin) {
        this->hp = 0;
    }
    CelsResult res3 = CelsSessionRecompose(&session);
    assert(res3 == CELS_OK);
    // Composition body MUST NOT run during destruction!
    assert(g_renderCount == 2); 
    // Observer must be forgotten and cleaned up!
    assert(g_texturesForgotten == 1);
    // Groups must be pruned to 0!
    uint32_t activeGroups = CELS_MAX_GROUPS - (session.groupsGapEnd - session.groupsGapStart);
    assert(activeGroups == 0);
    printf("PASS: Step 3 succeeded. Entity destroyed, observers cleaned, slot tree pruned!\n\n");

    /* --- Step 4: Subsequent Quiet Recompose (Nothing to do) --- */
    printf("=== STEP 4: Quiet Check on Empty / Destroyed Tree ===\n");
    CelsResult res4 = CelsSessionRecompose(&session);
    assert(res4 == CELS_OK);
    assert(g_renderCount == 2);
    printf("PASS: Step 4 succeeded.\n\n");

    CelsSessionDestroy(&session);
    printf("ALL LIFECYCLE TESTS PASSED PERFECTLY!\n");
    return 0;
}
