#include "cels.h"
#include "component_state.h"
#include "cels_input.h"
#include "tui_renderer.h"
#include "common_events.h"
#include <stdio.h>

/**
 * Child Composable: EntityNode
 *
 * Declares an entity node with an explicit 64-bit ID parameter.
 * Demonstrates:
 * - CEL_Composable(EntityNode, id, ...): Keyed composable taking entity ID as first parameter
 * - cel_has: Automatic component attachment & removal diffing
 * - cel_has_tag: Zero-sized tag attachment
 * - cel_get & cel_is: Immediate intra-frame component queries
 */
CEL_Composable(EntityNode, id, const CombatEntityState*, ent)
{
    cel_watch(ent);
    if (!ent->isAlive) return;

    /* 1. Unconditional components (automatically bound to id / cel_id()) */
    cel_has(Position, { .x = ent->x, .y = ent->y, .z = ent->z });
    cel_has(Health,   { .current = ent->hp, .max = ent->maxHp });

    /* 2. Conditional Tag */
    if (ent->isBoss) {
        cel_has_tag(BossTag);
    }

    /* 3. Conditional Shield:
     * When hasShield is true: attaches EnergyShield.
     * When hasShield is false: CELS automatically stages CELS_OP_REMOVE! */
    if (ent->hasShield) {
        cel_has(EnergyShield, {
            .capacity = ent->shieldCapacity,
            .rechargeRate = 5.0f
        });
    }

    /* 4. Conditional Debuff:
     * When isBurning is true: attaches BurnDebuff.
     * When isBurning is false: CELS automatically stages CELS_OP_REMOVE! */
    if (ent->isBurning) {
        cel_has(BurnDebuff, {
            .dps = ent->burnDps,
            .remainingSeconds = 3.0f
        });
    }

    /* 5. Immediate Intra-Frame Query: inspects staged component in this frame */
    const Health *hp = cel_get(Health);
    if (hp && hp->current < (hp->max * 0.25f)) {
        cel_has_tag(LowHealthAlert);
    }
}

/**
 * Composable HUD: Displays entity state and queries attached components via cel_is/cel_get.
 */
CEL_Composable(CombatEntityHUD, const CombatEntityState*, ent)
{
    cel_watch(ent);

    const Position *pos = cel_get(ent->entityId, Position);
    bool hasShield = cel_is(ent->entityId, EnergyShield);
    bool isBurning = cel_is(ent->entityId, BurnDebuff);
    bool isBoss = cel_is(ent->entityId, BossTag);
    bool isCritical = cel_is(ent->entityId, LowHealthAlert);

    TuiWindow("CELS Declarative ECS Components & Reconciliation", "Profile: 1K");
    TuiPrint("Entity ID:     0x%llX", (unsigned long long)ent->entityId);
    TuiPrint("Lifecycle:     %s", ent->isAlive ? "[ ALIVE / MOUNTED ]" : "[ DEFEATED / UNMOUNTED ]");
    TuiPrint("Archetype:     %s", isBoss ? "[ BOSS ENTITY (BossTag attached) ]" : "[ STANDARD ACTOR ]");
    if (pos) {
        TuiPrint("Position:      (%.1f, %.1f, %.1f) [cel_get: Attached]", pos->x, pos->y, pos->z);
    } else {
        TuiLine("Position:      [ Not Attached / Unmounted ]");
    }
    TuiPrint("Energy Shield: %s", hasShield ? "[ ATTACHED (50.0 cap) ]" : "[ REMOVED / DETACHED ]");
    TuiPrint("Burn Debuff:   %s", isBurning ? "[ ATTACHED (-25 DPS) ]" : "[ CLEAR / DETACHED ]");
    TuiPrint("Critical Alert:%s", isCritical ? " [ ALERT ACTIVE (<25% HP) ]" : " [ NORMAL ]");
    TuiDivider();
    TuiProgressBar("Health", (float)ent->hp, (float)ent->maxHp, "");
    TuiDivider();
    TuiLine("Declarative Reconciliation Status (cel_has / cel_is):");
    if (ent->isAlive) {
        TuiPrint("  Node mounted under ID 0x%llX - All active components reconciled", (unsigned long long)ent->entityId);
    } else {
        TuiLine("  Node pruned from tree - Entity & components automatically unmounted");
    }
    TuiFooter("Controls: [B] Burn | [S] Shield | [D] Damage | [H] Heal | [T] Boss | [K] Kill/Revive | [Q] Quit");
}

typedef enum ComponentAction {
    COMP_ACTION_BURN = 1,
    COMP_ACTION_SHIELD,
    COMP_ACTION_DAMAGE,
    COMP_ACTION_HEAL,
    COMP_ACTION_BOSS,
    COMP_ACTION_REVIVE
} ComponentAction;

static const CelsKeyBinding g_componentBindings[] = {
    { 'b', COMP_ACTION_BURN },
    { 's', COMP_ACTION_SHIELD },
    { 'd', COMP_ACTION_DAMAGE },
    { 'h', COMP_ACTION_HEAL },
    { 't', COMP_ACTION_BOSS },
    { 'k', COMP_ACTION_REVIVE }
};
static const CelsInputMap g_componentMap = CELS_INPUT_MAP("ComponentMap", g_componentBindings);

/**
 * Root Composition: Manages reactive state, handles keyboard actions, and composes nodes.
 */
CEL_Composition(ComponentAppComposition)
{
    CombatEntityState *ent = cel_remember_state(CombatEntityState, {
        .entityId = 0x1001,
        .x = 12.0f, .y = 0.0f, .z = 4.5f,
        .hp = 100, .maxHp = 100,
        .hasShield = true,
        .shieldCapacity = 50.0f,
        .isBurning = false,
        .burnDps = 25.0f,
        .isBoss = false,
        .isAlive = true
    });

    /* Route active input mapping down the entity subtree */
    cel_set_context(CelsInputMap, &g_componentMap);

    if (cel_action_consume(COMP_ACTION_BURN)) {
        cel_mutate(ent) {
            this->isBurning = !this->isBurning;
        }
    } else if (cel_action_consume(COMP_ACTION_SHIELD)) {
        cel_mutate(ent) {
            this->hasShield = !this->hasShield;
        }
    } else if (cel_action_consume(COMP_ACTION_DAMAGE)) {
        cel_mutate(ent) {
            this->hp -= 20;
            if (this->hp < 0) this->hp = 0;
        }
    } else if (cel_action_consume(COMP_ACTION_HEAL)) {
        cel_mutate(ent) {
            this->hp += 20;
            if (this->hp > this->maxHp) this->hp = this->maxHp;
        }
    } else if (cel_action_consume(COMP_ACTION_BOSS)) {
        cel_mutate(ent) {
            this->isBoss = !this->isBoss;
        }
    } else if (cel_action_consume(COMP_ACTION_REVIVE)) {
        cel_mutate(ent) {
            this->isAlive = !this->isAlive;
            if (this->isAlive && this->hp == 0) this->hp = 100;
        }
    }

    /* Mount the keyed entity composable node when alive, passing entityId directly */
    if (ent->isAlive) {
        EntityNode(ent->entityId, ent);
    }

    /* Compose HUD reflecting the attached components */
    CombatEntityHUD(ent);
}

/**
 * Declarative Application Module Definition.
 */
CEL_App(ComponentApp, ComponentAppComposition);
