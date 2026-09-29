#include "window_content.h"
#include "status_badge.h"
#include <stdio.h>

/**
 * Container composable implementation.
 *
 * Demonstrates:
 * - Persistent local slot memory (cel_remember)
 * - Reactive state observation (cel_watch)
 * - Composing child elements (CEL_StatusBadge)
 */
CEL_ComposableDef(CEL_WindowContent, WindowState*, win) {
    int *localRenderCount = cel_remember(int, 0);
    (*localRenderCount)++;

    const WindowState *state = cel_watch(WindowState, CEL_Window);
    if (state == NULL) {
        state = win;
    }

    printf("  [Content] Local renders: %d | Window: %dx%d (open: %s, badge: %s)\n",
           *localRenderCount, state->width, state->height,
           state->isOpen ? "true" : "false",
           state->showBadge ? "visible" : "hidden");

    /* Status badge lifecycle is controlled by showBadge, decoupled from window isOpen */
    if (state->showBadge) {
        CEL_StatusBadge();
    }
}
