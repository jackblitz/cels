#pragma once

#include "network_state.h"
#include <stdio.h>
#include <string.h>

/**
 * Multi-step asynchronous network connection task.
 *
 * Demonstrates:
 * - cancel { ... }: Clean socket teardown upon cancellation or unmount.
 * - cel_wait(ms): Non-blocking simulated network latency.
 * - cel_yield(): Yielding per-frame in steady-state loop.
 * - cel_cancel(): Self-cancellation handling.
 */
CEL_Task(NetworkConnectTask, const char*, server, int, port) {
    cancel {
        printf("  [NetworkTask] CANCEL: Teardown triggered! Closing socket to %s:%d...\n", server, port);
        cel_mutate(CelsGetCurrentSession(), CEL_NetworkState, NetworkState) {
            this->status = NET_CANCELLED;
            this->isConnecting = false;
        }
    }

    run {
        /* Step 1: DNS Resolution */
        printf("  [NetworkTask] Step 1: Resolving hostname '%s'...\n", server);
        cel_mutate(CelsGetCurrentSession(), CEL_NetworkState, NetworkState) {
            this->status = NET_RESOLVING;
        }
        cel_wait(200); /* 200ms non-blocking DNS delay */

        /* Step 2: Socket Connection */
        printf("  [NetworkTask] Step 2: Connecting TCP socket to port %d...\n", port);
        cel_mutate(CelsGetCurrentSession(), CEL_NetworkState, NetworkState) {
            this->status = NET_CONNECTING;
        }
        cel_wait(150); /* 150ms connection handshake */

        /* Step 3: Server Handshake & Auth */
        printf("  [NetworkTask] Step 3: Sending handshake & authentication token...\n");
        cel_mutate(CelsGetCurrentSession(), CEL_NetworkState, NetworkState) {
            this->status = NET_HANDSHAKE;
        }
        cel_wait(250); /* 250ms auth roundtrip */

        /* Step 4: Successfully Connected! */
        printf("  [NetworkTask] Step 4: Handshake approved! Session active on %s:%d.\n", server, port);
        cel_mutate(CelsGetCurrentSession(), CEL_NetworkState, NetworkState) {
            this->status = NET_CONNECTED;
            this->pingMs = 24;
            this->packetsReceived = 0;
            strncpy(this->server, server, sizeof(this->server) - 1);
        }

        /* Step 5: Ongoing Packet Pump */
        while (1) {
            cel_wait(100); /* Simulate packet arrival every 100ms */
            cel_mutate(CelsGetCurrentSession(), CEL_NetworkState, NetworkState) {
                this->packetsReceived++;
                this->pingMs = 20 + (int)(this->packetsReceived % 10);
            }
        }
    }
}
