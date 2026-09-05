#include "cels.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#define ALIGNED_SLAB(size, name) __declspec(align(64)) uint8_t name[size]
#else
#define ALIGNED_SLAB(size, name) __attribute__((aligned(64))) uint8_t name[size]
#endif

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "Assertion failed: %s at %s:%d\n",                 \
                    #cond, __FILE__, __LINE__);                                \
            assert(cond);                                                      \
        }                                                                      \
    } while (0)

#define KEY_APP_ROOT 0x100
#define KEY_CHILD_A  0x101
#define KEY_CHILD_B  0x102

typedef struct PlayerState {
    int health;
    bool has_shield;
} PlayerState;

static int g_meshExecCount = 0;
static int g_shieldExecCount = 0;
static int g_rootSkipCount = 0;

static void
ComposablePlayer(CelsComposer *cmp, CelsSlotTable *table, const PlayerState *state)
{
    CEL_CompositionScope(cmp, table, KEY_APP_ROOT) {
        cel_watch(*state) {
            printf("  [Exec] Root Node (State Changed: HP=%d, Shield=%s)\n",
                   state->health, state->has_shield ? "ON" : "OFF");

            // Subtree 1: Mesh
            CEL_Compose(KEY_CHILD_A) {
                printf("    [Exec] Mesh Subtree rendering...\n");
                g_meshExecCount++;
            }

            // Subtree 2: Shield VFX (Conditional)
            if (state->has_shield) {
                CEL_Compose(KEY_CHILD_B) {
                    printf("    [Exec] Shield VFX Subtree rendering...\n");
                    g_shieldExecCount++;
                }
            }
        }
    }
}

static inline PlayerState
CreatePlayerState(int health, bool hasShield)
{
    PlayerState s;
    memset(&s, 0, sizeof(s));
    s.health = health;
    s.has_shield = hasShield;
    return s;
}

static void
TestComposerFullLifecycle(void)
{
    printf("Running TestComposerFullLifecycle...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 32);
    TEST_ASSERT(initRes == CELS_OK);

    CelsComposer cmp;

    // Pass 1: Initial Mount (Health=100, Shield=ON)
    printf("\n=== PASS 1: Initial Mount ===\n");
    g_meshExecCount = 0;
    g_shieldExecCount = 0;
    g_rootSkipCount = 0;

    const PlayerState s1 = CreatePlayerState(100, true);
    ComposablePlayer(&cmp, &table, &s1);

    TEST_ASSERT(g_meshExecCount == 1);
    TEST_ASSERT(g_shieldExecCount == 1);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 0);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 3);

    // Pass 2: Identical State (Health=100, Shield=ON)
    // Entire subtree must skip automatically in O(1) time
    printf("\n=== PASS 2: Identical State ===\n");
    g_meshExecCount = 0;
    g_shieldExecCount = 0;

    const PlayerState s2 = CreatePlayerState(100, true);
    ComposablePlayer(&cmp, &table, &s2);

    TEST_ASSERT(g_meshExecCount == 0);
    TEST_ASSERT(g_shieldExecCount == 0);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 1);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 3);
    printf("  [Skip] Root Node Unchanged. Automatically skipped in O(1).\n");

    // Pass 3: Mutated Health (Health=80, Shield=ON)
    // Re-executes Root, Mesh, and Shield
    printf("\n=== PASS 3: Health Mutated ===\n");
    g_meshExecCount = 0;
    g_shieldExecCount = 0;

    const PlayerState s3 = CreatePlayerState(80, true);
    ComposablePlayer(&cmp, &table, &s3);

    TEST_ASSERT(g_meshExecCount == 1);
    TEST_ASSERT(g_shieldExecCount == 1);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 0);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 3);

    // Pass 4: Conditional Branch Vanishes (Health=80, Shield=OFF)
    // Shield is skipped, and Child B must be automatically pruned from the table!
    printf("\n=== PASS 4: Shield Disabled (Vanishing Branch) ===\n");
    g_meshExecCount = 0;
    g_shieldExecCount = 0;

    const PlayerState s4 = CreatePlayerState(80, false);
    ComposablePlayer(&cmp, &table, &s4);

    TEST_ASSERT(g_meshExecCount == 1);
    TEST_ASSERT(g_shieldExecCount == 0);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 0);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);

    // Pass 5: Identical State with Shield OFF (Health=80, Shield=OFF)
    // Should skip entire 2-group subtree in O(1)
    printf("\n=== PASS 5: Identical State with Shield OFF ===\n");
    g_meshExecCount = 0;
    g_shieldExecCount = 0;

    const PlayerState s5 = CreatePlayerState(80, false);
    ComposablePlayer(&cmp, &table, &s5);

    TEST_ASSERT(g_meshExecCount == 0);
    TEST_ASSERT(g_shieldExecCount == 0);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 1);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);
    printf("  [Skip] Root Node Unchanged. Automatically skipped in O(1).\n");

    // Pass 6: Branch Re-appearance (Health=80, Shield=ON)
    // Child B is re-inserted into the gap buffer!
    printf("\n=== PASS 6: Shield Re-enabled (Branch Re-appearance) ===\n");
    g_meshExecCount = 0;
    g_shieldExecCount = 0;

    const PlayerState s6 = CreatePlayerState(80, true);
    ComposablePlayer(&cmp, &table, &s6);

    TEST_ASSERT(g_meshExecCount == 1);
    TEST_ASSERT(g_shieldExecCount == 1);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 0);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 3);

    printf("  PASSED: TestComposerFullLifecycle\n");
}

