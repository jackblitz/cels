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
/* Synchronous recompose pass: state, root view and callbacks                */
/* ========================================================================= */

static uint32_t g_seqRootRuns = 0;
static uint32_t g_seqChildRuns = 0;
static uint32_t g_seqLeafRuns = 0;
static bool g_seqComposeEnabled = true;
static bool g_seqContextSeen = false;
static bool g_seqCreateInline = false;
static bool g_seqNotifyProbe = false;
static uint16_t g_seqChildFlags = 0;
static uint16_t g_seqLeafFlags = 0;
static uint32_t g_seqSelfInvalidations = 0;
static uint32_t g_seqCreateCount = 0;
static uint32_t g_seqDestroyCount = 0;
static uint32_t g_seqCreateKey = 0;
static CelsComposableId g_seqCreateComposable = CELS_COMPOSABLE_ID_INVALID;
static CelsComposableId g_seqCreateParent = CELS_COMPOSABLE_ID_INVALID;
static CelsComposableId g_seqDestroyComposable = CELS_COMPOSABLE_ID_INVALID;

/**
 * Records a mount reported through the session's transaction context.
 */
static void
SeqOnCreate(CelsComposableId composable,
            CelsComposableId parent,
            uint32_t key,
            void *userdata)
{
    (void)userdata;
    g_seqCreateCount++;
    g_seqCreateComposable = composable;
    g_seqCreateParent = parent;
    g_seqCreateKey = key;
}

/**
 * Records a prune reported through the session's transaction context.
 */
static void
SeqOnDestroy(CelsComposableId composable, void *userdata)
{
    (void)userdata;
    g_seqDestroyCount++;
    g_seqDestroyComposable = composable;
}

/**
 * Resets every counter the recompose suites assert on.
 */
static void
SeqCountersReset(void)
{
    g_seqRootRuns = 0;
    g_seqChildRuns = 0;
    g_seqLeafRuns = 0;
    g_seqContextSeen = false;
    g_seqChildFlags = 0;
    g_seqLeafFlags = 0;
    g_seqCreateCount = 0;
    g_seqDestroyCount = 0;
    g_seqCreateComposable = CELS_COMPOSABLE_ID_INVALID;
    g_seqCreateParent = CELS_COMPOSABLE_ID_INVALID;
    g_seqDestroyComposable = CELS_COMPOSABLE_ID_INVALID;
}

/**
 * Root view for the sequential pass: SeqRoot -> SeqChild -> SeqLeaf.
 *
 * The root body snapshots the flags of the two groups BELOW it, which the walk
 * has not entered yet. That is the only stable vantage point on what the drain
 * did: §2.1 has the walk clear a group's flags on entering it, so by the time
 * any body runs its own flags are already gone. The child stands in for a
 * cel_update raised mid-walk, since reactive cells are not implemented yet.
 */
CEL_CompositionScope
SeqRootView(void)
{
    if (!g_seqComposeEnabled) {
        return CEL_CompositionDone();
    }

    CEL_Composition(CEL_Name("SeqRoot")) {
        CelsCompositionHost *const host = CelsInvalidationContextGet();
        g_seqRootRuns++;
        g_seqContextSeen = (CelsTransactionContextGet() != NULL);
        if (host != NULL && CelsSlotTableGroupCount(&host->slotTable) >= 3u) {
            g_seqChildFlags = CelsSlotTableGroupFlags(&host->slotTable, 1u);
            g_seqLeafFlags = CelsSlotTableGroupFlags(&host->slotTable, 2u);
        }

        CEL_Compose(CEL_Name("SeqChild")) {
            const CelsComposableId self = CEL_Composable();
            g_seqChildRuns++;

            if (host != NULL) {
                if (g_seqSelfInvalidations > 0) {
                    g_seqSelfInvalidations--;
                    TEST_ASSERT(CelsCompositionHostInvalidate(host, self) ==
                                CELS_OK);
                }
            }

            if (g_seqNotifyProbe) {
                g_seqNotifyProbe = false;
                const uint32_t before = g_seqCreateCount;
                CelsTransactionNotifyCreate(self, 0u,
                                            CelsHashString("SeqChild", 0));
                g_seqCreateInline = (g_seqCreateCount == before + 1u);
            }

            CEL_Compose(CEL_Name("SeqLeaf")) {
                g_seqLeafRuns++;
            }
        }
    }
    return CEL_CompositionDone();
}

