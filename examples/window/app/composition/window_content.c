#include "window_content.h"
#include "status_badge.h"
#include <stdio.h>

/**
 * Container composable implementation.
 *
 * Demonstrates:
 * - Persistent local slot memory (cel_remember)
 * - Reactive state observation (cel_watch)
 * - Composing child elements (StatusBadge)
 */
CEL_ComposableDef(WindowContent, WindowState*, win) {
    int *localRenderCount = cel_remember(int, 0);
    (*localRenderCount)++;

    /* Reactively observe the hoisted window state instance */
    cel_watch(win);

    printf("  [Content] Local renders: %d | Window: %dx%d (open: %s, badge: %s)\n",
           *localRenderCount, win->width, win->height,
           win->isOpen ? "true" : "false",
           win->showBadge ? "visible" : "hidden");

    /* Status badge lifecycle is controlled by showBadge, decoupled from window isOpen */
    if (win->showBadge) {
        StatusBadge();
    }
}

