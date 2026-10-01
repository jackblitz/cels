#pragma once

#include "cels.h"


/**
 * Reactive player gauge state structure.
 * Fields are ordered in descending alignment (float 4-byte, bool 1-byte).
 */
CEL_State(PlayerGaugeState) {
    float currentHealth;
    float maxHealth;
    bool  isAlive;
};

typedef struct HealthActionSignal {
    float delta;
} HealthActionSignal;