static void
TestMultipleParametersDiffing(void)
{
    printf("Running TestMultipleParametersDiffing...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    CelsComposer cmp;

    int level = 5;
    uint32_t color = 0xFF00FF;

    // Pass 1: Write both parameters into group 0x500
    CelsComposerBegin(&cmp, &table);
    TEST_ASSERT(CelsComposerGroupStart(&cmp, 0x500));
    const bool p1Change1 = CELS_CHANGED(&cmp, level);
    const bool p1Change2 = CELS_CHANGED(&cmp, color);
    TEST_ASSERT(p1Change1 == true);
    TEST_ASSERT(p1Change2 == true);
    CelsComposerGroupEnd(&cmp);

    // Pass 2: Identical inputs
    CelsComposerBegin(&cmp, &table);
    TEST_ASSERT(CelsComposerGroupStart(&cmp, 0x500));
    const bool p2Change1 = CELS_CHANGED(&cmp, level);
    const bool p2Change2 = CELS_CHANGED(&cmp, color);
    TEST_ASSERT(p2Change1 == false);
    TEST_ASSERT(p2Change2 == false);
    CelsComposerGroupEnd(&cmp);

    // Pass 3: Only color changes
    color = 0x00FFFF;
    CelsComposerBegin(&cmp, &table);
    TEST_ASSERT(CelsComposerGroupStart(&cmp, 0x500));
    const bool p3Change1 = CELS_CHANGED(&cmp, level);
    const bool p3Change2 = CELS_CHANGED(&cmp, color);
    TEST_ASSERT(p3Change1 == false);
    TEST_ASSERT(p3Change2 == true);
    CelsComposerGroupEnd(&cmp);

    printf("  PASSED: TestMultipleParametersDiffing\n");
}

static int g_inventoryItemExecs = 0;

static void
ComposableInventoryItem(uint32_t slotKey, int itemCount)
{
    CEL_Compose(slotKey) {
        cel_watch(itemCount) {
            g_inventoryItemExecs++;
        }
    }
}

static void
TestDslMacroAmbientContext(void)
{
    printf("Running TestDslMacroAmbientContext...\n");

    TEST_ASSERT(CelsComposerGetCurrent() == NULL);

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    int potionCount = 5;

    // Pass 1: Initial mount using ambient default composer
    g_inventoryItemExecs = 0;
    CEL_CompositionScope(&table, 0x1000) {
        TEST_ASSERT(CelsComposerGetCurrent() != NULL);
        ComposableInventoryItem(0x1001, potionCount);
    }
    TEST_ASSERT(CelsComposerGetCurrent() == NULL);
    TEST_ASSERT(g_inventoryItemExecs == 1);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);

    // Pass 2: Identical state -> ComposableInventoryItem should skip in O(1)
    // (Also testing CEL_Scope shorthand alias)
    g_inventoryItemExecs = 0;
    CEL_Scope(&table, 0x1000) {
        ComposableInventoryItem(0x1001, potionCount);
    }
    TEST_ASSERT(CelsComposerGetCurrent() == NULL);
    TEST_ASSERT(g_inventoryItemExecs == 0);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);

    // Pass 3: Mutated state -> Potion count changes to 10 -> executes
    potionCount = 10;
    g_inventoryItemExecs = 0;
    CEL_CompositionScope(&table, 0x1000) {
        ComposableInventoryItem(0x1001, potionCount);
    }
    TEST_ASSERT(CelsComposerGetCurrent() == NULL);
    TEST_ASSERT(g_inventoryItemExecs == 1);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);

    printf("  PASSED: TestDslMacroAmbientContext\n");
}

