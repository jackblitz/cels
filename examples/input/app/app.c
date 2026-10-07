#include "cels.h"
#include "input_state.h"
#include "cels_layout.h"
#include "tui_renderer.h"
#include <stdio.h>
#include <string.h>

/* ========================================================================= */
/* Declarative Action Maps                                                   */
/* ========================================================================= */

static const CelsKeyBinding g_gameplayBindings[] = {
    { 'w', ACTION_MOVE_UP },    { 'W', ACTION_MOVE_UP },    { 0x103, ACTION_MOVE_UP },
    { 's', ACTION_MOVE_DOWN },  { 'S', ACTION_MOVE_DOWN },  { 0x102, ACTION_MOVE_DOWN },
    { 'a', ACTION_MOVE_LEFT },  { 'A', ACTION_MOVE_LEFT },  { 0x104, ACTION_MOVE_LEFT },
    { 'd', ACTION_MOVE_RIGHT }, { 'D', ACTION_MOVE_RIGHT }, { 0x105, ACTION_MOVE_RIGHT },
    { ' ', ACTION_ATTACK },     { '\r', ACTION_ATTACK },    { '\n', ACTION_ATTACK },
    { '\t', ACTION_TOGGLE_MENU }
};
static const CelsInputMap g_gameplayMap = CELS_INPUT_MAP("GameplayMap", g_gameplayBindings);

static const CelsKeyBinding g_uiMenuBindings[] = {
    { 'w', ACTION_NAV_PREV },   { 'W', ACTION_NAV_PREV },   { 0x103, ACTION_NAV_PREV },
    { 's', ACTION_NAV_NEXT },   { 'S', ACTION_NAV_NEXT },   { 0x102, ACTION_NAV_NEXT },
    { 'a', ACTION_VALUE_DEC },  { 'A', ACTION_VALUE_DEC },  { 0x104, ACTION_VALUE_DEC },
    { 'd', ACTION_VALUE_INC },  { 'D', ACTION_VALUE_INC },  { 0x105, ACTION_VALUE_INC },
    { ' ', ACTION_SUBMIT },     { '\r', ACTION_SUBMIT },    { '\n', ACTION_SUBMIT },
    { 0x157, ACTION_SUBMIT },   { 0x1bf, ACTION_SUBMIT },
    { '\t', ACTION_TOGGLE_MENU }
};
static const CelsInputMap g_uiMenuMap = CELS_INPUT_MAP("UiMenuMap", g_uiMenuBindings);

/* ========================================================================= */
/* Gameplay View: Self-contained context declaration                         */
/* ========================================================================= */

CEL_Composable(GameWorldPanel, WorkspaceAppState*, state)
{
    /* Publish gameplay input mapping down this composable's scope */
    cel_set_context(CelsInputMap, &g_gameplayMap);

    if (cel_action_consume(ACTION_MOVE_UP)) {
        cel_mutate(state) {
            this->playerY++;
            snprintf(this->lastGameplayAction, sizeof(this->lastGameplayAction), "Moved North [W]");
        }
    } else if (cel_action_consume(ACTION_MOVE_DOWN)) {
        cel_mutate(state) {
            this->playerY--;
            snprintf(this->lastGameplayAction, sizeof(this->lastGameplayAction), "Moved South [S]");
        }
    } else if (cel_action_consume(ACTION_MOVE_LEFT)) {
        cel_mutate(state) {
            this->playerX--;
            snprintf(this->lastGameplayAction, sizeof(this->lastGameplayAction), "Moved West [A]");
        }
    } else if (cel_action_consume(ACTION_MOVE_RIGHT)) {
        cel_mutate(state) {
            this->playerX++;
            snprintf(this->lastGameplayAction, sizeof(this->lastGameplayAction), "Moved East [D]");
        }
    } else if (cel_action_consume(ACTION_ATTACK)) {
        cel_mutate(state) {
            this->attacks++;
            snprintf(this->lastGameplayAction, sizeof(this->lastGameplayAction), "Attacked [Space]");
        }
    }

    TuiPrint(">> [WORLD SIMULATION: PLAYER CHARACTER ACTIVE]");
    TuiPrint("Coordinates:    X: %3d | Y: %3d | Facing: NORTH | Attacks: %d",
             state->playerX, state->playerY, state->attacks);
    TuiProgressBar("Stamina", (float)state->stamina, 100.0f, "READY");
    TuiPrint("Last Event:     %s", state->lastGameplayAction[0] ? state->lastGameplayAction : "Idle");
    TuiPrint("Controls:       [W/A/S/D] or [Arrows] Move | [Space/Enter] Attack | [Tab] Open Pause Menu");
}

/* ========================================================================= */
/* Focusable Child Widgets for UI Menu                                       */
/* ========================================================================= */

CEL_Composable(VolumeSlider, SettingsState*, settings)
{
    if (cel_focusable()) {
        if (cel_action_consume(ACTION_VALUE_INC)) {
            cel_mutate(settings) {
                this->volume = (this->volume + 5 <= 100) ? this->volume + 5 : 100;
            }
        } else if (cel_action_consume(ACTION_VALUE_DEC)) {
            cel_mutate(settings) {
                this->volume = (this->volume - 5 >= 0) ? this->volume - 5 : 0;
            }
        }
        TuiPrint(">> [1. MASTER VOLUME]     < %3d %% >  (Adjust: [A]/[D] or Left/Right)", settings->volume);
    } else {
        TuiPrint("   [1. Master Volume]       %3d %%", settings->volume);
    }
    TuiProgressBar("Volume", (float)settings->volume, 100.0f, settings->isMuted ? "MUTED" : "");
}

