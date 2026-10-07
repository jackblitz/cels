#include "cels.h"
#include "transition_state.h"
#include "cels_input.h"
#include "tui_renderer.h"
#include "common_events.h"
#include <stdio.h>

/**
 * Composable Health Bar HUD.
 * Demonstrates declarative temporal interpolation using cel_transition rendered in a TUI window.
 */
CEL_Composable(HealthBarHUD, const PlayerGaugeState*, gauge) {
    cel_watch(gauge);
    if (gauge == NULL) return;

    /* cel_transition smoothly interpolates display value toward authoritative target */
    float visualHealth = cel_transition(gauge->currentHealth, 400, CEL_EASE_OUT_QUAD);

    TuiWindow("CELS Temporal Motion & Interpolation", "Profile: 512");
    TuiPrint("Target (Authoritative): %5.1f / %5.1f HP", gauge->currentHealth, gauge->maxHealth);
    TuiPrint("Current (Visual):       %5.1f / %5.1f HP", visualHealth, gauge->maxHealth);
    TuiPrint("Player Status:          %s", gauge->isAlive ? "ALIVE" : "DEFEATED");
    TuiDivider();
    TuiProgressBar("Health", visualHealth, gauge->maxHealth, "EASE_OUT_QUAD (400ms)");
    TuiFooter("Controls: [D] -25 Damage | [H] +25 Heal | [Q] Quit");
}

typedef enum TransitionAction {
    TRANS_ACTION_DAMAGE = 1,
    TRANS_ACTION_HEAL
} TransitionAction;

static const CelsKeyBinding g_transitionBindings[] = {
    { 'd', TRANS_ACTION_DAMAGE },
    { 'h', TRANS_ACTION_HEAL }
};
static const CelsInputMap g_transitionMap = CELS_INPUT_MAP("TransitionMap", g_transitionBindings);

/**
 * Root composition initializing and observing the player health state.
 */
CEL_Composition(TransitionAppComposition) {
    PlayerGaugeState *gauge = cel_remember_state(PlayerGaugeState, {
        .currentHealth = 100.0f,
        .maxHealth     = 100.0f,
        .isAlive       = true
    });

    /* Route active input mapping down the transition subtree */
    cel_set_context(CelsInputMap, &g_transitionMap);

    if (cel_action_consume(TRANS_ACTION_DAMAGE)) {
        cel_mutate(gauge) {
            this->currentHealth -= 25.0f;
            if (this->currentHealth < 0.0f) this->currentHealth = 0.0f;
            this->isAlive = (this->currentHealth > 0.0f);
        }
    } else if (cel_action_consume(TRANS_ACTION_HEAL)) {
        cel_mutate(gauge) {
            this->currentHealth += 25.0f;
            if (this->currentHealth > this->maxHealth) this->currentHealth = this->maxHealth;
            this->isAlive = true;
        }
    }

    /* 2. Targeted Signals: React to external signals sent to this session */
    cel_connect(HealthActionSignal, sig) {
        cel_mutate(gauge) {
            this->currentHealth += sig->delta;
            if (this->currentHealth < 0.0f) this->currentHealth = 0.0f;
            if (this->currentHealth > this->maxHealth) this->currentHealth = this->maxHealth;
            this->isAlive = (this->currentHealth > 0.0f);
        }
    }

    HealthBarHUD(gauge);
}

/**
 * Declarative Application Root.
 */
CEL_App(TransitionApp, TransitionAppComposition);
