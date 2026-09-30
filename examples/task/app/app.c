#include "cels.h"
#include "network_state.h"
#include "network_task.h"
#include <stdio.h>

/**
 * Composable HUD observing reactive network state.
 * Uses persistent slot memory (cel_remember) to track previous status across frames.
 */
CEL_Composable(NetworkHUD, NetworkState*, net) {
    cel_watch(net);
    if (net == NULL) return;

    switch (net->status) {
        case NET_DISCONNECTED:
            printf("[HUD] Status: DISCONNECTED. Press [C] to connect.\n");
            break;
        case NET_RESOLVING:
            printf("[HUD] Status: Resolving DNS...\n");
            break;
        case NET_CONNECTING:
            printf("[HUD] Status: Opening TCP socket...\n");
            break;
        case NET_HANDSHAKE:
            printf("[HUD] Status: Authenticating...\n");
            break;
        case NET_CONNECTED:
            printf("[HUD] Status: CONNECTED | Server: %s | Ping: %d ms | Packets: %u\n",
                   net->server, net->pingMs, net->packetsReceived);
            break;
        case NET_CANCELLED:
            printf("[HUD] Status: CANCELLED. Ready to reconnect ([C]).\n");
            break;
    }

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

    NetworkHUD(net);
}

/**
 * Declarative Application Root.
 */
CEL_App(NetworkApp, NetworkAppComposition);

