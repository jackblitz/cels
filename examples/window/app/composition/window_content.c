#include "window_content.h"
#include "status_badge.h"
#include "tui_renderer.h"
#include <stdio.h>
#include <string.h>

typedef struct LastEventMsg {
    char text[64];
} LastEventMsg;

/**
 * Container composable implementation.
 *
 * Demonstrates:
 * - Persistent local slot memory (cel_remember)
 * - Reactive state observation (cel_watch)
 * - In-place terminal window rendering
 * - Child component lifecycle & discrete event bubbling
 */
CEL_ComposableDef(WindowContent, WindowState*, win) {
    int *localRenderCount = cel_remember(int, 0);
    (*localRenderCount)++;

    LastEventMsg *lastEvent = cel_remember(LastEventMsg, { .text = "No events received yet" });

    /* Reactively observe the hoisted window state instance */
    cel_watch(win);

    /* Listen for discrete events bubbling up from child components */
    cel_listen(BadgeNotificationEvent, ev) {
        snprintf(lastEvent->text, sizeof(lastEvent->text), "%s (badge #%d)", ev->message, ev->badgeId);
    }

    /* Render declarative TUI window */
    TuiWindow("CELS Window Component", "Profile: 1K");
    TuiPrint("Dimensions:   %d x %d px", win->width, win->height);
    TuiPrint("Window State: %s", win->isOpen ? "OPEN" : "CLOSED");
    TuiPrint("Render Pass:  #%d", *localRenderCount);
    TuiPrint("Status Badge: %s", win->showBadge ? "[ ACTIVE / MOUNTED ]" : "[ HIDDEN / UNMOUNTED ]");
    TuiDivider();
    TuiLine("Child Event Bubbling (cel_listen):");
    TuiPrint("  %s", lastEvent->text);
    TuiFooter("Controls: [B] Toggle Status Badge | [Q] Quit");

    /* Status badge lifecycle is controlled by showBadge */
    if (win->showBadge) {
        StatusBadge();
    }
}
