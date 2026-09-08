/**
 * @file main.c
 * @brief A runnable tour of CELS: state changes in, mounts and destroys out.
 *
 * The whole engine reports exactly two things — a composable appeared, or a
 * composable went away — so that is what this prints. Everything else you see
 * is your own code reacting to those two events.
 *
 * Five scenes, each demonstrating one claim from CELS.md:
 *
 *   1. The first pass mounts the tree.
 *   2. A quiet Recompose costs nothing.
 *   3. A cell write reaches exactly the composables that read it, and a
 *      subtree appears because the state says it should — not because
 *      anything called a spawn function.
 *   4. The subtree disappears the same way.
 *   5. Lifecycle tears the whole Composition down.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cels.h"

/* ========================================================================= */
/* Backend: what a composable actually *is* is entirely your own code        */
/* ========================================================================= */

#define DEMO_MAX_BACKEND 64u

/**
 * Toy backend registry keyed by CelsComposableId, standing in for whatever
 * your app really owns — an entity, a widget, a sound voice. CELS knows
 * nothing about it; the two callbacks below are the entire integration.
 *
 * Read it only within the pass that mounted the composable, as DeathScreen
 * does below. A CelsComposableId is a logical group index, so inserting or
 * pruning renumbers it: an id stored here on one pass can name a different
 * composable on the next. Holding ids across passes needs a stable key of
 * your own — see "Raised, not applied" in CELS.md §7.
 */
static const char *g_backendFor[DEMO_MAX_BACKEND];

/** Mount/prune event tallies. Counted as events, precisely because ids move. */
static uint32_t g_mountEvents = 0;
static uint32_t g_destroyEvents = 0;

/**
 * Fires the instant a composable mounts, before its own body runs.
 *
 * @param composable The composable that appeared.
 * @param parent     Its parent, or CELS_COMPOSABLE_ID_INVALID at a root.
 * @param key        The callsite key it mounted under. Unused here.
 * @param userdata   Unused here.
 */
static void
OnComposableCreated(CelsComposableId composable,
                    CelsComposableId parent,
                    uint32_t key,
                    void *userdata)
{
    (void)key;
    (void)userdata;

    if (composable < DEMO_MAX_BACKEND) {
        g_backendFor[composable] = "backend-object";
    }
    g_mountEvents++;

    if (parent == CELS_COMPOSABLE_ID_INVALID) {
        printf("      + mount   id=%-2u (root)\n", composable);
    } else {
        printf("      + mount   id=%-2u parent=%u\n", composable, parent);
    }
}

/**
 * Fires the instant a composable is pruned, inline, during Recompose.
 *
 * @param composable The composable that vanished.
 * @param userdata   Unused here.
 */
static void
OnComposableDestroyed(CelsComposableId composable, void *userdata)
{
    (void)userdata;

    if (composable < DEMO_MAX_BACKEND) {
        g_backendFor[composable] = NULL;
    }
    g_destroyEvents++;
    printf("      - destroy id=%-2u\n", composable);
}

/* ========================================================================= */
/* Reactive state, written from outside composition                          */
/* ========================================================================= */

CEL_Mutable(PlayerData) {
    int hp;
    int gold;
};

static PlayerData *g_player;

/** Body-run counters, so the output can show what actually re-ran. */
static uint32_t g_hudRuns = 0;
static uint32_t g_deathRuns = 0;
static uint32_t g_inventoryRuns = 0;

/** Style B parameter for Inventory. Never changes, so Inventory never re-runs. */
static int g_inventorySlots = 12;

/** Whether the app still declares the Arena Composition at all. See scene 5. */
static bool g_arenaDeclared = true;

/**
 * Applies damage from outside any composition pass.
 *
 * Builds a whole new value and replaces it in one write, the way Compose code
 * does `state = state.copy(hp = ...)`. This composes nothing: it compares,
 * writes, and queues an invalidation for every subscriber.
 *
 * @param self   Cell to update. Non-NULL.
 * @param amount Damage to apply.
 */
static void
PlayerDataTakeDamage(PlayerData *self, int amount)
{
    PlayerData next = CEL_Watch(self);
    next.hp -= amount;
    cel_update(self, next);
}

/**
 * Heals the player, same shape as PlayerDataTakeDamage.
 *
 * @param self   Cell to update. Non-NULL.
 * @param amount Hit points to restore.
 */
static void
PlayerDataHeal(PlayerData *self, int amount)
{
    PlayerData next = CEL_Watch(self);
    next.hp += amount;
    cel_update(self, next);
}

/* ========================================================================= */
/* The composition: what exists, as a function of state                      */
/* ========================================================================= */

/**
 * The root view. Note there is no create or destroy call anywhere in here —
 * DeathScreen exists precisely while hp <= 0, and CELS works out the rest.
 *
 * @return The completed composition scope.
 */
