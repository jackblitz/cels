#pragma once

#include "cels.h"


/**
 * Connection lifecycle states for the asynchronous network task.
 */
typedef enum NetStatus {
    NET_DISCONNECTED,
    NET_RESOLVING,
    NET_CONNECTING,
    NET_HANDSHAKE,
    NET_CONNECTED,
    NET_CANCELLED
} NetStatus;

/**
 * Reactive network state structure.
 * Fields are ordered in descending alignment to eliminate struct padding.
 */
CEL_State(NetworkState) {
    uint32_t  packetsReceived;
    int32_t   pingMs;
    NetStatus status;
    char      server[64];
    bool      isConnecting;
};

