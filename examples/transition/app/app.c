#include "cels.h"
#include "transition_state.h"
#include <stdio.h>

/**
 * Composable Health Bar HUD.
 * Demonstrates declarative temporal interpolation using cel_transition.
 */
CEL_Composable(HealthBarHUD, const PlayerGaugeState*, gauge) {
    cel_watch(gauge);
    if (gauge == NULL) return;

    /* cel_transition smoothly interpolates display value toward authoritative target */
    float visualHealth = cel_transition(gauge->currentHealth, 400, CEL_EASE_OUT_QUAD);

    int currentPercent = (int)((visualHealth / gauge->maxHealth) * 100.0f);
    if (currentPercent < 0) currentPercent = 0;
    if (currentPercent > 100) currentPercent = 100;
    int filled = currentPercent / 5; /* 20 blocks max */

    printf("\r[HUD] Health: [");
    for (int i = 0; i < 20; ++i) {
        putchar(i < filled ? '=' : ' ');
    }
    printf("] %3d%% (visual: %5.1f / auth: %5.1f)  ",
           currentPercent, visualHealth, gauge->currentHealth);
    fflush(stdout);
}

/**
 * Root composition initializing and observing the player health state.
 */
CEL_Composition(TransitionAppComposition) {
    PlayerGaugeState *gauge = cel_remember_state(PlayerGaugeState, {
        .currentHealth = 100.0f,
        .maxHealth     = 100.0f,
        .isAlive       = true
    });

    HealthBarHUD(gauge);
}

/**
 * Declarative Application Root.
 */
CEL_App(TransitionApp, TransitionAppComposition);