static int g_styleBChildExecs = 0;
static int g_lastObservedClicks = -1;

typedef struct ItemCardState {
    int itemId;
    int quantity;
} ItemCardState;

static void
ComposableItemCard(const ItemCardState *state)
{
    // Style B: CEL_Compose enters group AND diffs *state in one construct!
    CEL_Compose(0x2001, *state) {
        g_styleBChildExecs++;

        // Test CEL_remember: local ephemeral UI state in the slot table
        bool initHover = false;
        bool *isHovered = CEL_remember(initHover);
        TEST_ASSERT(isHovered != NULL);

        int initClicks = 0;
        int *clickCount = CEL_remember_type(int, initClicks);
        TEST_ASSERT(clickCount != NULL);

        // Record remembered count observed on entry before mutating
        g_lastObservedClicks = *clickCount;

        // Mutate remembered state across frames
        (*clickCount)++;
    }
}

static void
TestStyleBComposeAndRemember(void)
{
    printf("Running TestStyleBComposeAndRemember...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    ItemCardState item = { .itemId = 42, .quantity = 1 };

    // Pass 1: Initial mount -> Style B executes block (clickCount seeded to 0)
    g_styleBChildExecs = 0;
    g_lastObservedClicks = -1;
    CEL_Scope(&table, 0x2000) {
        ComposableItemCard(&item);
    }
    TEST_ASSERT(g_styleBChildExecs == 1);
    TEST_ASSERT(g_lastObservedClicks == 0); // Seeded with 0
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);

    // Pass 2: Identical state -> Style B skips in O(1)!
    g_styleBChildExecs = 0;
    CEL_Scope(&table, 0x2000) {
        ComposableItemCard(&item);
    }
    TEST_ASSERT(g_styleBChildExecs == 0); // Completely skipped in O(1)!
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);

    // Pass 3: State changes (quantity = 2) -> Style B re-executes block
    // clickCount was mutated to 1 in Pass 1, and should be preserved here!
    item.quantity = 2;
    g_styleBChildExecs = 0;
    CEL_Scope(&table, 0x2000) {
        ComposableItemCard(&item);
    }
    TEST_ASSERT(g_styleBChildExecs == 1);
    TEST_ASSERT(g_lastObservedClicks == 1); // Preserved from Pass 1!

    // Pass 4: State changes (quantity = 3) -> clickCount should now be 2!
    item.quantity = 3;
    g_styleBChildExecs = 0;
    CEL_Scope(&table, 0x2000) {
        ComposableItemCard(&item);
    }
    TEST_ASSERT(g_styleBChildExecs == 1);
    TEST_ASSERT(g_lastObservedClicks == 2); // Preserved from Pass 3!

    printf("  PASSED: TestStyleBComposeAndRemember\n");
}

/* ========================================================================= */
/* Test Suite: Header Variants and Public Macros                             */
/* ========================================================================= */

typedef struct HudCardState {
    int score;
    int lives;
} HudCardState;

static int g_hudCardExecs = 0;
static int g_hudStatusExecs = 0;
static int g_lastObservedTaps = -1;

/* Declarations using header macro variants */
CEL_Compose_Decl(ComposableHudCard, const HudCardState *state);
CEL_COMPOSE_DECL(ComposableHudStatus);
CEL_CompositionScope_Decl(GameHudScope, const HudCardState *state);

