#include "window.h"
#include "window_content.h"
#include <stdio.h>

/* ========================================================================= */
/* Root Composition & Evaluation Predicate                                   */
/* ========================================================================= */

CEL_Evaluation(WindowEval, void*, ctx) {
    (void)ctx;
    const WindowState *state = cel_get_state(CEL_Window, WindowState);
    return (state == NULL || state->isOpen);
}

CEL_Composition(WindowComposition, void *userData) {
    (void)userData;
    WindowState *win = cel_remember_state(CEL_Window, WindowState, ((WindowState){
        .isOpen = true,
        .showBadge = true,
        .width  = 800,
        .height = 600,
        .nativeHandle = (void*)0x12345678
    }));

    CEL_WindowContent(win);
}

CelsCompositionRef Window_GetComposition(void)
{
    return (CelsCompositionRef){
        .key = CEL_Window,
        .body = WindowComposition,
        .userData = NULL,
        .lifecycleEval = WindowEval,
        .evalCtx = NULL
    };
}