/**
 * Opens a Flecs world and a session driven only by CelsSessionRecompose.
 *
 * @param maxDrainIterations Convergence bound, or 0 for the default.
 * @param outWorld           Receives the new world. Non-NULL.
 * @param outSession         Receives the initialized session. Non-NULL.
 */
static void
SeqSessionOpen(uint32_t maxDrainIterations,
               ecs_world_t **outWorld,
               CelsSession *outSession)
{
    ecs_world_t *const world = ecs_init();
    TEST_ASSERT(world != NULL);

    CelsSessionConfig config;
    memset(&config, 0, sizeof(config));
    config.compositionScope = SeqRootView;
    config.workerCount = 1;
    config.maxComposables = 32;
    config.maxDrainIterations = maxDrainIterations;
    config.transactionContext.onCreate = SeqOnCreate;
    config.transactionContext.onDestroy = SeqOnDestroy;

    TEST_ASSERT(CelsSessionInit(outSession, world, &config) == CELS_OK);
    *outWorld = world;
}

/**
 * Tears down a session opened with SeqSessionOpen and its world.
 */
static void
SeqSessionClose(ecs_world_t *world, CelsSession *session)
{
    CelsSessionDestroy(session);
    ecs_fini(world);
}

/**
 * Drives the drain loop: quiet path, targeted invalidation, mid-walk pickup,
 * inline callbacks and Lifecycle teardown, all on one session.
 */
