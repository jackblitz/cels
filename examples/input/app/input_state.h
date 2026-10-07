#pragma once

#include "cels.h"
#include "cels_input.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Application Mode                                                          */
/* ========================================================================= */

typedef enum AppMode {
    MODE_GAMEPLAY = 0,
    MODE_UI_MENU  = 1
} AppMode;

/* ========================================================================= */
/* Reactive Settings State                                                   */
/* ========================================================================= */

CEL_State(SettingsState) {
    int  volume;          /* 0 - 100 % */
    bool isMuted;         /* True if audio is muted */
    int  graphicsQuality; /* 0: LOW, 1: MED, 2: HIGH, 3: ULTRA */
};

/* ========================================================================= */
/* Reactive Application State                                                */
/* ========================================================================= */

CEL_State(WorkspaceAppState) {
    AppMode mode;

    /* Gameplay Simulation */
    int  playerX;
    int  playerY;
    int  stamina;
    int  attacks;
    char lastGameplayAction[32];
    char lastUiAction[32];
};

#ifdef __cplusplus
}
#endif