void
ComposableHudStatus(void)
{
    CEL_Compose(0x3002) {
        g_hudStatusExecs++;
    }
}

void
ComposableHudCard(const HudCardState *state)
{
    CEL_Compose(0x3001, *state) {
        g_hudCardExecs++;

        // Test cel_remember from cels.h
        int initTaps = 0;
        int *taps = cel_remember(initTaps);
        TEST_ASSERT(taps != NULL);
        g_lastObservedTaps = *taps;
        (*taps)++;

        // Test cel_watch from cels.h
        cel_watch(state->score) {
            ComposableHudStatus();
        }
    }
}

void
GameHudScope(const HudCardState *state)
{
    // 1-argument CEL_CompositionScope: assigns and manages its own slot table!
    CEL_CompositionScope(0x3000) {
        ComposableHudCard(state);
    }
}

static void
TestHeaderVariantsAndCompositionScope(void)
{
    printf("Running TestHeaderVariantsAndCompositionScope...\n");

    HudCardState state = { .score = 100, .lives = 3 };

    // Pass 1: Initial mount -> GameHudScope executes, automatically assigns own table
    g_hudCardExecs = 0;
    g_hudStatusExecs = 0;
    g_lastObservedTaps = -1;
    GameHudScope(&state);

    TEST_ASSERT(g_hudCardExecs == 1);
    TEST_ASSERT(g_hudStatusExecs == 1);
    TEST_ASSERT(g_lastObservedTaps == 0);

    // Pass 2: Identical state -> ComposableHudCard skips in O(1) in auto-assigned table!
    g_hudCardExecs = 0;
    g_hudStatusExecs = 0;
    GameHudScope(&state);

    TEST_ASSERT(g_hudCardExecs == 0); // Skipped in O(1)!
    TEST_ASSERT(g_hudStatusExecs == 0); // Child also skipped!

    // Pass 3: State changes -> score = 250 -> recomposes in same table
    state.score = 250;
    g_hudCardExecs = 0;
    g_hudStatusExecs = 0;
    GameHudScope(&state);

    TEST_ASSERT(g_hudCardExecs == 1); // Re-executed due to mutation
    TEST_ASSERT(g_hudStatusExecs == 1);
    TEST_ASSERT(g_lastObservedTaps == 1); // Preserved from Pass 1!

    // Pass 4: Out of scope -> Free the table back up using CEL_Dispose!
    CEL_Dispose(0x3000);

    // Pass 5: Calling again after dispose -> Fresh mount into newly assigned table!
    g_hudCardExecs = 0;
    g_hudStatusExecs = 0;
    g_lastObservedTaps = -1;
    GameHudScope(&state);

    TEST_ASSERT(g_hudCardExecs == 1); // Fresh mount!
    TEST_ASSERT(g_lastObservedTaps == 0); // Reset to 0 because previous table was freed!

    // Clean up
    CEL_Dispose(0x3000);

    printf("  PASSED: TestHeaderVariantsAndCompositionScope\n");
}

/* ========================================================================= */
/* Test Suite: CEL_query Reactive ECS Query Observer                         */
/* ========================================================================= */

typedef struct DummyEcsQuery {
    uint64_t changeTick;
    int entityCount;
} DummyEcsQuery;

static int g_queryBodyExecs = 0;
static int g_queryEntityChildExecs = 0;