CEL_CompositionScope
ArenaView(void)
{
    // Lifecycle decides whether a Composition dies; the app decides whether it
    // is declared at all. Without this the destroyed Arena would be re-declared
    // on the same pass that tore it down, and simply come back.
    if (!g_arenaDeclared) {
        return CEL_CompositionDone();
    }

    CEL_Composition(CEL_Name("Arena")) {
        CEL_Compose(CEL_Name("PlayerHud")) {
            const PlayerData player = CEL_Watch(g_player);
            g_hudRuns++;

            if (player.hp <= 0) {
                CEL_Compose(CEL_Name("DeathScreen")) {
                    g_deathRuns++;

                    // Safe on the very pass this mounts: onCreate for THIS
                    // composable already ran, immediately above.
                    const char *const backend = g_backendFor[CEL_Composable()];
                    printf("      . DeathScreen body sees backend: %s\n",
                           backend != NULL ? backend : "(none)");
                }
            }
        }

        // Style B: the parameter is diffed, so this subtree is skipped in
        // O(1) whenever slotCount is unchanged — which is every pass here,
        // since nothing in this demo touches the inventory.
        CEL_Compose(CEL_Name("Inventory"), g_inventorySlots) {
            g_inventoryRuns++;
        }
    }
    return CEL_CompositionDone();
}

/* ========================================================================= */
/* Driver                                                                    */
/* ========================================================================= */

/**
 * Runs one recompose pass and reports what it did.
 *
 * @param session Session to recompose. Non-NULL.
 * @param label   Human-readable description of the scene.
 */
static void
Step(CelsSession *session, const char *label)
{
    const uint32_t hudBefore = g_hudRuns;
    const uint32_t deathBefore = g_deathRuns;
    const uint32_t inventoryBefore = g_inventoryRuns;

    printf("\n  %s\n", label);

    const CelsResult result = CelsSessionRecompose(session);
    if (result != CELS_OK) {
        printf("      ! %s\n", CelsResultToString(result));
        return;
    }

    printf("      bodies run: PlayerHud x%u  DeathScreen x%u  Inventory x%u\n",
           g_hudRuns - hudBefore,
           g_deathRuns - deathBefore,
           g_inventoryRuns - inventoryBefore);
    printf("      mounts so far: %u   destroys so far: %u\n",
           g_mountEvents, g_destroyEvents);
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("===============================================================\n");
    printf(" CELS - state goes in, mounts and destroys come out\n");
    printf("===============================================================\n");

    const PlayerData initial = { .hp = 100, .gold = 0 };
    g_player = CEL_MutableState(PlayerData, initial);
    if (g_player == NULL) {
        printf("failed to allocate the player cell\n");
        return 1;
    }

    CelsSessionConfig config;
    memset(&config, 0, sizeof(config));
    config.compositionScope = ArenaView;
    config.maxComposables = 128;
    config.transactionContext.onCreate = OnComposableCreated;
    config.transactionContext.onDestroy = OnComposableDestroyed;

    CelsSession session;
    const CelsResult init = CelsSessionInit(&session, &config);
    if (init != CELS_OK) {
        printf("session init failed: %s\n", CelsResultToString(init));
        return 1;
    }

    Step(&session, "[1] First pass - the tree mounts");
    Step(&session, "[2] Nothing changed - a quiet Recompose does no work");

    printf("\n  [3] PlayerDataTakeDamage(120) from outside composition\n");
    printf("      hp 100 -> -20, so DeathScreen should appear\n");
    PlayerDataTakeDamage(g_player, 120);
    Step(&session, "      ...then Recompose:");

    printf("\n  [4] PlayerDataHeal(80) - hp back above zero\n");
    PlayerDataHeal(g_player, 80);
    Step(&session, "      ...then Recompose:");

    printf("\n  [5] CelsLifecycleMarkForDestroy(\"Arena\"), and the app stops\n");
    printf("      declaring it. One onDestroy for the root, not one per child.\n");
    const CelsResult marked =
        CelsLifecycleMarkForDestroy(&session, CelsHashString("Arena", 0));
    if (marked != CELS_OK) {
        printf("      ! %s\n", CelsResultToString(marked));
    }
    g_arenaDeclared = false;
    Step(&session, "      ...then Recompose:");

    printf("\n---------------------------------------------------------------\n");
    printf(" What to notice\n");
    printf("---------------------------------------------------------------\n");
    printf("  * Nothing ever called a create or destroy function. DeathScreen\n");
    printf("    mounting IS the effect of hp dropping below zero.\n");
    printf("  * Scene 2 ran no bodies at all. A quiet Recompose is one queue\n");
    printf("    check, not a tree walk.\n");
    printf("  * Inventory shows x0 from scene 3 on. Its Style B parameter did\n");
    printf("    not change, so the whole subtree was skipped in O(1) even\n");
    printf("    though its parent re-ran right above it.\n");
    printf("  * PlayerHud re-ran because it called CEL_Watch on the cell that\n");
    printf("    changed. You subscribe by reading, not by registering.\n");
    printf("  * Scene 3 mounted DeathScreen as id=2, the id Inventory held a\n");
    printf("    moment earlier -- ids are logical indices and renumber as the\n");
    printf("    tree changes shape. Fine to use inside the pass; never store\n");
    printf("    one across passes as a key of your own.\n");
    printf("  * Scene 5 fired ONE destroy, for the root. Callbacks do not\n");
    printf("    cascade; the subtree's bookkeeping was cleaned up silently.\n");
    printf("  * Every callback above fired inline, on this thread, during\n");
    printf("    Recompose - not queued, not deferred.\n");

    CelsSessionDestroy(&session);
    printf("\n  Session destroyed; slab freed in one shot.\n");
    return 0;
}
