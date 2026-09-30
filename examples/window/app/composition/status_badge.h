#pragma once

#include "cels.h"
#include <stdio.h>

/**
 * Data associated with the status badge component.
 */
typedef struct BadgeData {
    const char *label;
} BadgeData;

/**
 * Child lifecycle demonstrating pure mount / unmount topology tracking.
 * Mounts when showBadge is enabled, and unmounts when showBadge is toggled off,
 * all while the root window remains open.
 */
CEL_Lifecycle(StatusBadgeLifecycle, BadgeData *badge) {
    mount {
        printf("    [BadgeLifecycle] MOUNT: Status badge mounted ('%s')\n", badge->label);
    }
    unmount {
        printf("    [BadgeLifecycle] UNMOUNT: Status badge unmounted\n");
    }
}

/**
 * Leaf composable rendering a badge when showBadge is enabled.
 */
CEL_Composable(StatusBadge) {
    BadgeData *badge = cel_remember(BadgeData, { .label = "Connected / Active" });
    cel_lifecycle(StatusBadgeLifecycle, badge);
    printf("    [Badge] Status: %s\n", badge->label);
}