static void
TestEcsQueryObserver(void)
{
    printf("Running TestEcsQueryObserver...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    DummyEcsQuery query = { .changeTick = 1, .entityCount = 2 };

    // Pass 1: Initial mount -> CEL_query executes
    g_queryBodyExecs = 0;
    g_queryEntityChildExecs = 0;
    CEL_CompositionScope(&table, 0x4000) {
        CEL_query(query) {
            g_queryBodyExecs++;
            for (int i = 0; i < query.entityCount; i++) {
                CEL_Compose(0x4100 + (uint32_t)i) {
                    g_queryEntityChildExecs++;
                }
            }
        }
    }
    TEST_ASSERT(g_queryBodyExecs == 1);
    TEST_ASSERT(g_queryEntityChildExecs == 2);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 3);

    // Pass 2: Identical query state -> CEL_query skips in O(1)!
    g_queryBodyExecs = 0;
    g_queryEntityChildExecs = 0;
    CEL_CompositionScope(&table, 0x4000) {
        CEL_query(query) {
            g_queryBodyExecs++;
            for (int i = 0; i < query.entityCount; i++) {
                CEL_Compose(0x4100 + (uint32_t)i) {
                    g_queryEntityChildExecs++;
                }
            }
        }
    }
    TEST_ASSERT(g_queryBodyExecs == 0); // Entire query skipped in O(1)!
    TEST_ASSERT(g_queryEntityChildExecs == 0);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 3);

    // Pass 3: Query changes (changeTick = 2, 3 entities) -> recomposes
    query.changeTick = 2;
    query.entityCount = 3;
    g_queryBodyExecs = 0;
    g_queryEntityChildExecs = 0;
    CEL_CompositionScope(&table, 0x4000) {
        CEL_query(query) {
            g_queryBodyExecs++;
            for (int i = 0; i < query.entityCount; i++) {
                CEL_Compose(0x4100 + (uint32_t)i) {
                    g_queryEntityChildExecs++;
                }
            }
        }
    }
    TEST_ASSERT(g_queryBodyExecs == 1); // Recomposed!
    TEST_ASSERT(g_queryEntityChildExecs == 3);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 4);

    printf("  PASSED: TestEcsQueryObserver\n");
}

/* ========================================================================= */
/* Test Suite: CEL_observable Flecs Query Diff (Before vs After)             */
/* ========================================================================= */

typedef struct FlecsEnemyQuery {
    uint64_t matchTick;
    uint32_t entityCount;
    int totalPartyHp;
} FlecsEnemyQuery;

static int g_observableExecCount = 0;
static FlecsEnemyQuery g_capturedPrevQuery;
static FlecsEnemyQuery g_capturedCurrQuery;

static void
TestFlecsQueryObservableDiff(void)
{
    printf("Running TestFlecsQueryObservableDiff...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    FlecsEnemyQuery query = {
        .matchTick = 100,
        .entityCount = 2,
        .totalPartyHp = 200
    };

    // Pass 1: Initial mount -> CEL_observable executes
    g_observableExecCount = 0;
    memset(&g_capturedPrevQuery, 0, sizeof(g_capturedPrevQuery));
    memset(&g_capturedCurrQuery, 0, sizeof(g_capturedCurrQuery));

    CEL_CompositionScope(&table, 0x5000) {
        FlecsEnemyQuery prevQuery;
        CEL_observable(query, prevQuery) {
            g_observableExecCount++;
            g_capturedPrevQuery = prevQuery;
            g_capturedCurrQuery = query;
        }
    }
    TEST_ASSERT(g_observableExecCount == 1);
    // On mount, previous is zeroed
    TEST_ASSERT(g_capturedPrevQuery.matchTick == 0);
    TEST_ASSERT(g_capturedCurrQuery.matchTick == 100);
    TEST_ASSERT(g_capturedCurrQuery.totalPartyHp == 200);

    // Pass 2: Identical state (no ECS systems touched matching entities)
    g_observableExecCount = 0;
    CEL_CompositionScope(&table, 0x5000) {
        FlecsEnemyQuery prevQuery;
        CEL_observable(query, prevQuery) {
            g_observableExecCount++;
            g_capturedPrevQuery = prevQuery;
            g_capturedCurrQuery = query;
        }
    }
    // Entire observable block skipped in O(1)!
    TEST_ASSERT(g_observableExecCount == 0);

    // Pass 3: Combat System dealt 60 damage -> HP changes from 200 to 140!
    // matchTick increments from 100 to 101
    query.matchTick = 101;
    query.totalPartyHp = 140;

    g_observableExecCount = 0;
    CEL_CompositionScope(&table, 0x5000) {
        FlecsEnemyQuery prevQuery;
        // Test CEL_Observeable alias as well
        CEL_Observeable(query, prevQuery) {
            g_observableExecCount++;
            g_capturedPrevQuery = prevQuery;
            g_capturedCurrQuery = query;
        }
    }
    TEST_ASSERT(g_observableExecCount == 1); // Recomposed!

    // VERIFY: Data is proven different to before recomposition!
    TEST_ASSERT(g_capturedPrevQuery.matchTick == 100);        // BEFORE recomposition
    TEST_ASSERT(g_capturedPrevQuery.totalPartyHp == 200);     // BEFORE recomposition
    TEST_ASSERT(g_capturedCurrQuery.matchTick == 101);        // NOW during recomposition
    TEST_ASSERT(g_capturedCurrQuery.totalPartyHp == 140);     // NOW during recomposition
    TEST_ASSERT(g_capturedPrevQuery.totalPartyHp != g_capturedCurrQuery.totalPartyHp);

    // Pass 4: Enemy dies -> entityCount drops from 2 to 1, matchTick = 102
    query.matchTick = 102;
    query.entityCount = 1;
    query.totalPartyHp = 70;

    g_observableExecCount = 0;
    CEL_CompositionScope(&table, 0x5000) {
        FlecsEnemyQuery prevQuery;
        cel_observable(query, prevQuery) {
            g_observableExecCount++;
            g_capturedPrevQuery = prevQuery;
            g_capturedCurrQuery = query;
        }
    }
    TEST_ASSERT(g_observableExecCount == 1);
    TEST_ASSERT(g_capturedPrevQuery.entityCount == 2); // BEFORE recomposition
    TEST_ASSERT(g_capturedCurrQuery.entityCount == 1); // NOW during recomposition
    TEST_ASSERT(g_capturedPrevQuery.entityCount != g_capturedCurrQuery.entityCount);

    printf("  PASSED: TestFlecsQueryObservableDiff\n");
}

