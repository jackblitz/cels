#pragma once

#include "cels.h"

#define CEL_NetworkState CEL_ID("CEL_NetworkState")

typedef enum NetStatus {
    NET_DISCONNECTED,
    NET_RESOLVING,
    NET_CONNECTING,
    NET_HANDSHAKE,
    NET_CONNECTED,
    NET_CANCELLED
} NetStatus;

CEL_State(NetworkState) {
    NetStatus status;
    bool isConnecting;
    int  pingMs;
    char server[64];
    uint32_t packetsReceived;
};
