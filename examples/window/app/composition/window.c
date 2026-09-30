#include "window.h"
#include "window_content.h"
#include <stdio.h>

CEL_Evaluation(WindowEval) {
    const WindowState *state = cel_get_state(WindowState);
    return (state == NULL || state->isOpen);
}

CEL_Composition(WindowComposition) {
    WindowState *win = cel_remember_state(WindowState, {
        .nativeHandle = (void*)0x12345678,
        .width        = 800,
        .height       = 600,
        .isOpen       = true,
        .showBadge    = true
    });

    WindowContent(win);
}