CEL_Composable(MuteToggle, SettingsState*, settings)
{
    if (cel_focusable()) {
        if (cel_action_consume(ACTION_SUBMIT) ||
            cel_action_consume(ACTION_VALUE_INC) ||
            cel_action_consume(ACTION_VALUE_DEC)) {
            cel_mutate(settings) {
                this->isMuted = !this->isMuted;
            }
        }
        TuiPrint(">> [2. AUDIO MUTE]        [ %s ]  (Toggle: [Space], [Enter] or [A]/[D])",
                 settings->isMuted ? "MUTED" : "UNMUTED / PLAYING");
    } else {
        TuiPrint("   [2. Audio Mute]          [ %s ]",
                 settings->isMuted ? "MUTED" : "UNMUTED / PLAYING");
    }
}

CEL_Composable(GraphicsSelector, SettingsState*, settings)
{
    static const char *kPresets[] = { "LOW", "MEDIUM", "HIGH", "ULTRA" };
    int q = settings->graphicsQuality;
    if (q < 0 || q >= 4) q = 0;

    if (cel_focusable()) {
        if (cel_action_consume(ACTION_VALUE_INC) || cel_action_consume(ACTION_SUBMIT)) {
            cel_mutate(settings) {
                this->graphicsQuality = (this->graphicsQuality + 1) % 4;
            }
        } else if (cel_action_consume(ACTION_VALUE_DEC)) {
            cel_mutate(settings) {
                this->graphicsQuality = (this->graphicsQuality + 3) % 4;
            }
        }
        TuiPrint(">> [3. GRAPHICS PRESET]   < %s >  (Cycle: [A]/[D], Arrows or [Space])", kPresets[settings->graphicsQuality]);
    } else {
        TuiPrint("   [3. Graphics Preset]     %s", kPresets[q]);
    }
}

CEL_Composable(ResumeButton, WorkspaceAppState*, appState)
{
    if (cel_focusable()) {
        if (cel_action_consume(ACTION_SUBMIT)) {
            cel_mutate(appState) {
                this->mode = MODE_GAMEPLAY;
            }
        }
        TuiPrint(">> [4. RESUME GAME]       [ PRESS SPACE / ENTER TO RESUME ]");
    } else {
        TuiPrint("   [4. Resume Game]");
    }
}

/* ========================================================================= */
/* UI Menu View: Self-contained context declaration + CEL_Layout             */
/* ========================================================================= */

CEL_Composable(PauseSettingsMenu, WorkspaceAppState*, state, SettingsState*, settings)
{
    /* Publish UI menu input mapping down this menu subtree */
    cel_set_context(CelsInputMap, &g_uiMenuMap);

    TuiPrint(">> [PAUSE & SETTINGS MENU: UI FOCUS ACTIVE]");

    /* Direct CEL_Layout invocation — zero #define required! */
    CEL_Layout(CEL_LAYOUT_DIR_VERT) {
        VolumeSlider(settings);
        MuteToggle(settings);
        GraphicsSelector(settings);
        ResumeButton(state);
    }

    TuiPrint("UI Controls:  [W/S/Up/Down] Focus | [A/D/Left/Right] Adjust | [Space/Enter] Select");
}

/* ========================================================================= */
/* Root Composition: Flat, clean, zero wrapper hell                          */
/* ========================================================================= */

CEL_Composition(InputAppComposition)
{
    WorkspaceAppState *state = cel_remember_state(WorkspaceAppState, {
        .mode = MODE_GAMEPLAY,
        .playerX = 10,
        .playerY = 15,
        .stamina = 100,
        .attacks = 0,
        .lastGameplayAction = {0},
        .lastUiAction = {0}
    });

    SettingsState *settings = cel_remember_state(SettingsState, {
        .volume = 65,
        .isMuted = false,
        .graphicsQuality = 2 /* HIGH */
    });

    cel_watch(state);
    cel_watch(settings);

    /* Global toggle: Press [Tab] to toggle between Gameplay and Pause UI */
    if (cel_key_consume('\t')) {
        cel_mutate(state) {
            this->mode = (this->mode == MODE_GAMEPLAY) ? MODE_UI_MENU : MODE_GAMEPLAY;
        }
    }

    TuiWindow("CELS Input Manager & Focus Scoping",
              state->mode == MODE_GAMEPLAY ? "Mode: [GAMEPLAY]" : "Mode: [PAUSE UI]");

    /* Clean unwrapped routing: each view declares its own ambient dependencies! */
    if (state->mode == MODE_GAMEPLAY) {
        GameWorldPanel(state);
    } else {
        PauseSettingsMenu(state, settings);
    }

    TuiFooter("Workspace Controls: [Tab] Switch Mode | [Q] Quit Application");
}

CEL_App(InputApp, InputAppComposition);