static void
SeqRecomposeSuiteRun(void)
{
    printf("\n[Step 9] Sequential recompose: drain loop and lifecycle...\n");

    ecs_world_t *world = NULL;
    CelsSession session;
    SeqSessionOpen(0, &world, &session);

    // 9.1 First pass composes; a second pass with nothing owed runs nothing.
    SeqCountersReset();
    TEST_ASSERT(CelsSessionRecompose(&session) == CELS_OK);
    TEST_ASSERT(g_seqRootRuns == 1);
    TEST_ASSERT(g_seqChildRuns == 1);
    TEST_ASSERT(g_seqLeafRuns == 1);
    TEST_ASSERT(g_seqContextSeen);
    TEST_ASSERT(CelsTransactionContextGet() == NULL);

    SeqCountersReset();
    TEST_ASSERT(CelsSessionRecompose(&session) == CELS_OK);
    TEST_ASSERT(g_seqRootRuns == 0);
    TEST_ASSERT(g_seqChildRuns == 0);
    TEST_ASSERT(CelsSessionIsDirty(&session) == false);
    printf("  -> Quiet Recompose returned CELS_OK and ran nothing\n");

    const CEL_CompositionScope scope = CelsSessionGetCompositionScope(&session);
    TEST_ASSERT(scope.groupCount == 3);
    const ecs_entity_t seqRootEntity = (ecs_entity_t)scope.entity;
    TEST_ASSERT(seqRootEntity != 0);

    // 9.2 A queued invalidation reaches its target, and ONLY its target carries
    // INVALIDATED; the spine above it carries CONTAINS_INVALIDATED so no
    // ancestor can O(1)-skip past the subtree the queue reached.
    uint32_t childIndex = 0;
    TEST_ASSERT(CelsSlotTableFindGroup(&session.rootHost.slotTable,
                                       CelsHashString("SeqChild", 0),
                                       &childIndex) != NULL);
    TEST_ASSERT(childIndex == 1);

    uint32_t leafIndex = 0;
    TEST_ASSERT(CelsSlotTableFindGroup(&session.rootHost.slotTable,
                                       CelsHashString("SeqLeaf", 0),
                                       &leafIndex) != NULL);
    TEST_ASSERT(leafIndex == 2);

    SeqCountersReset();
    TEST_ASSERT(CelsCompositionHostInvalidate(&session.rootHost, leafIndex) ==
                CELS_OK);
    TEST_ASSERT(session.rootHost.invalidationCount == 1);
    TEST_ASSERT(session.rootHost.isDirty == true);
    // Queued, never applied on the spot: no flag is set until the drain.
    TEST_ASSERT(CelsSlotTableGroupFlags(&session.rootHost.slotTable,
                                        leafIndex) ==
                (uint16_t)CELS_GROUP_FLAG_NONE);

    TEST_ASSERT(CelsSessionRecompose(&session) == CELS_OK);
    TEST_ASSERT(g_seqLeafRuns == 1);
    TEST_ASSERT((g_seqLeafFlags &
                 (uint16_t)CELS_GROUP_FLAG_INVALIDATED) != 0u);
    TEST_ASSERT((g_seqChildFlags &
                 (uint16_t)CELS_GROUP_FLAG_CONTAINS_INVALIDATED) != 0u);
    TEST_ASSERT((g_seqChildFlags &
                 (uint16_t)CELS_GROUP_FLAG_INVALIDATED) == 0u);
    TEST_ASSERT(session.rootHost.invalidationCount == 0);
    TEST_ASSERT(CelsSlotTableGroupFlags(&session.rootHost.slotTable,
                                        leafIndex) ==
                (uint16_t)CELS_GROUP_FLAG_NONE);
    printf("  -> Drain marked SeqLeaf INVALIDATED and its spine "
           "CONTAINS_INVALIDATED\n");

    // 9.3 An invalidation raised from inside a body costs no frame: the next
    // drain iteration of THIS call picks it up.
    SeqCountersReset();
    g_seqSelfInvalidations = 1;
    CelsSessionMarkDirty(&session);
    TEST_ASSERT(CelsSessionRecompose(&session) == CELS_OK);
    TEST_ASSERT(g_seqSelfInvalidations == 0);
    TEST_ASSERT(g_seqChildRuns == 2);
    TEST_ASSERT(session.rootHost.invalidationCount == 0);
    TEST_ASSERT(CelsSessionIsDirty(&session) == false);
    printf("  -> Mid-walk invalidation converged inside the same call\n");

    // 9.4 The session's context is published for the walk, and firing through
    // it lands on the developer's callback inline, before the next statement.
    SeqCountersReset();
    g_seqNotifyProbe = true;
    g_seqCreateInline = false;
    CelsSessionMarkDirty(&session);
    TEST_ASSERT(CelsSessionRecompose(&session) == CELS_OK);
    TEST_ASSERT(g_seqCreateInline);
    TEST_ASSERT(g_seqCreateCount == 1);
    TEST_ASSERT(g_seqCreateComposable == childIndex);
    TEST_ASSERT(g_seqCreateParent == 0);
    TEST_ASSERT(g_seqCreateKey == CelsHashString("SeqChild", 0));
    TEST_ASSERT(CelsTransactionContextGet() == NULL);
    printf("  -> onCreate fired inline with the composing composable's id\n");

    // 9.5 Lifecycle: destroyed first, once, and the body never runs that pass.
    SeqCountersReset();
    TEST_ASSERT(CelsLifecycleMarkForDestroy(
                    &session, CelsHashString("SeqRoot", 0)) == CELS_OK);
    TEST_ASSERT(session.pendingDestroyCount == 1);
    g_seqComposeEnabled = false;

    TEST_ASSERT(CelsSessionRecompose(&session) == CELS_OK);
    TEST_ASSERT(g_seqDestroyCount == 1);
    TEST_ASSERT(g_seqDestroyComposable == 0);
    TEST_ASSERT(g_seqRootRuns == 0);
    TEST_ASSERT(g_seqChildRuns == 0);
    TEST_ASSERT(g_seqLeafRuns == 0);
    TEST_ASSERT(session.pendingDestroyCount == 0);
    TEST_ASSERT(CelsSlotTableGroupCount(&session.rootHost.slotTable) == 0);
    TEST_ASSERT(!ecs_is_alive(world, seqRootEntity));
    printf("  -> Composition destroyed before composing: one onDestroy for "
           "the root, subtree cleaned up silently\n");

    // 9.6 The table survives the teardown and remounts cleanly.
    SeqCountersReset();
    g_seqComposeEnabled = true;
    CelsSessionMarkDirty(&session);
    TEST_ASSERT(CelsSessionRecompose(&session) == CELS_OK);
    TEST_ASSERT(g_seqRootRuns == 1);
    TEST_ASSERT(CelsSlotTableGroupCount(&session.rootHost.slotTable) == 3);

    SeqSessionClose(world, &session);
}

