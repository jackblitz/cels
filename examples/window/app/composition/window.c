#include "window.h"
#include "window_content.h"
#include "common_events.h"
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

    /* 1. React to common keyboard signals from host */
    cel_connect(CelsKeySignal, sig) {
        if (sig->key == 'b' || sig->key == 'B') {
            cel_mutate(win) {
                this->showBadge = !this->showBadge;
            }
        }
    }

    /* 2. React to external window command signals */
    cel_connect(WindowActionSignal, sig) {
        if (sig->action == WINDOW_ACTION_TOGGLE_BADGE) {
            cel_mutate(win) {
                this->showBadge = !this->showBadge;
            }
        } else if (sig->action == WINDOW_ACTION_CLOSE) {
            cel_mutate(win) {
                this->isOpen = false;
            }
        }
    }

    WindowContent(win);
}
