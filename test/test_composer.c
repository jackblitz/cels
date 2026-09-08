#include "cels.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define ALIGNED_SLAB(size, name) __declspec(align(64)) uint8_t name[size]
#else
#define ALIGNED_SLAB(size, name) __attribute__((aligned(64))) uint8_t name[size]
#endif

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("\n[ASSERTION FAILED]: %s at %s:%d\n",                      \
                   #cond, __FILE__, __LINE__);                                 \
            fflush(stdout);                                                    \
            exit(1);                                                           \
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
    CEL_Composition(cmp, table, CEL_Key(KEY_APP_ROOT)) {
        cel_watch(*state) {
            printf("  [Exec] Root Node (State Changed: HP=%d, Shield=%s)\n",
                   state->health, state->has_shield ? "ON" : "OFF");

            // Subtree 1: Mesh
            CEL_Compose(CEL_Key(KEY_CHILD_A)) {
                printf("    [Exec] Mesh Subtree rendering...\n");
                g_meshExecCount++;
            }

            // Subtree 2: Shield VFX (Conditional)
            if (state->has_shield) {
                CEL_Compose(CEL_Key(KEY_CHILD_B)) {
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
    CEL_Compose(CEL_Key(slotKey)) {
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
    CEL_Composition(&table, CEL_Key(0x1000)) {
        TEST_ASSERT(CelsComposerGetCurrent() != NULL);
        ComposableInventoryItem(0x1001, potionCount);
    }
    TEST_ASSERT(CelsComposerGetCurrent() == NULL);
    TEST_ASSERT(g_inventoryItemExecs == 1);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);

    // Pass 2: Identical state -> ComposableInventoryItem should skip in O(1)
    // (Also testing CEL_Scope shorthand alias)
    g_inventoryItemExecs = 0;
    CEL_Scope(&table, CEL_Key(0x1000)) {
        ComposableInventoryItem(0x1001, potionCount);
    }
    TEST_ASSERT(CelsComposerGetCurrent() == NULL);
    TEST_ASSERT(g_inventoryItemExecs == 0);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);

    // Pass 3: Mutated state -> Potion count changes to 10 -> executes
    potionCount = 10;
    g_inventoryItemExecs = 0;
    CEL_Composition(&table, CEL_Key(0x1000)) {
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
    CEL_Compose(CEL_Key(0x2001), *state) {
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
    CEL_Scope(&table, CEL_Key(0x2000)) {
        ComposableItemCard(&item);
    }
    TEST_ASSERT(g_styleBChildExecs == 1);
    TEST_ASSERT(g_lastObservedClicks == 0); // Seeded with 0
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);

    // Pass 2: Identical state -> Style B skips in O(1)!
    g_styleBChildExecs = 0;
    CEL_Scope(&table, CEL_Key(0x2000)) {
        ComposableItemCard(&item);
    }
    TEST_ASSERT(g_styleBChildExecs == 0); // Completely skipped in O(1)!
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 2);

    // Pass 3: State changes (quantity = 2) -> Style B re-executes block
    // clickCount was mutated to 1 in Pass 1, and should be preserved here!
    item.quantity = 2;
    g_styleBChildExecs = 0;
    CEL_Scope(&table, CEL_Key(0x2000)) {
        ComposableItemCard(&item);
    }
    TEST_ASSERT(g_styleBChildExecs == 1);
    TEST_ASSERT(g_lastObservedClicks == 1); // Preserved from Pass 1!

    // Pass 4: State changes (quantity = 3) -> clickCount should now be 2!
    item.quantity = 3;
    g_styleBChildExecs = 0;
    CEL_Scope(&table, CEL_Key(0x2000)) {
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
    CEL_Compose(CEL_Key(0x3002)) {
        g_hudStatusExecs++;
    }
}

void
ComposableHudCard(const HudCardState *state)
{
    CEL_Compose(CEL_Key(0x3001), *state) {
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

CEL_CompositionScope
GameHudScope(const HudCardState *state)
{
    // 1-argument CEL_Composition: assigns and manages its own slot table!
    CEL_Composition(CEL_Key(0x3000)) {
        ComposableHudCard(state);
    }
    return CEL_CompositionDone();
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

    TEST_ASSERT(g_hudCardExecs == 0); // Whole card subtree skipped!
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
    CEL_Dispose(CEL_Key(0x3000));

    // Pass 5: Calling again after dispose -> Fresh mount into newly assigned table!
    g_hudCardExecs = 0;
    g_hudStatusExecs = 0;
    g_lastObservedTaps = -1;
    GameHudScope(&state);

    TEST_ASSERT(g_hudCardExecs == 1); // Fresh mount!
    TEST_ASSERT(g_lastObservedTaps == 0); // Reset to 0 because previous table was freed!

    // Clean up
    CEL_Dispose(CEL_Key(0x3000));

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
TestQueryObserver(void)
{
    printf("Running TestQueryObserver...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    DummyEcsQuery query = { .changeTick = 1, .entityCount = 2 };

    // Pass 1: Initial mount -> CEL_query executes
    g_queryBodyExecs = 0;
    g_queryEntityChildExecs = 0;
    CEL_Composition(&table, CEL_Key(0x4000)) {
        CEL_query(query) {
            g_queryBodyExecs++;
            for (int i = 0; i < query.entityCount; i++) {
                CEL_Compose(CEL_Key(0x4100 + (uint32_t)i)) {
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
    CEL_Composition(&table, CEL_Key(0x4000)) {
        CEL_query(query) {
            g_queryBodyExecs++;
            for (int i = 0; i < query.entityCount; i++) {
                CEL_Compose(CEL_Key(0x4100 + (uint32_t)i)) {
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
    CEL_Composition(&table, CEL_Key(0x4000)) {
        CEL_query(query) {
            g_queryBodyExecs++;
            for (int i = 0; i < query.entityCount; i++) {
                CEL_Compose(CEL_Key(0x4100 + (uint32_t)i)) {
                    g_queryEntityChildExecs++;
                }
            }
        }
    }
    TEST_ASSERT(g_queryBodyExecs == 1); // Recomposed!
    TEST_ASSERT(g_queryEntityChildExecs == 3);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 4);

    printf("  PASSED: TestQueryObserver\n");
}

/* ========================================================================= */
/* Test Suite: CEL_observable Query Diff (Before vs After)             */
/* ========================================================================= */

typedef struct ObservableQuery {
    uint64_t matchTick;
    uint32_t entityCount;
    int totalPartyHp;
} ObservableQuery;

static int g_observableExecCount = 0;
static ObservableQuery g_capturedPrevQuery;
static ObservableQuery g_capturedCurrQuery;

static void
TestObservableQueryDiff(void)
{
    printf("Running TestObservableQueryDiff...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    ObservableQuery query = {
        .matchTick = 100,
        .entityCount = 2,
        .totalPartyHp = 200
    };

    // Pass 1: Initial mount -> CEL_observable executes
    g_observableExecCount = 0;
    memset(&g_capturedPrevQuery, 0, sizeof(g_capturedPrevQuery));
    memset(&g_capturedCurrQuery, 0, sizeof(g_capturedCurrQuery));

    CEL_Composition(&table, CEL_Key(0x5000)) {
        ObservableQuery prevQuery;
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
    CEL_Composition(&table, CEL_Key(0x5000)) {
        ObservableQuery prevQuery;
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
    CEL_Composition(&table, CEL_Key(0x5000)) {
        ObservableQuery prevQuery;
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
    CEL_Composition(&table, CEL_Key(0x5000)) {
        ObservableQuery prevQuery;
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

    printf("  PASSED: TestObservableQueryDiff\n");
}

/* ========================================================================= */
/* Test Suite 8: Nic Barker Clay UI Inspired Names & Composition Lookup      */
/* ========================================================================= */

static void
TestClayStyleNamesAndCompositionLookup(void)
{
    printf("Running TestClayStyleNamesAndCompositionLookup...\n");

    // 1. FNV-1a Hash Distinctness & Index Offsets
    const CelsId nameHud = CEL_Name("PlayerHud");
    const CelsId nameHealth = CEL_Name("HealthBar");
    const CelsId nameMana = CEL_Name("ManaBar");
    TEST_ASSERT(nameHud.id != 0);
    TEST_ASSERT(nameHealth.id != 0);
    TEST_ASSERT(nameMana.id != 0);
    TEST_ASSERT(nameHealth.id != nameMana.id);
    TEST_ASSERT(nameHealth.id != nameHud.id);

    const CelsId slot0 = CEL_Name("Slot", 0);
    const CelsId slot1 = CEL_Name("Slot", 1);
    const CelsId slot1Alias = CEL_NameI("Slot", 1);
    TEST_ASSERT(slot0.id != slot1.id);
    TEST_ASSERT(slot1.id == slot1Alias.id);

    // 2. Parent-Scoped Local Names (Clay CLAY_ID_LOCAL equivalent)
    const CelsId parentCardA = CEL_Name("CardA");
    const CelsId parentCardB = CEL_Name("CardB");
    const CelsId btnA = CEL_NameScoped("CardA", "CloseBtn");
    const CelsId btnB = CEL_NameScoped("CardB", "CloseBtn");
    TEST_ASSERT(btnA.id != btnB.id);
    TEST_ASSERT(parentCardA.id != parentCardB.id);

    // 3. Composition with CEL_Name (replacing raw numbers like 0x0101)
    int health = 100;
    int mana = 50;
    int healthRenderCount = 0;
    int manaRenderCount = 0;

    // Pass 1: Initial mount using CEL_Name instead of raw numbers
    CEL_Composition(CEL_Name("PlayerHud")) {
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
    TEST_ASSERT(healthRef.key == CEL_Name("HealthBar").id);
    TEST_ASSERT(healthRef.group != NULL);
    TEST_ASSERT(healthRef.slots != NULL);
    TEST_ASSERT(*(int *)healthRef.slots == 100); // Verify remembered HP in slot memory!

    CelsCompositionRef manaRef = CEL_FindByName("ManaBar");
    TEST_ASSERT(manaRef.found == true);
    TEST_ASSERT(manaRef.key == CEL_Name("ManaBar").id);

    CelsCompositionRef slotRef2 = CEL_Find(CEL_NameI("InventorySlot", 2));
    TEST_ASSERT(slotRef2.found == true);
    TEST_ASSERT(slotRef2.key == CEL_NameI("InventorySlot", 2).id);

    CelsCompositionRef nonExistentRef = CEL_FindByName("NonExistentNode");
    TEST_ASSERT(nonExistentRef.found == false);

    // 5. State Retention across Recomposition with O(1) skipping
    // Health changes (100 -> 80), Mana is unchanged (50).
    health = 80;
    healthRenderCount = 0;
    manaRenderCount = 0;

    CEL_Composition(CEL_Name("PlayerHud")) {
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

/* ========================================================================= */
/* Test Suite: Composition-Walk Invalidation Gate                            */
/* ========================================================================= */

/*
 * Composition walks DOWN from the root; invalidation arrives at a LEAF from
 * outside. If an unchanged Style B ancestor O(1)-skips, the walk never reaches
 * the invalidated composable, the state changes and the tree silently does not.
 * CELS_GROUP_FLAG_CONTAINS_INVALIDATED is what defeats that skip, and these
 * tests are what prove it still does.
 *
 * Invalidation is driven directly through CelsSlotTableGroupInvalidate rather
 * than through reactive cells, so this suite does not depend on state.c.
 */

#define KEY_GATE_ROOT   0x700
#define KEY_GATE_PARENT 0x701
#define KEY_GATE_CHILD  0x702
#define KEY_GATE_LEAF   0x703

#define GATE_MAX_EVENTS 8u

typedef struct GateParams {
    int primary;
    int secondary;
} GateParams;

/** One onCreate report, plus when it arrived relative to the bodies. */
typedef struct GateMountEvent {
    CelsComposableId composable;
    CelsComposableId parent;
    uint32_t key;
    uint32_t order;
} GateMountEvent;

/** One onDestroy report, plus what the table still looked like at the time. */
typedef struct GatePruneEvent {
    CelsComposableId composable;
    uint32_t order;
    uint32_t groupCountAtDestroy;
    uint32_t keyAtDestroy;
} GatePruneEvent;

static int g_gateParentExecs = 0;
static int g_gateChildExecs = 0;
static int g_gateLeafExecs = 0;
static bool g_gateLeafPresent = true;

static uint32_t g_gateClock = 0;
static uint32_t g_gateLeafBodyOrder = 0;
static GateMountEvent g_gateMountEvents[GATE_MAX_EVENTS];
static uint32_t g_gateMountEventCount = 0;
static GatePruneEvent g_gatePruneEvents[GATE_MAX_EVENTS];
static uint32_t g_gatePruneEventCount = 0;

static void
GateResetCounters(void)
{
    g_gateParentExecs = 0;
    g_gateChildExecs = 0;
    g_gateLeafExecs = 0;
}

static void
GateResetEvents(void)
{
    g_gateClock = 0;
    g_gateLeafBodyOrder = 0;
    g_gateMountEventCount = 0;
    g_gatePruneEventCount = 0;
    memset(g_gateMountEvents, 0, sizeof(g_gateMountEvents));
    memset(g_gatePruneEvents, 0, sizeof(g_gatePruneEvents));
}

static void
GateOnCreate(CelsComposableId composable,
             CelsComposableId parent,
             uint32_t key,
             void *userdata)
{
    (void)userdata;

    if (g_gateMountEventCount >= GATE_MAX_EVENTS) {
        return;
    }

    GateMountEvent *const event = &g_gateMountEvents[g_gateMountEventCount];
    event->composable = composable;
    event->parent = parent;
    event->key = key;
    event->order = ++g_gateClock;
    g_gateMountEventCount++;
}

static void
GateOnDestroy(CelsComposableId composable, void *userdata)
{
    const CelsSlotTable *const table = (const CelsSlotTable *)userdata;

    if (g_gatePruneEventCount >= GATE_MAX_EVENTS || table == NULL) {
        return;
    }

    GatePruneEvent *const event = &g_gatePruneEvents[g_gatePruneEventCount];
    event->composable = composable;
    event->order = ++g_gateClock;
    event->groupCountAtDestroy = CelsSlotTableGroupCount(table);
    event->keyAtDestroy = 0;

    // The pruned composable must still be addressable by its own id here: the
    // reclaim happens only after every unsubscribe and every onDestroy.
    uint32_t phys = 0;
    if (CelsSlotTableGroupToPhysicalIdx(table, composable, &phys) == CELS_OK) {
        event->keyAtDestroy = table->groups[phys].key;
    }

    g_gatePruneEventCount++;
}

/*
 * Root(0) -> Parent(1, Style B) -> Child(2, stateless + cel_watch) ->
 * Leaf(3, Style B). Three different skip mechanisms stacked on one spine, so a
 * gate that only covers Style B still fails this tree at the cel_watch.
 */
static void
ComposableGateTree(const GateParams *parentParams,
                   const GateParams *leafParams,
                   int middleValue)
{
    CEL_Compose(CEL_Key(KEY_GATE_PARENT), *parentParams) {
        g_gateParentExecs++;

        CEL_Compose(CEL_Key(KEY_GATE_CHILD)) {
            cel_watch(middleValue) {
                g_gateChildExecs++;

                if (g_gateLeafPresent) {
                    CEL_Compose(CEL_Key(KEY_GATE_LEAF), *leafParams) {
                        g_gateLeafExecs++;
                        g_gateLeafBodyOrder = ++g_gateClock;
                    }
                }
            }
        }
    }
}

static void
TestGateInvalidatedRunsWithUnchangedParams(void)
{
    printf("Running TestGateInvalidatedRunsWithUnchangedParams...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    CelsComposer cmp;
    const GateParams parentParams = { .primary = 1, .secondary = 2 };
    const GateParams leafParams = { .primary = 3, .secondary = 4 };
    const int middleValue = 7;

    g_gateLeafPresent = true;

    // Pass 1: initial mount. Root, Parent, Child, Leaf in logical order.
    GateResetCounters();
    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }
    TEST_ASSERT(g_gateParentExecs == 1);
    TEST_ASSERT(g_gateChildExecs == 1);
    TEST_ASSERT(g_gateLeafExecs == 1);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 4);

    // Pass 2: nothing changed, nothing invalidated -> Parent skips in O(1).
    GateResetCounters();
    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }
    TEST_ASSERT(g_gateParentExecs == 0);
    TEST_ASSERT(g_gateChildExecs == 0);
    TEST_ASSERT(g_gateLeafExecs == 0);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 1);

    // Pass 3: Parent itself is INVALIDATED while its parameters are identical.
    // Its body must run; its untouched Child subtree must still skip in O(1),
    // because the gate is a gate and not a "recompose everything" switch.
    TEST_ASSERT(CelsSlotTableGroupInvalidate(&table, 1) == CELS_OK);
    TEST_ASSERT((CelsSlotTableGroupFlags(&table, 1)
                 & (uint16_t)CELS_GROUP_FLAG_INVALIDATED) != 0u);
    TEST_ASSERT((CelsSlotTableGroupFlags(&table, 0)
                 & (uint16_t)CELS_GROUP_FLAG_CONTAINS_INVALIDATED) != 0u);

    GateResetCounters();
    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }
    TEST_ASSERT(g_gateParentExecs == 1); // Ran on the flag alone
    TEST_ASSERT(g_gateChildExecs == 0);  // Child's cel_watch still skipped
    TEST_ASSERT(g_gateLeafExecs == 0);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 1);

    // The walk clears both flags on every group it entered.
    TEST_ASSERT(CelsSlotTableGroupFlags(&table, 0)
                == (uint16_t)CELS_GROUP_FLAG_NONE);
    TEST_ASSERT(CelsSlotTableGroupFlags(&table, 1)
                == (uint16_t)CELS_GROUP_FLAG_NONE);

    // Pass 4: the flag is spent, so the O(1) skip is back.
    GateResetCounters();
    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }
    TEST_ASSERT(g_gateParentExecs == 0);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 1);

    printf("  PASSED: TestGateInvalidatedRunsWithUnchangedParams\n");
}

static void
TestGateContainsInvalidatedReachesDescendant(void)
{
    printf("Running TestGateContainsInvalidatedReachesDescendant...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    CelsComposer cmp;
    const GateParams parentParams = { .primary = 10, .secondary = 20 };
    const GateParams leafParams = { .primary = 30, .secondary = 40 };
    const int middleValue = 99;

    g_gateLeafPresent = true;

    // Pass 1: mount the whole spine.
    GateResetCounters();
    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }
    TEST_ASSERT(g_gateLeafExecs == 1);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 4);

    // Verify the spine really is a spine: invalidation has a parentIndex chain
    // to climb, or the rest of this test proves nothing.
    CelsSlotReader reader;
    TEST_ASSERT(CelsSlotTableReaderOpen(&table, &reader) == CELS_OK);
    CelsSlotGroup parentGroup, childGroup, leafGroup;
    TEST_ASSERT(CelsSlotReaderGroupGet(&reader, 1, &parentGroup) == CELS_OK);
    TEST_ASSERT(CelsSlotReaderGroupGet(&reader, 2, &childGroup) == CELS_OK);
    TEST_ASSERT(CelsSlotReaderGroupGet(&reader, 3, &leafGroup) == CELS_OK);
    TEST_ASSERT(parentGroup.key == KEY_GATE_PARENT);
    TEST_ASSERT(parentGroup.parentIndex == 0);
    TEST_ASSERT(childGroup.key == KEY_GATE_CHILD);
    TEST_ASSERT(childGroup.parentIndex == 1);
    TEST_ASSERT(leafGroup.key == KEY_GATE_LEAF);
    TEST_ASSERT(leafGroup.parentIndex == 2);
    TEST_ASSERT(CelsSlotReaderClose(&reader) == CELS_OK);

    // Pass 2: everything identical -> Parent skips its whole subtree in O(1).
    GateResetCounters();
    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }
    TEST_ASSERT(g_gateParentExecs == 0);
    TEST_ASSERT(g_gateLeafExecs == 0);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 1);

    // Pass 3: THE REGRESSION. Invalidate the leaf only. Every parameter on the
    // spine above it is byte-identical to last pass, so without
    // CONTAINS_INVALIDATED the walk O(1)-skips at Parent, never reaches the
    // leaf, and the failure is completely silent.
    TEST_ASSERT(CelsSlotTableGroupInvalidate(&table, 3) == CELS_OK);
    TEST_ASSERT((CelsSlotTableGroupFlags(&table, 3)
                 & (uint16_t)CELS_GROUP_FLAG_INVALIDATED) != 0u);
    TEST_ASSERT((CelsSlotTableGroupFlags(&table, 2)
                 & (uint16_t)CELS_GROUP_FLAG_CONTAINS_INVALIDATED) != 0u);
    TEST_ASSERT((CelsSlotTableGroupFlags(&table, 1)
                 & (uint16_t)CELS_GROUP_FLAG_CONTAINS_INVALIDATED) != 0u);
    TEST_ASSERT((CelsSlotTableGroupFlags(&table, 0)
                 & (uint16_t)CELS_GROUP_FLAG_CONTAINS_INVALIDATED) != 0u);

    GateResetCounters();
    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }

    // Running the ancestors is the ONLY way down, so all three bodies ran.
    TEST_ASSERT(g_gateParentExecs == 1);
    TEST_ASSERT(g_gateChildExecs == 1);
    TEST_ASSERT(g_gateLeafExecs == 1);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 0);
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 4);

    // Flags cleared on the way through, on every group the walk entered.
    for (uint32_t i = 0; i < 4u; i++) {
        TEST_ASSERT(CelsSlotTableGroupFlags(&table, i)
                    == (uint16_t)CELS_GROUP_FLAG_NONE);
    }

    // Pass 4: with the flags spent, the O(1) skip must come straight back —
    // a flag that stuck would quietly turn every later pass into a full walk.
    GateResetCounters();
    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }
    TEST_ASSERT(g_gateParentExecs == 0);
    TEST_ASSERT(g_gateChildExecs == 0);
    TEST_ASSERT(g_gateLeafExecs == 0);
    TEST_ASSERT(CelsComposerSkipCount(&cmp) == 1);

    printf("  PASSED: TestGateContainsInvalidatedReachesDescendant\n");
}

static void
TestGateMountAndPruneCallbacks(void)
{
    printf("Running TestGateMountAndPruneCallbacks...\n");

    ALIGNED_SLAB(4096, slab);
    CelsSlotTable table;
    const CelsResult initRes =
        CelsSlotTableInit(&table, slab, sizeof(slab), 16);
    TEST_ASSERT(initRes == CELS_OK);

    CelsComposer cmp;
    GateParams parentParams = { .primary = 5, .secondary = 6 };
    const GateParams leafParams = { .primary = 7, .secondary = 8 };
    int middleValue = 11;

    CelsTransactionContext context;
    memset(&context, 0, sizeof(context));
    context.onCreate = GateOnCreate;
    context.onDestroy = GateOnDestroy;
    context.userdata = &table;
    CelsTransactionContextSet(&context);

    // Pass 1: everything mounts. onCreate must fire for each composable BEFORE
    // that composable's own body runs.
    GateResetCounters();
    GateResetEvents();
    g_gateLeafPresent = true;
    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }

    TEST_ASSERT(g_gateLeafExecs == 1);
    TEST_ASSERT(g_gateMountEventCount == 4);
    TEST_ASSERT(g_gatePruneEventCount == 0);

    // Root: reported with no parent, under its own callsite key.
    TEST_ASSERT(g_gateMountEvents[0].composable == 0);
    TEST_ASSERT(g_gateMountEvents[0].parent == CELS_COMPOSABLE_ID_INVALID);
    TEST_ASSERT(g_gateMountEvents[0].key == KEY_GATE_ROOT);

    TEST_ASSERT(g_gateMountEvents[1].composable == 1);
    TEST_ASSERT(g_gateMountEvents[1].parent == 0);
    TEST_ASSERT(g_gateMountEvents[1].key == KEY_GATE_PARENT);

    TEST_ASSERT(g_gateMountEvents[2].composable == 2);
    TEST_ASSERT(g_gateMountEvents[2].parent == 1);
    TEST_ASSERT(g_gateMountEvents[2].key == KEY_GATE_CHILD);

    TEST_ASSERT(g_gateMountEvents[3].composable == 3);
    TEST_ASSERT(g_gateMountEvents[3].parent == 2);
    TEST_ASSERT(g_gateMountEvents[3].key == KEY_GATE_LEAF);

    // The leaf's own onCreate landed strictly before the leaf's own body, which
    // is what lets a body read whatever its onCreate set up on the mount pass.
    TEST_ASSERT(g_gateLeafBodyOrder != 0);
    TEST_ASSERT(g_gateMountEvents[3].order < g_gateLeafBodyOrder);

    // Pass 2: the leaf's branch vanishes. Both spine values change so the walk
    // actually descends far enough to notice the leaf is gone.
    GateResetCounters();
    GateResetEvents();
    g_gateLeafPresent = false;
    parentParams.primary = 500;
    middleValue = 12;

    const uint32_t countBeforePrune = CelsSlotTableGroupCount(&table);
    TEST_ASSERT(countBeforePrune == 4);

    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }

    TEST_ASSERT(g_gateParentExecs == 1);
    TEST_ASSERT(g_gateChildExecs == 1);
    TEST_ASSERT(g_gateLeafExecs == 0);
    TEST_ASSERT(g_gateMountEventCount == 0);

    // onDestroy fired exactly once, for the leaf, by its own id.
    TEST_ASSERT(g_gatePruneEventCount == 1);
    TEST_ASSERT(g_gatePruneEvents[0].composable == 3);

    // Unsubscribe-then-report-then-reclaim: at the moment onDestroy ran, the
    // leaf's group was still live and still addressable by its id. Had the
    // space been reclaimed first, the id would already belong to whatever
    // occupies that slot next, and a surviving subscription would invalidate
    // that stranger instead.
    TEST_ASSERT(g_gatePruneEvents[0].groupCountAtDestroy == countBeforePrune);
    TEST_ASSERT(g_gatePruneEvents[0].keyAtDestroy == KEY_GATE_LEAF);

    // Reclaimed only after the callback returned.
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 3);

    CelsTransactionContextSet(NULL);

    // With no context published, mount and prune are silent again.
    GateResetEvents();
    g_gateLeafPresent = true;
    parentParams.primary = 501;
    middleValue = 13;
    CEL_Composition(&cmp, &table, CEL_Key(KEY_GATE_ROOT)) {
        ComposableGateTree(&parentParams, &leafParams, middleValue);
    }
    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 4);
    TEST_ASSERT(g_gateMountEventCount == 0);
    TEST_ASSERT(g_gatePruneEventCount == 0);

    printf("  PASSED: TestGateMountAndPruneCallbacks\n");
}

#define KEY_SHIFT_ROOT  0x400
#define KEY_SHIFT_EARLY 0x401
#define KEY_SHIFT_LATE  0x402
#define KEY_SHIFT_DEEP  0x403

/**
 * Proves parentIndex survives a structural insertion made before an existing
 * subtree.
 *
 * parentIndex names a LOGICAL index, so inserting a group renumbers everything
 * after it. If the stored links are not renumbered with it, the chain
 * CelsSlotTableGroupInvalidate climbs points at unrelated groups, and
 * CONTAINS_INVALIDATED lands somewhere that lets the walk O(1)-skip the
 * composable that actually changed. Nothing observable fails at the moment the
 * links go stale, which is exactly why this needs its own test.
 *
 * Pass one builds Root -> Late -> Deep. Pass two inserts Early ahead of Late,
 * pushing Late and Deep up by one, then checks the chain from Deep still
 * reaches Root through Late.
 */
static void
TestParentIndexSurvivesInsertionRenumbering(void)
{
    printf("\n[TEST]: ParentIndexSurvivesInsertionRenumbering\n");

    ALIGNED_SLAB(8192, slab);
    CelsSlotTable table;
    TEST_ASSERT(CelsSlotTableInit(&table, slab, sizeof(slab), 64) == CELS_OK);

    CelsComposer composer;
    memset(&composer, 0, sizeof(composer));

    // Pass 1: Root -> Late -> Deep, with no Early sibling yet.
    CelsComposerBegin(&composer, &table);
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_ROOT));
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_LATE));
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_DEEP));
    CelsComposerGroupEnd(&composer);
    CelsComposerGroupEnd(&composer);
    CelsComposerGroupEnd(&composer);

    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 3);

    // Deep sat at logical 2 under Late at logical 1.
    uint32_t physical = 0;
    TEST_ASSERT(CelsSlotTableGroupToPhysicalIdx(&table, 2, &physical)
                == CELS_OK);
    TEST_ASSERT(table.groups[physical].key == KEY_SHIFT_DEEP);
    TEST_ASSERT(table.groups[physical].parentIndex == 1);

    // Pass 2: an Early sibling appears before Late, renumbering it and Deep.
    CelsComposerBegin(&composer, &table);
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_ROOT));
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_EARLY));
    CelsComposerGroupEnd(&composer);
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_LATE));
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_DEEP));
    CelsComposerGroupEnd(&composer);
    CelsComposerGroupEnd(&composer);
    CelsComposerGroupEnd(&composer);

    TEST_ASSERT(CelsSlotTableGroupCount(&table) == 4);

    // Deep is now logical 3, Late logical 2, and the link must have moved too.
    TEST_ASSERT(CelsSlotTableGroupToPhysicalIdx(&table, 3, &physical)
                == CELS_OK);
    TEST_ASSERT(table.groups[physical].key == KEY_SHIFT_DEEP);
    const uint32_t deepParent = table.groups[physical].parentIndex;
    TEST_ASSERT(deepParent == 2);

    TEST_ASSERT(CelsSlotTableGroupToPhysicalIdx(&table, deepParent, &physical)
                == CELS_OK);
    TEST_ASSERT(table.groups[physical].key == KEY_SHIFT_LATE);

    // The payoff: invalidating Deep must mark its real ancestors, so neither
    // can be skipped on the way down to it.
    CelsSlotTableClearAllFlags(&table);
    TEST_ASSERT(CelsSlotTableGroupInvalidate(&table, 3) == CELS_OK);

    TEST_ASSERT((CelsSlotTableGroupFlags(&table, 3)
                 & (uint16_t)CELS_GROUP_FLAG_INVALIDATED) != 0u);
    TEST_ASSERT((CelsSlotTableGroupFlags(&table, 2)
                 & (uint16_t)CELS_GROUP_FLAG_CONTAINS_INVALIDATED) != 0u);
    TEST_ASSERT((CelsSlotTableGroupFlags(&table, 0)
                 & (uint16_t)CELS_GROUP_FLAG_CONTAINS_INVALIDATED) != 0u);

    // Early is a bystander: it must not have been dragged in by a stale link.
    TEST_ASSERT(CelsSlotTableGroupFlags(&table, 1)
                == (uint16_t)CELS_GROUP_FLAG_NONE);

    printf("  PASSED: TestParentIndexSurvivesInsertionRenumbering\n");
}

