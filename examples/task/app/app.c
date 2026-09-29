#include "cels.h"
#include "network_state.h"
#include "network_task.h"
#include <stdio.h>

#define CEL_NetworkApp CEL_ID("CEL_NetworkApp")

/* ========================================================================= */
/* Composable UI / HUD                                                       */
/* ========================================================================= */

CEL_Composable(NetworkHUD) {
    const NetworkState *net = cel_watch(NetworkState, CEL_NetworkState);
    if (net == NULL) return;

    static NetStatus lastReportedStatus = -1;
    static uint32_t lastReportedPackets = 0;

    if (net->status != lastReportedStatus || (net->status == NET_CONNECTED && net->packetsReceived != lastReportedPackets)) {
        lastReportedStatus = net->status;
        lastReportedPackets = net->packetsReceived;

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
    }

    /* Conditionally execute task while isConnecting is true.
     * When isConnecting is toggled off (e.g. by user pressing [X]),
     * the task is unmounted and its cancel {} block runs immediately! */
    if (net->isConnecting) {
        cel_task(NetworkConnectTask, "game.cels.internal", 7777);
    }
}

/* ========================================================================= */
/* Root Composition                                                          */
/* ========================================================================= */

CEL_Composition(NetworkAppComposition, void *userData) {
    (void)userData;
    cel_remember_state(CEL_NetworkState, NetworkState, ((NetworkState){
        .status = NET_DISCONNECTED,
        .isConnecting = false,
        .pingMs = 0,
        .packetsReceived = 0,
        .server = {0}
    }));

    NetworkHUD();
}

static CelsCompositionRef App_OnStart(CelsEngine *engine, CelsSession *session) {
    (void)engine;
    (void)session;
    printf("[App] Network Task Application started.\n");
    return (CelsCompositionRef){
        .key = CEL_NetworkApp,
        .body = NetworkAppComposition,
        .userData = NULL,
        .lifecycleEval = NULL,
        .evalCtx = NULL
    };
}

static void App_OnEnd(CelsEngine *engine, CelsSession *session) {
    (void)engine;
    (void)session;
    printf("[App] Network Task Application teardown complete.\n");
}

/* ========================================================================= */
/* Application Manifest                                                      */
/* ========================================================================= */

CEL_App(NetworkApp,
    .onStart = App_OnStart,
    .onEnd   = App_OnEnd
);
