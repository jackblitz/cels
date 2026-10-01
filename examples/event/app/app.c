#include "cels.h"
#include "event_state.h"
#include "tui_renderer.h"
#include "common_events.h"
#include <stdio.h>
#include <string.h>

/**
 * Child Leaf Composable: ActionButton
 *
 * Emits a discrete local event that bubbles up to parent composables in the active tree.
 */
CEL_Composable(ActionButton, bool, shouldClick) {
    if (shouldClick) {
        cel_event(UserActionEvent, {
            .actionId = 1,
            .cost = 15,
            .actionName = "CastSpell"
        });
    }
}

/**
 * Parent Composable: ControlPanel
 *
 * Listens for local events bubbling up from child components using cel_listen().
 */
CEL_Composable(ControlPanel, char*, lastEvent, int*, eventCount, bool, triggerClick) {
    /* Listen for discrete events bubbling from child composables */
    cel_listen(UserActionEvent, ev) {
        (*eventCount)++;
        snprintf(lastEvent, 64, "action='%s' (cost: %d)", ev->actionName, ev->cost);
    }

    ActionButton(triggerClick);
}

/**
 * Targeted Signal Listener: CombatWidget
 *
 * Listens for targeted signals delivered directly to this session using cel_connect().
 */
CEL_Composable(CombatWidget, char*, lastSignal, int*, signalCount) {
    cel_connect(CombatSignal, sig) {
        (*signalCount)++;
        snprintf(lastSignal, 64, "target=%d, damage=%.1f (crit: %s)",
                 sig->targetId, sig->damage, sig->isCritical ? "YES" : "NO");
    }
}

/**
 * Global Engine Broadcast Listener: AudioMonitor
 *
 * Listens for global engine-wide broadcasts using cel_bind().
 */
CEL_Composable(AudioMonitor, char*, lastBroadcast, int*, broadcastCount) {
    cel_bind(AudioBroadcast, bcast) {
        (*broadcastCount)++;
        snprintf(lastBroadcast, 64, "sfx='%s', volume=%.2f",
                 bcast->soundEffect, bcast->volume);
    }
}

/**
 * Asynchronous Multi-Step Workflow Task
 *
 * Coordinates a multi-step sequence by awaiting events across all communication scopes.
 */
CEL_Task(WorkflowCoordinatorTask, EventDemoState*, state) {
    cancel {
        if (state != NULL) {
            cel_mutate(state) {
                snprintf(this->workflowStatus, sizeof(this->workflowStatus), "Task cancelled.");
            }
        }
    }
    run {
        if (state != NULL) {
            cel_mutate(state) {
                snprintf(this->workflowStatus, sizeof(this->workflowStatus), "Step 1: Waiting for [E] (UserAction)...");
            }
        }
        UserActionEvent userEv;
        bool gotEv = cel_wait_for_timeout(UserActionEvent, &userEv, 5000);
        if (state != NULL) {
            cel_mutate(state) {
                this->workflowStep = 1;
                snprintf(this->workflowStatus, sizeof(this->workflowStatus),
                         gotEv ? "Step 1 done. Waiting for [S] (Combat)..." : "Step 1 timed out. Waiting for [S]...");
            }
        }

        CombatSignal combatSig;
        bool gotSig = cel_wait_for_timeout(CombatSignal, &combatSig, 5000);
        if (state != NULL) {
            cel_mutate(state) {
                this->workflowStep = 2;
                snprintf(this->workflowStatus, sizeof(this->workflowStatus),
                         gotSig ? "Step 2 done. Waiting for [B] (Audio)..." : "Step 2 timed out. Waiting for [B]...");
            }
        }

        AudioBroadcast audioBcast;
        bool gotBcast = cel_wait_for_timeout(AudioBroadcast, &audioBcast, 5000);
        if (state != NULL) {
            cel_mutate(state) {
                this->workflowStep = 3;
                snprintf(this->workflowStatus, sizeof(this->workflowStatus),
                         gotBcast ? "Step 3 done. Checking 40ms timeout..." : "Step 3 timed out. Checking timeout...");
            }
        }

        UserActionEvent timeoutEv;
        (void)cel_wait_for_timeout(UserActionEvent, &timeoutEv, 40);

        if (state != NULL) {
            cel_mutate(state) {
                this->workflowStep = 4;
                this->taskWorkflowCompleted = true;
                this->taskWorkflowActive = false;
                snprintf(this->workflowStatus, sizeof(this->workflowStatus), "Workflow fully completed!");
            }
        }
    }
}

