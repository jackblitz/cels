#include "cels.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flecs.h"

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("\n[ASSERTION FAILED]: %s at %s:%d\n",                      \
                   #cond, __FILE__, __LINE__);                                 \
            fflush(stdout);                                                    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/* ========================================================================= */
/* Component Definitions using CEL_Component                                 */
/* ========================================================================= */

CEL_Component(Position) {
    float x;
    float y;
};

CEL_Component(Health) {
    int32_t current;
    int32_t max;
};

CEL_Component(IsHeader) {
    char dummy;
};

CEL_Component(IsPlayer) {
    char dummy;
};

/* ========================================================================= */
/* Composable Root View & State                                              */
/* ========================================================================= */

static bool g_showSubmenu = false;
static float g_playerX = 100.0f;
static float g_playerY = 200.0f;
static int32_t g_playerHp = 100;

CEL_CompositionScope
MyRootView(void)
{
    CEL_Composition(CEL_Name("AppRoot")) {
        CEL_Compose(CEL_Name("Header")) {
            CEL_Has(Position, { .x = 0.0f, .y = 0.0f });
            CEL_Tag(IsHeader);

            CEL_Compose(CEL_Name("Title")) {
                // Nested child of Header
            }
        }

        CEL_Compose(CEL_Name("PlayerHud")) {
            CEL_Has(Position, { .x = g_playerX, .y = g_playerY });
            CEL_Has(Health, { .current = g_playerHp, .max = 100 });
            CEL_Tag(IsPlayer);
        }

        if (g_showSubmenu) {
            CEL_Compose(CEL_Name("Submenu")) {
                CEL_Has(Position, { .x = 50.0f, .y = 50.0f });
            }
        }
    }
    return CEL_CompositionDone();
}

/* ========================================================================= */
/* Test Main                                                                 */
/* ========================================================================= */

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("==============================================================\n");
    printf("  CELS Task 4: CelsSession & Flecs Entity Lifecycle Test      \n");
    printf("==============================================================\n\n");

    // 1. Initialize Flecs world
    printf("[Step 1] Initializing developer-owned Flecs world...\n");
    ecs_world_t *world = ecs_init();
    TEST_ASSERT(world != NULL);

    // 2. Register components with Flecs
    printf("[Step 2] Registering components with CEL_RegisterComponent...\n");
    CEL_RegisterComponent(world, Position);
    CEL_RegisterComponent(world, Health);
    CEL_RegisterComponent(world, IsHeader);
    CEL_RegisterComponent(world, IsPlayer);

    // 3. Initialize CelsSession
    printf("[Step 3] Initializing CelsSession with 4 workers and MyRootView...\n");
    CelsSession session;
    const CelsResult initRes = CelsSessionInit(&session, world, &(CelsSessionConfig){
        .workerCount = 4,
        .compositionScope = MyRootView,
        .slabSize = 4096,
        .maxGroups = 32
    });
    TEST_ASSERT(initRes == CELS_OK);
    TEST_ASSERT(session.world == world);
    TEST_ASSERT(session.rootEntity != 0);
    TEST_ASSERT(session.compositionScope == MyRootView);
    TEST_ASSERT(session.slabSize == 4096);
    TEST_ASSERT(CelsSessionGetSlabSize(&session) == 4096);
    TEST_ASSERT(CelsSessionIsDirty(&session) == true);

    CEL_CompositionScope preScope = CelsSessionGetCompositionScope(&session);
    TEST_ASSERT(preScope.entity == session.rootEntity);
    TEST_ASSERT(preScope.groupCount == 0);

    // 4. Frame 1: Initial composition
    printf("[Step 4] Frame 1: Progressing world (initial composition)...\n");
    bool progressOk = ecs_progress(world, 0.016f);
    TEST_ASSERT(progressOk);
    TEST_ASSERT(CelsSessionIsDirty(&session) == false);

    // Verify entity creation and naming
    ecs_entity_t appRoot = ecs_lookup(world, "AppRoot");
    TEST_ASSERT(appRoot != 0);
    printf("  -> Found entity 'AppRoot' (id: %llu)\n", (unsigned long long)appRoot);

    CEL_CompositionScope postScope = CelsSessionGetCompositionScope(&session);
    TEST_ASSERT(postScope.entity == appRoot);
    TEST_ASSERT(postScope.groupCount == 4);

    ecs_entity_t header = ecs_lookup_child(world, appRoot, "Header");
    TEST_ASSERT(header != 0);
    printf("  -> Found child entity 'Header' (id: %llu)\n", (unsigned long long)header);

    ecs_entity_t title = ecs_lookup_child(world, header, "Title");
    TEST_ASSERT(title != 0);
    printf("  -> Found grandchild entity 'Title' (id: %llu)\n", (unsigned long long)title);

    ecs_entity_t playerHud = ecs_lookup_child(world, appRoot, "PlayerHud");
    TEST_ASSERT(playerHud != 0);
    printf("  -> Found child entity 'PlayerHud' (id: %llu)\n", (unsigned long long)playerHud);

    // Verify hierarchy parenting
    TEST_ASSERT(ecs_get_parent(world, header) == appRoot);
    TEST_ASSERT(ecs_get_parent(world, title) == header);
    TEST_ASSERT(ecs_get_parent(world, playerHud) == appRoot);

    // Verify components set via CEL_Has
    const Position *posHeader = ecs_get(world, header, Position);
    TEST_ASSERT(posHeader != NULL);
    TEST_ASSERT(posHeader->x == 0.0f && posHeader->y == 0.0f);

    const Position *posPlayer = ecs_get(world, playerHud, Position);
    TEST_ASSERT(posPlayer != NULL);
    TEST_ASSERT(posPlayer->x == 100.0f && posPlayer->y == 200.0f);

    const Health *hpPlayer = ecs_get(world, playerHud, Health);
    TEST_ASSERT(hpPlayer != NULL);
    TEST_ASSERT(hpPlayer->current == 100 && hpPlayer->max == 100);

    // Verify tags set via CEL_Tag
    TEST_ASSERT(ecs_has(world, header, IsHeader));
    TEST_ASSERT(ecs_has(world, playerHud, IsPlayer));

    // Verify Submenu was not rendered
    ecs_entity_t submenu = ecs_lookup_child(world, appRoot, "Submenu");
    TEST_ASSERT(submenu == 0);

    // 5. Frame 2: Recomposition with entity reuse (cache hit)
    printf("\n[Step 5] Frame 2: Recomposition entity reuse test...\n");
    CelsSessionMarkDirty(&session);
    TEST_ASSERT(CelsSessionIsDirty(&session) == true);

    progressOk = ecs_progress(world, 0.016f);
    TEST_ASSERT(progressOk);

    // Verify entities are identical (reused from slot table, not re-allocated)
    TEST_ASSERT(ecs_lookup(world, "AppRoot") == appRoot);
    TEST_ASSERT(ecs_lookup_child(world, appRoot, "Header") == header);
    TEST_ASSERT(ecs_lookup_child(world, header, "Title") == title);
    TEST_ASSERT(ecs_lookup_child(world, appRoot, "PlayerHud") == playerHud);
    printf("  CONFIRMED: All Flecs entity IDs were preserved across recomposition!\n");

    // 6. Frame 3: Branch addition
    printf("\n[Step 6] Frame 3: Dynamic branch insertion (Submenu)...\n");
    g_showSubmenu = true;
    g_playerX = 150.0f;
    g_playerHp = 75;
    CelsSessionMarkDirty(&session);

    progressOk = ecs_progress(world, 0.016f);
    TEST_ASSERT(progressOk);

    submenu = ecs_lookup_child(world, appRoot, "Submenu");
    TEST_ASSERT(submenu != 0);
    printf("  -> Dynamic branch 'Submenu' created (id: %llu)\n", (unsigned long long)submenu);
    TEST_ASSERT(ecs_get_parent(world, submenu) == appRoot);

    const Position *posSubmenu = ecs_get(world, submenu, Position);
    TEST_ASSERT(posSubmenu != NULL);
    TEST_ASSERT(posSubmenu->x == 50.0f && posSubmenu->y == 50.0f);

    // Verify updated component values on existing entity
    posPlayer = ecs_get(world, playerHud, Position);
    TEST_ASSERT(posPlayer != NULL);
    TEST_ASSERT(posPlayer->x == 150.0f);

    hpPlayer = ecs_get(world, playerHud, Health);
    TEST_ASSERT(hpPlayer != NULL);
    TEST_ASSERT(hpPlayer->current == 75);

    // 7. Frame 4: Vanishing branch pruning
    printf("\n[Step 7] Frame 4: Vanishing branch pruning test...\n");
    g_showSubmenu = false;
    CelsSessionMarkDirty(&session);

    progressOk = ecs_progress(world, 0.016f);
    TEST_ASSERT(progressOk);

    // Verify Submenu entity was automatically deleted from Flecs world
    TEST_ASSERT(!ecs_is_alive(world, submenu));
    TEST_ASSERT(ecs_lookup_child(world, appRoot, "Submenu") == 0);
    printf("  CONFIRMED: 'Submenu' entity automatically pruned on branch exit!\n");

    // 8. Cleanup
    printf("\n[Step 8] Destroying CelsSession and Flecs world...\n");
    CelsSessionDestroy(&session);
    ecs_fini(world);

    printf("\n==============================================================\n");
    printf("  ALL CELS SESSION TESTS PASSED SUCCESSFULLY!                 \n");
    printf("==============================================================\n");

    return 0;
}
