#include "window.h"
#include "window_content.h"
#include "common_events.h"
#include "cels_input.h"
#include <stdio.h>

CEL_Evaluation(WindowEval) {
    const WindowState *state = cel_get_state(WindowState);
    return (state == NULL || state->isOpen);
}

typedef enum WindowInputAction {
    WIN_INPUT_TOGGLE_BADGE = 1
} WindowInputAction;

static const CelsKeyBinding g_windowBindings[] = {
    { 'b', WIN_INPUT_TOGGLE_BADGE }
};
static const CelsInputMap g_windowMap = CELS_INPUT_MAP("WindowMap", g_windowBindings);

CEL_Composition(WindowComposition) {
    WindowState *win = cel_remember_state(WindowState, {
        .nativeHandle = (void*)0x12345678,
        .width        = 800,
        .height       = 600,
        .isOpen       = true,
        .showBadge    = true
    });

    /* Route active input mapping down the window tree */
    cel_set_context(CelsInputMap, &g_windowMap);

    if (cel_action_consume(WIN_INPUT_TOGGLE_BADGE)) {
        cel_mutate(win) {
            this->showBadge = !this->showBadge;
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
