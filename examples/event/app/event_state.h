#pragma once

#include "cels.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * 1. Local Tree Event: Emitted by a child composable and bubbles up to ancestor composables.
 */
typedef struct UserActionEvent {
    int  actionId;
    int  cost;
    char actionName[32];
} UserActionEvent;

/**
 * 2. Targeted Session Signal: Sent from an external session, thread, or system
 *    directly into a specific target session's inbox.
 */
typedef struct CombatSignal {
    int   targetId;
    float damage;
    bool  isCritical;
} CombatSignal;

typedef struct StartWorkflowSignal {
    bool active;
} StartWorkflowSignal;

/**
 * 3. Engine-Wide Broadcast: Dispatched over the global thread-safe bus to
 *    all sessions and background workers across the engine.
 */
typedef struct AudioBroadcast {
    char  soundEffect[32];
    float volume;
} AudioBroadcast;

/**
 * Reactive application state model tracking activity metrics across all communication scopes.
 */
CEL_State(EventDemoState) {
    int  treeEventsHandled;
    int  sessionSignalsHandled;
    int  broadcastsHandled;
    int  workflowStep;
    char workflowStatus[64];
    bool taskWorkflowActive;
    bool taskWorkflowCompleted;
};