CEL_Mutable(AliasProbe) {
    int value;
};

/**
 * Proves a pruned composable's subscription cannot alias onto the composable
 * that later reuses its id.
 *
 * Ids are logical group indices and pruning reclaims them immediately, so a
 * subscription outliving its composable does not leak harmlessly — it aliases,
 * and the next update invalidates whatever moved in. The composer unsubscribes
 * before reclaiming to prevent it; this is the test that would actually catch
 * the failure, which the unsubscribe path did not have while the cell module
 * was still a skeleton.
 *
 * Pass one subscribes a composable to a cell. Pass two prunes it and mounts a
 * different composable that takes the same id without ever reading the cell.
 * Updating the cell must then invalidate nothing.
 */
static void
TestPrunedSubscriptionDoesNotAliasReusedId(void)
{
    printf("\n[TEST]: PrunedSubscriptionDoesNotAliasReusedId\n");

    CelsMutableStateResetPool();

    const AliasProbe initial = { .value = 1 };
    AliasProbe *const cell = CEL_MutableState(AliasProbe, initial);
    TEST_ASSERT(cell != NULL);

    ALIGNED_SLAB(8192, slab);
    CelsCompositionHost host;
    memset(&host, 0, sizeof(host));
    TEST_ASSERT(CelsSlotTableInit(&host.slotTable, slab, sizeof(slab), 64)
                == CELS_OK);

    CelsComposer composer;
    memset(&composer, 0, sizeof(composer));

    // The composer subscribes reads against whichever host is published.
    CelsInvalidationContextSet(&host);

    // Pass 1: Root -> Watcher, and Watcher reads the cell.
    CelsComposerBegin(&composer, &host.slotTable);
    CelsComposerSetCurrent(&composer);
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_ROOT));
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_EARLY));
    const CelsComposableId watcherId =
        CelsComposerGetCurrentComposable(&composer);
    AliasProbe seen = { .value = 0 };
    TEST_ASSERT(CelsMutableStateRead(cell, &seen, sizeof(seen)) == CELS_OK);
    TEST_ASSERT(seen.value == 1);
    CelsComposerGroupEnd(&composer);
    CelsComposerGroupEnd(&composer);

    // An update now reaches the subscriber, confirming it really subscribed.
    host.isDirty = false;
    host.invalidationCount = 0;
    AliasProbe next = { .value = 2 };
    TEST_ASSERT(CelsMutableStateUpdate(cell, &next, sizeof(next)) == CELS_OK);
    TEST_ASSERT(host.isDirty);

    // Pass 2: Watcher vanishes and a different composable takes its id.
    CelsComposerBegin(&composer, &host.slotTable);
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_ROOT));
    TEST_ASSERT(CelsComposerGroupStart(&composer, KEY_SHIFT_LATE));
    const CelsComposableId successorId =
        CelsComposerGetCurrentComposable(&composer);
    CelsComposerGroupEnd(&composer);
    CelsComposerGroupEnd(&composer);

    // The successor genuinely occupies the pruned composable's id — without
    // this the test would pass for the wrong reason.
    TEST_ASSERT(successorId == watcherId);

    // The successor never read the cell, so updating it must invalidate
    // nothing. A surviving subscription would dirty the host here.
    host.isDirty = false;
    host.invalidationCount = 0;
    AliasProbe third = { .value = 3 };
    TEST_ASSERT(CelsMutableStateUpdate(cell, &third, sizeof(third)) == CELS_OK);
    TEST_ASSERT(!host.isDirty);
    TEST_ASSERT(host.invalidationCount == 0);

    CelsComposerSetCurrent(NULL);
    CelsInvalidationContextSet(NULL);
    CelsMutableStateResetPool();

    printf("  PASSED: TestPrunedSubscriptionDoesNotAliasReusedId\n");
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("====================================================\n");
    printf(" Starting Cels Composer & Diffing Engine Test Suite\n");
    printf("====================================================\n");

    TestComposerFullLifecycle();
    TestMultipleParametersDiffing();
    TestDslMacroAmbientContext();
    TestStyleBComposeAndRemember();
    TestHeaderVariantsAndCompositionScope();
    TestQueryObserver();
    TestObservableQueryDiff();
    TestClayStyleNamesAndCompositionLookup();
    TestGroupStartRespectsParentSubtreeBound();
    TestGateInvalidatedRunsWithUnchangedParams();
    TestGateContainsInvalidatedReachesDescendant();
    TestGateMountAndPruneCallbacks();
    TestParentIndexSurvivesInsertionRenumbering();
    TestPrunedSubscriptionDoesNotAliasReusedId();

    printf("====================================================\n");
    printf(" All Composer test suites PASSED successfully!\n");
    printf("====================================================\n");
    return 0;
}
