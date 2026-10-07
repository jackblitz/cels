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
 * - cel_mutate(net): Direct pointer mutation of reactive state from cooperative fibers.
 */
CEL_Task(NetworkConnectTask, NetworkState*, net, const char*, server) {
    cancel {
        cel_mutate(net) {
            this->status = NET_CANCELLED;
            this->isConnecting = false;
        }
    }

    run {
        /* Step 1: DNS Resolution */
        cel_mutate(net) {
            this->status = NET_RESOLVING;
        }
        cel_wait(200); /* 200ms non-blocking DNS delay */

        /* Step 2: Socket Connection */
        cel_mutate(net) {
            this->status = NET_CONNECTING;
        }
        cel_wait(150); /* 150ms connection handshake */

        /* Step 3: Server Handshake & Auth */
        cel_mutate(net) {
            this->status = NET_HANDSHAKE;
        }
        cel_wait(250); /* 250ms auth roundtrip */

        /* Step 4: Successfully Connected */
        cel_mutate(net) {
            this->packetsReceived = 0;
            this->pingMs = 24;
            this->status = NET_CONNECTED;
            strncpy(this->server, server, sizeof(this->server) - 1);
            this->server[sizeof(this->server) - 1] = '\0';
        }

        /* Step 5: Ongoing Packet Pump */
        while (1) {
            cel_wait(100); /* Simulate packet arrival every 100ms */
            cel_mutate(net) {
                this->packetsReceived++;
                this->pingMs = 20 + (int)(this->packetsReceived % 10);
            }
        }
    }
}