typedef struct EventTextSlot {
    char text[64];
} EventTextSlot;

/**
 * Root Composition for the Event & Messaging Application.
 */
CEL_Composition(EventAppComposition) {
    EventDemoState *state = cel_remember_state(EventDemoState, {
        .treeEventsHandled     = 0,
        .sessionSignalsHandled = 0,
        .broadcastsHandled     = 0,
        .workflowStep          = 0,
        .workflowStatus        = {0},
        .taskWorkflowActive    = false,
        .taskWorkflowCompleted = false
    });

    cel_watch(state);

    EventTextSlot *lastEvent = cel_remember(EventTextSlot, { .text = "None yet (Press [E])" });
    EventTextSlot *lastSignal = cel_remember(EventTextSlot, { .text = "None yet (Press [S])" });
    EventTextSlot *lastBroadcast = cel_remember(EventTextSlot, { .text = "None yet (Press [B])" });

    int *eventCount = cel_remember(int, 0);
    int *signalCount = cel_remember(int, 0);
    int *broadcastCount = cel_remember(int, 0);

    /* Track trigger pulse in slot table */
    bool *triggerChildClick = cel_remember(bool, false);
    bool shouldClick = *triggerChildClick;
    *triggerChildClick = false; /* Reset pulse */

    /* 1. React to common keyboard signals from host */
    cel_connect(CelsKeySignal, sig) {
        if (sig->key == 'e' || sig->key == 'E') {
            cel_event(UserActionEvent, {
                .actionId = 1,
                .cost = 10,
                .actionName = "Shield"
            });
        } else if (sig->key == 's' || sig->key == 'S') {
            CelsSession *sess = CelsGetCurrentSession();
            cel_signal(sess, CombatSignal, {
                .targetId = 20,
                .damage = 45.0f,
                .isCritical = false
            });
        } else if (sig->key == 'b' || sig->key == 'B') {
            cel_broadcast(AudioBroadcast, {
                .soundEffect = "laser.wav",
                .volume = 0.8f
            });
        } else if (sig->key == 't' || sig->key == 'T') {
            cel_mutate(state) {
                this->taskWorkflowActive = true;
                this->workflowStep = 0;
                this->taskWorkflowCompleted = false;
            }
        }
    }

    ControlPanel(lastEvent->text, eventCount, shouldClick);
    CombatWidget(lastSignal->text, signalCount);
    AudioMonitor(lastBroadcast->text, broadcastCount);

    /* Render declarative TUI composable window */
    TuiWindow("CELS Messaging: Events, Signals & Broadcasts", "Profile: 1K");
    TuiPrint("1. Local Tree Events  (cel_listen):   %d handled", *eventCount);
    TuiPrint("   Last: %s", lastEvent->text);
    TuiPrint("2. Targeted Signals   (cel_connect):  %d received", *signalCount);
    TuiPrint("   Last: %s", lastSignal->text);
    TuiPrint("3. Engine Broadcasts  (cel_bind):     %d delivered", *broadcastCount);
    TuiPrint("   Last: %s", lastBroadcast->text);
    TuiDivider();
    if (state->taskWorkflowActive) {
        float stepProgress = (float)(state->workflowStep + 1) * 25.0f;
        TuiPrint("4. Workflow Task: STEP %d/4 ACTIVE", state->workflowStep + 1);
        TuiPrint("   Status: %s", state->workflowStatus[0] ? state->workflowStatus : "Running...");
        TuiProgressBar("Workflow", stepProgress, 100.0f, "");
    } else if (state->taskWorkflowCompleted) {
        TuiLine("4. Workflow Task: COMPLETED");
        TuiPrint("   Status: %s", state->workflowStatus[0] ? state->workflowStatus : "Done");
        TuiProgressBar("Workflow", 100.0f, 100.0f, "DONE");
    } else {
        TuiLine("4. Workflow Task: IDLE (Press [T])");
        TuiProgressBar("Workflow", 0.0f, 100.0f, "IDLE");
    }
    TuiFooter("Controls: [E] Event | [S] Signal | [B] Broadcast | [T] Task | [Q] Quit");

    cel_connect(StartWorkflowSignal, sig) {
        (void)sig;
        cel_mutate(state) {
            this->taskWorkflowActive = true;
            this->workflowStep = 0;
            this->taskWorkflowCompleted = false;
        }
    }

    /* Conditionally mount workflow coordinator task */
    if (state->taskWorkflowActive) {
        cel_task(WorkflowCoordinatorTask, state);
    }
}

CEL_App(EventApp, EventAppComposition);