/* ========================================================================= */
/* Test Suite 8: Nic Barker Clay UI Inspired Names & Composition Lookup      */
/* ========================================================================= */

static void
TestClayStyleNamesAndCompositionLookup(void)
{
    printf("Running TestClayStyleNamesAndCompositionLookup...\n");

    // 1. FNV-1a Hash Distinctness & Index Offsets
    const uint32_t nameHud = CEL_Name("PlayerHud");
    const uint32_t nameHealth = CEL_Name("HealthBar");
    const uint32_t nameMana = CEL_Name("ManaBar");
    TEST_ASSERT(nameHud != 0);
    TEST_ASSERT(nameHealth != 0);
    TEST_ASSERT(nameMana != 0);
    TEST_ASSERT(nameHealth != nameMana);
    TEST_ASSERT(nameHealth != nameHud);

    const uint32_t slot0 = CEL_Name("Slot", 0);
    const uint32_t slot1 = CEL_Name("Slot", 1);
    const uint32_t slot1Alias = CEL_NameI("Slot", 1);
    TEST_ASSERT(slot0 != slot1);
    TEST_ASSERT(slot1 == slot1Alias);

    // 2. Parent-Scoped Local Names (Clay CLAY_ID_LOCAL equivalent)
    const uint32_t parentCardA = CEL_Name("CardA");
    const uint32_t parentCardB = CEL_Name("CardB");
    const uint32_t btnA = CEL_NameScoped("CardA", "CloseBtn");
    const uint32_t btnB = CEL_NameScoped("CardB", "CloseBtn");
    TEST_ASSERT(btnA != btnB);
    TEST_ASSERT(parentCardA != parentCardB);

    // 3. Composition with CEL_Name (replacing raw numbers like 0x0101)
    int health = 100;
    int mana = 50;
    int healthRenderCount = 0;
    int manaRenderCount = 0;

    // Pass 1: Initial mount using CEL_Name instead of raw numbers
    CEL_CompositionScope(CEL_Name("PlayerHud")) {
        // Child 1: Stateful Compose with CEL_Name
        CEL_Compose(CEL_Name("HealthBar"), health) {
            healthRenderCount++;
            int *cachedHp = cel_remember(health);
            *cachedHp = health;
        }

        // Child 2: Stateless Compose with cel_watch
        CEL_Compose(CEL_Name("ManaBar")) {
            cel_watch(mana) {
                manaRenderCount++;
            }
        }

        // Child 3: Dynamic indexed items (Clay CLAY_IDI equivalent)
        for (int i = 0; i < 4; i++) {
            CEL_Compose(CEL_NameI("InventorySlot", i)) {
                // Item rendering logic
            }
        }
    }

    TEST_ASSERT(healthRenderCount == 1);
    TEST_ASSERT(manaRenderCount == 1);

    // 4. Finding Compositions by CEL_Name outside the composition pass
    CelsCompositionRef healthRef = CEL_Find(CEL_Name("HealthBar"));
    TEST_ASSERT(healthRef.found == true);
    TEST_ASSERT(healthRef.key == CEL_Name("HealthBar"));
    TEST_ASSERT(healthRef.group != NULL);
    TEST_ASSERT(healthRef.slots != NULL);
    TEST_ASSERT(*(int *)healthRef.slots == 100); // Verify remembered HP in slot memory!

    CelsCompositionRef manaRef = CEL_FindByName("ManaBar");
    TEST_ASSERT(manaRef.found == true);
    TEST_ASSERT(manaRef.key == CEL_Name("ManaBar"));

    CelsCompositionRef slotRef2 = CEL_Find(CEL_NameI("InventorySlot", 2));
    TEST_ASSERT(slotRef2.found == true);
    TEST_ASSERT(slotRef2.key == CEL_NameI("InventorySlot", 2));

    CelsCompositionRef nonExistentRef = CEL_FindByName("NonExistentNode");
    TEST_ASSERT(nonExistentRef.found == false);

    // 5. State Retention across Recomposition with O(1) skipping
    // Health changes (100 -> 80), Mana is unchanged (50).
    health = 80;
    healthRenderCount = 0;
    manaRenderCount = 0;

    CEL_CompositionScope(CEL_Name("PlayerHud")) {
        CEL_Compose(CEL_Name("HealthBar"), health) {
            healthRenderCount++;
            int *cachedHp = cel_remember(health);
            *cachedHp = health;
        }

        CEL_Compose(CEL_Name("ManaBar")) {
            cel_watch(mana) {
                manaRenderCount++;
            }
        }

        for (int i = 0; i < 4; i++) {
            CEL_Compose(CEL_NameI("InventorySlot", i)) {
                // Preserved in slot table
            }
        }
    }

    TEST_ASSERT(healthRenderCount == 1); // Health recomposed
    TEST_ASSERT(manaRenderCount == 0);   // Mana skipped in O(1)!

    // Verify updated remember state in HealthBar
    healthRef = CEL_Find(CEL_Name("HealthBar"));
    TEST_ASSERT(healthRef.found == true);
    TEST_ASSERT(*(int *)healthRef.slots == 80);

    // 6. Cleanup with CEL_Dispose using CEL_Name
    CEL_Dispose(CEL_Name("PlayerHud"));
    TEST_ASSERT(CEL_Find(CEL_Name("HealthBar")).found == false);
    TEST_ASSERT(CEL_Find(CEL_Name("ManaBar")).found == false);

    printf("  PASSED: TestClayStyleNamesAndCompositionLookup\n");
}

