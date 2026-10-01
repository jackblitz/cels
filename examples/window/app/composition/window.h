#pragma once

#include "cels.h"


/**
 * Reactive Window State Definition.
 * Fields are ordered in descending alignment to eliminate struct padding.
 */
CEL_State(WindowState) {
    void    *nativeHandle;
    int32_t  width;
    int32_t  height;
    bool     isOpen;
    bool     showBadge;
};

typedef enum WindowAction {
    WINDOW_ACTION_TOGGLE_BADGE,
    WINDOW_ACTION_CLOSE
} WindowAction;

typedef struct WindowActionSignal {
    WindowAction action;
} WindowActionSignal;

/**
 * Evaluates whether the window composition should remain active.
 * Returning false triggers composition teardown.
 */
bool WindowEval(void *ctx);

/**
 * Window root composition function.
 * Attaches the window layout hierarchy to the session tree.
 */
void WindowComposition(void *userData);

