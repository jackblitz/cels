#include "cels.h"
#include "network_state.h"
#include "network_task.h"
#include "tui_renderer.h"
#include "common_events.h"
#include <stdio.h>

/**
 * Composable HUD observing reactive network state.
 * Uses persistent slot memory (cel_remember) to track progress across frames.
 */
CEL_Composable(NetworkHUD, NetworkState*, net) {
    cel_watch(net);
    if (net == NULL) return;

    float progress = 0.0f;
    const char *statusStr = "DISCONNECTED";

    switch (net->status) {
        case NET_DISCONNECTED:
            statusStr = "DISCONNECTED";
            progress = 0.0f;
            break;
        case NET_RESOLVING:
            statusStr = "RESOLVING DNS (Step 1/4)";
            progress = 25.0f;
            break;
        case NET_CONNECTING:
            statusStr = "OPENING TCP SOCKET (Step 2/4)";
            progress = 50.0f;
            break;
        case NET_HANDSHAKE:
            statusStr = "SERVER HANDSHAKE (Step 3/4)";
            progress = 75.0f;
            break;
        case NET_CONNECTED:
            statusStr = "CONNECTED (Steady State)";
            progress = 100.0f;
            break;
        case NET_CANCELLED:
            statusStr = "CANCELLED";
            progress = 0.0f;
            break;
    }

    TuiWindow("CELS Asynchronous Procedural Task", "Profile: 512");
    TuiLine("Task:             NetworkConnectTask");
    TuiPrint("Status:           %s", statusStr);
    TuiPrint("Server:           %s", net->server[0] ? net->server : "None");
    TuiPrint("Packets Received: %u", net->packetsReceived);
    TuiDivider();
    TuiProgressBar("Progress", progress, 100.0f, "");
    TuiFooter("Controls: [C] Connect | [X] Cancel | [Q] Quit");

    /* Conditionally execute task while isConnecting is true.
     * When isConnecting is toggled off (e.g. by user pressing [X]),
     * the task is unmounted and its cancel {} block runs immediately! */
    if (net->isConnecting) {
        cel_task(NetworkConnectTask, net, "game.cels.internal:7777");
    }
}

/**
 * Root composition for the network task application.
 */
CEL_Composition(NetworkAppComposition) {
    NetworkState *net = cel_remember_state(NetworkState, {
        .packetsReceived = 0,
        .pingMs          = 0,
        .status          = NET_DISCONNECTED,
        .server          = {0},
        .isConnecting    = false
    });

    /* 1. React to common keyboard signals from host */
    cel_connect(CelsKeySignal, sig) {
        if (sig->key == 'c' || sig->key == 'C') {
            cel_mutate(net) {
                this->isConnecting = true;
                this->status = NET_RESOLVING;
            }
        } else if (sig->key == 'x' || sig->key == 'X') {
            cel_mutate(net) {
                this->isConnecting = false;
            }
        }
    }

    /* 2. React to external network command signals */
    cel_connect(NetworkCommandSignal, sig) {
        if (sig->command == NET_CMD_CONNECT) {
            cel_mutate(net) {
                this->isConnecting = true;
            }
        } else if (sig->command == NET_CMD_CANCEL) {
            cel_mutate(net) {
                this->isConnecting = false;
            }
        }
    }

    NetworkHUD(net);
}

/**
 * Declarative Application Root.
 */
CEL_App(NetworkApp, NetworkAppComposition);
