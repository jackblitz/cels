#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Common keyboard event signal dispatched by the shared host to the active session.
 * Handled inside composables via cel_connect(CelsKeySignal, sig).
 */
typedef struct CelsKeySignal {
    int key;
} CelsKeySignal;

#ifdef __cplusplus
}
#endif