/**
 * Drives a deliberate invalidation cycle into the convergence bound.
 */
static void
SeqCycleSuiteRun(void)
{
    printf("\n[Step 10] Sequential recompose: runaway invalidation cycle...\n");

    ecs_world_t *world = NULL;
    CelsSession session;
    SeqSessionOpen(3, &world, &session);

    SeqCountersReset();
    g_seqSelfInvalidations = 100;

    const CelsResult cycleRes = CelsSessionRecompose(&session);
    TEST_ASSERT(cycleRes == CELS_ERROR_RECOMPOSE_DID_NOT_CONVERGE);
    TEST_ASSERT(g_seqRootRuns == 3);
    // Abandoned, not corrupted: the queue and the dirty flag both survive.
    TEST_ASSERT(session.rootHost.invalidationCount == 1);
    TEST_ASSERT(session.rootHost.isDirty == true);
    TEST_ASSERT(CelsTransactionContextGet() == NULL);
    printf("  -> Bound hit after 3 iterations; queue left intact\n");

    SeqCountersReset();
    g_seqSelfInvalidations = 0;
    TEST_ASSERT(CelsSessionRecompose(&session) == CELS_OK);
    TEST_ASSERT(g_seqRootRuns == 1);
    TEST_ASSERT(session.rootHost.invalidationCount == 0);
    printf("  -> Next call resumed from the surviving queue and settled\n");

    SeqSessionClose(world, &session);
}

/**
 * Checks that a composable budget derives the slab instead of hand-sizing it.
 */
static void
SeqSlabSizingSuiteRun(void)
{
    printf("\n[Step 11] maxComposables derives slab size and group count...\n");

    ecs_world_t *world = ecs_init();
    TEST_ASSERT(world != NULL);

    CelsSessionConfig config;
    memset(&config, 0, sizeof(config));
    config.compositionScope = SeqRootView;
    config.workerCount = 1;
    config.maxComposables = 128;
    // Deliberately wrong by hand: a composable budget must override both.
    config.slabSize = 65536;
    config.maxGroups = 2;

    CelsSession session;
    TEST_ASSERT(CelsSessionInit(&session, world, &config) == CELS_OK);
    TEST_ASSERT(CelsSessionGetSlabSize(&session) == 16384);
    TEST_ASSERT(CelsSlotTableGroupCapacity(&session.rootHost.slotTable) == 128);
    // 128 bytes each: 32 structural, 96 of slots = 12 words per composable.
    TEST_ASSERT(CelsSlotTableSlotCapacity(&session.rootHost.slotTable) ==
                (16384u - 128u * 32u) / 8u);
    TEST_ASSERT(session.maxDrainIterations ==
                CELS_DEFAULT_MAX_DRAIN_ITERATIONS);
    printf("  -> maxComposables=128 derived a 16KB slab, 128 groups\n");

    CelsSessionDestroy(&session);
    ecs_fini(world);

    // A budget smaller than one page still gets one whole page. A fresh world
    // per session: the dispatcher names its pipeline phases, and Flecs rejects
    // a second "OnRecompose" in a world that already has one.
    world = ecs_init();
    TEST_ASSERT(world != NULL);

    memset(&config, 0, sizeof(config));
    config.compositionScope = SeqRootView;
    config.workerCount = 1;
    config.maxComposables = 10;

    TEST_ASSERT(CelsSessionInit(&session, world, &config) == CELS_OK);
    TEST_ASSERT(CelsSessionGetSlabSize(&session) == 4096);
    TEST_ASSERT(CelsSlotTableGroupCapacity(&session.rootHost.slotTable) == 10);
    printf("  -> maxComposables=10 rounded up to a single 4KB page\n");

    CelsSessionDestroy(&session);
    ecs_fini(world);
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

    // 9-11. The synchronous recompose pass, on its own worlds. Each suite owns
    // one world at a time: ecs_id(CelsCompositionHost) is a single global, so
    // two live worlds would leave one session holding the other's component id.
    SeqRecomposeSuiteRun();
    SeqCycleSuiteRun();
    SeqSlabSizingSuiteRun();

    printf("\n==============================================================\n");
    printf("  ALL CELS SESSION TESTS PASSED SUCCESSFULLY!                 \n");
    printf("==============================================================\n");

    return 0;
}