/* ========================================================================= */
/* Test Suite: GroupStart Must Respect the Current Parent's Subtree Bound    */
/* ========================================================================= */

/*
 * Regression test for a bug where CelsComposerGroupStart's cache-hit check
 * bounded readerIndex against the table's TOTAL group count instead of the
 * current parent's own groupEndStack bound. That let a new child inserted
 * under one subtree "adopt" an unrelated group belonging to a later sibling
 * subtree whenever the two happened to share the same key.
 */

#define KEY_DUPCHECK_ROOT 0x600
#define KEY_DUPCHECK_A    0x601
#define KEY_DUPCHECK_B    0x602 /* Reused as both A's new child and Root's sibling */

static void
TestGroupStartRespectsParentSubtreeBound(void)
{
    printf("Running TestGroupStartRespectsParentSubtreeBound...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    CelsComposer cmp;

    // Pass 1: Root -> A (no children), Root -> B (A's sibling)
    CelsComposerBegin(&cmp, &table);
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_DUPCHECK_ROOT));
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_DUPCHECK_A));
    CelsComposerGroupEnd(&cmp); // Close A: no children
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_DUPCHECK_B));
    CelsComposerGroupEnd(&cmp); // Close B
    CelsComposerGroupEnd(&cmp); // Close Root

    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 3);

    // Pass 2: A gains a NEW child that happens to reuse B's key. A correct
    // implementation must treat this as a fresh insertion scoped to A, and
    // must NOT "adopt" the unrelated sibling group B that physically follows
    // A in the table at this point in the traversal.
    CelsComposerBegin(&cmp, &table);
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_DUPCHECK_ROOT));
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_DUPCHECK_A));
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_DUPCHECK_B)); // A's new child
    CelsComposerGroupEnd(&cmp); // Close A's child
    CelsComposerGroupEnd(&cmp); // Close A
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_DUPCHECK_B)); // Root's original B
    CelsComposerGroupEnd(&cmp); // Close B
    CelsComposerGroupEnd(&cmp); // Close Root

    // Both groups must exist independently: no misattribution, no duplicate
    // collapsing into a single physical slot.
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 4);

    CelsSlotReader reader;
    TEST_ASSERT(CelsSlotTableReaderOpen(&table, &reader) == CELS_OK);

    CelsSlotGroup rootGroup, aGroup, aChildGroup, rootBGroup;
    TEST_ASSERT(CelsSlotReaderGroupGet(&reader, 0, &rootGroup) == CELS_OK);
    TEST_ASSERT(CelsSlotReaderGroupGet(&reader, 1, &aGroup) == CELS_OK);
    TEST_ASSERT(CelsSlotReaderGroupGet(&reader, 2, &aChildGroup) == CELS_OK);
    TEST_ASSERT(CelsSlotReaderGroupGet(&reader, 3, &rootBGroup) == CELS_OK);

    TEST_ASSERT(rootGroup.key == KEY_DUPCHECK_ROOT);
    TEST_ASSERT(rootGroup.groupSize == 3); // A + A's child + Root's B

    TEST_ASSERT(aGroup.key == KEY_DUPCHECK_A);
    TEST_ASSERT(aGroup.parentIndex == 0);
    TEST_ASSERT(aGroup.groupSize == 1); // Owns exactly its new child

    TEST_ASSERT(aChildGroup.key == KEY_DUPCHECK_B);
    TEST_ASSERT(aChildGroup.parentIndex == 1); // Parented to A, NOT Root!

    TEST_ASSERT(rootBGroup.key == KEY_DUPCHECK_B);
    TEST_ASSERT(rootBGroup.parentIndex == 0); // Root's original sibling, untouched

    TEST_ASSERT(CelsSlotReaderClose(&reader) == CELS_OK);

    // Pass 3: A loses its child again. Root's B must still be found via a
    // clean cache hit (not re-inserted as a duplicate) once the ancestor
    // bounds shrink back down after pruning.
    CelsComposerBegin(&cmp, &table);
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_DUPCHECK_ROOT));
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_DUPCHECK_A));
    CelsComposerGroupEnd(&cmp); // Close A: child vanished, pruned in O(1)
    TEST_ASSERT(CelsComposerGroupStart(&cmp, KEY_DUPCHECK_B));
    CelsComposerGroupEnd(&cmp);
    CelsComposerGroupEnd(&cmp);

    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 3);

    printf("  PASSED: TestGroupStartRespectsParentSubtreeBound\n");
}

int
main(void)
{
    printf("====================================================\n");
    printf(" Starting Cels Composer & Diffing Engine Test Suite\n");
    printf("====================================================\n");

    TestComposerFullLifecycle();
    TestMultipleParametersDiffing();
    TestDslMacroAmbientContext();
    TestStyleBComposeAndRemember();
    TestHeaderVariantsAndCompositionScope();
    TestEcsQueryObserver();
    TestFlecsQueryObservableDiff();
    TestClayStyleNamesAndCompositionLookup();
    TestGroupStartRespectsParentSubtreeBound();

    printf("====================================================\n");
    printf(" All Composer test suites PASSED successfully!\n");
    printf("====================================================\n");
    return 0;
}
